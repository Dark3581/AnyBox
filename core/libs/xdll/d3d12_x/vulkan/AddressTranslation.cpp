#include "AddressTranslation.hpp"

#include <map>
#include <set>
#include <unordered_map>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::size_t kHeaderWords = 5;

enum Op : std::uint32_t {
    OpDecorate = 71,
    OpMemberDecorate = 72,
    OpTypeInt = 21,
    OpTypeStruct = 30,
    OpTypePointer = 32,
    OpTypeFunction = 33,
    OpConstant = 43,
    OpFunction = 54,
    OpFunctionParameter = 55,
    OpFunctionEnd = 56,
    OpFunctionCall = 57,
    OpVariable = 59,
    OpLoad = 61,
    OpAccessChain = 65,
    OpUConvert = 113,
    OpConvertUToPtr = 120,
    OpIAdd = 128,
    OpShiftRightLogical = 194,
    OpShiftLeftLogical = 196,
    OpBitwiseOr = 197,
    OpBitwiseAnd = 199,
    OpLabel = 248,
    OpReturnValue = 254,
};

constexpr std::uint32_t kDecorationOffset = 35;
constexpr std::uint32_t kStoragePushConstant = 9;
constexpr std::uint32_t kStoragePhysicalStorageBuffer = 5349;
constexpr std::uint32_t kMemoryAccessAligned = 2;
constexpr std::uint64_t kRegionMask = 0x7FFF;
constexpr std::uint64_t kPageMask = 0xFFFF;

struct Instruction {
    std::size_t at = 0;
    std::uint32_t opcode = 0;
    std::uint32_t count = 0;
};

class Module {
public:
    explicit Module(std::span<const std::uint32_t> words) : words_(words.begin(), words.end()) {}

    bool Parse(std::string& error) {
        if (words_.size() < kHeaderWords || words_[0] != kMagic) {
            error = "not a SPIR-V module";
            return false;
        }
        for (std::size_t at = kHeaderWords; at < words_.size();) {
            const std::uint32_t count = words_[at] >> 16;
            if (count == 0 || at + count > words_.size()) {
                error = "a SPIR-V instruction runs past the end of the module";
                return false;
            }
            instructions_.push_back({at, words_[at] & 0xFFFFu, count});
            at += count;
        }
        return true;
    }

    std::uint32_t Word(const Instruction& instruction, std::size_t index) const { return words_[instruction.at + index]; }
    std::vector<std::uint32_t>& Words() { return words_; }
    const std::vector<Instruction>& Instructions() const { return instructions_; }

private:
    std::vector<std::uint32_t> words_;
    std::vector<Instruction> instructions_;
};

struct Definition {
    std::uint32_t opcode = 0;
    std::vector<std::uint32_t> operands;
};

class Emitter {
public:
    explicit Emitter(std::uint32_t& bound) : bound_(bound) {}

    std::uint32_t Id() { return bound_++; }

    void Emit(std::vector<std::uint32_t>& out, std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) {
        out.push_back(static_cast<std::uint32_t>((operands.size() + 1) << 16) | opcode);
        out.insert(out.end(), operands.begin(), operands.end());
    }

private:
    std::uint32_t& bound_;
};

}

std::optional<AddressTranslation> TranslateTitleAddresses(std::span<const std::uint32_t> words, std::string& error) {
    Module module(words);
    if (!module.Parse(error)) return std::nullopt;
    std::uint32_t uintType = 0;
    std::uint32_t ulongType = 0;
    std::uint32_t pushVariable = 0;
    std::uint32_t pushStruct = 0;
    std::uint32_t pushUintPointer = 0;
    std::uint32_t psbUlongPointer = 0;
    std::uint32_t functionType = 0;
    std::size_t firstFunction = 0;
    std::map<std::uint32_t, std::uint32_t> pointerPointee;
    std::map<std::uint32_t, std::uint32_t> pointerStorage;
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint32_t>> memberOffsets;
    std::unordered_map<std::uint32_t, std::uint64_t> constants;
    std::unordered_map<std::uint32_t, Definition> definitions;
    std::vector<const Instruction*> additions;
    for (const Instruction& instruction : module.Instructions()) {
        const auto word = [&](std::size_t index) { return module.Word(instruction, index); };
        switch (instruction.opcode) {
        case OpTypeInt:
            if (word(3) == 0 && word(2) == 32) uintType = word(1);
            if (word(3) == 0 && word(2) == 64) ulongType = word(1);
            break;
        case OpTypePointer:
            pointerStorage[word(1)] = word(2);
            pointerPointee[word(1)] = word(3);
            break;
        case OpMemberDecorate:
            if (instruction.count >= 5 && word(3) == kDecorationOffset) memberOffsets[word(1)][word(2)] = word(4);
            break;
        case OpConstant:
            if (instruction.count == 4) constants[word(2)] = word(3);
            if (instruction.count == 5) constants[word(2)] = word(3) | (static_cast<std::uint64_t>(word(4)) << 32);
            break;
        case OpVariable:
            if (word(3) == kStoragePushConstant) {
                if (pushVariable != 0) {
                    error = "the module declares more than one push constant block";
                    return std::nullopt;
                }
                pushVariable = word(2);
                pushStruct = pointerPointee[word(1)];
            }
            break;
        case OpTypeFunction:
            if (instruction.count == 4 && ulongType != 0 && word(2) == ulongType && word(3) == ulongType) functionType = word(1);
            break;
        case OpFunction:
            if (firstFunction == 0) firstFunction = instruction.at;
            break;
        case OpLoad:
        case OpAccessChain:
        case OpUConvert:
        case OpShiftLeftLogical:
        case OpBitwiseOr: {
            Definition definition{instruction.opcode, {}};
            for (std::uint32_t index = 1; index < instruction.count; ++index) definition.operands.push_back(word(index));
            definitions[word(2)] = std::move(definition);
            break;
        }
        case OpIAdd:
            additions.push_back(&instruction);
            break;
        default:
            break;
        }
    }
    for (const auto& [pointer, storage] : pointerStorage) {
        if (storage == kStoragePushConstant && pointerPointee[pointer] == uintType && pushUintPointer == 0) pushUintPointer = pointer;
        if (storage == kStoragePhysicalStorageBuffer && pointerPointee[pointer] == ulongType && psbUlongPointer == 0) psbUlongPointer = pointer;
    }
    if (pushVariable == 0 || ulongType == 0) {
        AddressTranslation unchanged;
        unchanged.words.assign(words.begin(), words.end());
        return unchanged;
    }
    if (uintType == 0 || firstFunction == 0) {
        error = "the module has a push constant block and 64-bit integers but no 32-bit integer type or no function";
        return std::nullopt;
    }
    const auto offsets = memberOffsets.find(pushStruct);
    if (offsets == memberOffsets.end() || offsets->second[kHeapBaseLowMember] != kHeapBaseLowOffset || offsets->second[kHeapBaseHighMember] != kHeapBaseHighOffset) {
        error = "the push constant block does not hold the heap base at bytes 128..135";
        return std::nullopt;
    }
    const auto pushMember = [&](std::uint32_t value) -> std::optional<std::uint64_t> {
        const auto convert = definitions.find(value);
        if (convert == definitions.end() || convert->second.opcode != OpUConvert) return std::nullopt;
        const auto load = definitions.find(convert->second.operands[2]);
        if (load == definitions.end() || load->second.opcode != OpLoad) return std::nullopt;
        const auto chain = definitions.find(load->second.operands[2]);
        if (chain == definitions.end() || chain->second.opcode != OpAccessChain || chain->second.operands.size() != 4 || chain->second.operands[2] != pushVariable) {
            return std::nullopt;
        }
        const auto index = constants.find(chain->second.operands[3]);
        if (index == constants.end()) return std::nullopt;
        return index->second;
    };
    bool usesCodeBase = false;
    for (const auto& [result, definition] : definitions) {
        if (definition.opcode != OpAccessChain || definition.operands.size() != 4 || definition.operands[2] != pushVariable) continue;
        const auto index = constants.find(definition.operands[3]);
        if (index != constants.end() && (index->second == kCodeBaseLowMember || index->second == kCodeBaseHighMember)) usesCodeBase = true;
    }
    std::set<std::uint32_t> heapBases;
    for (const auto& [result, definition] : definitions) {
        if (definition.opcode != OpBitwiseOr || definition.operands[0] != ulongType) continue;
        for (const auto [shifted, other] : {std::pair{2, 3}, std::pair{3, 2}}) {
            const auto shift = definitions.find(definition.operands[shifted]);
            if (shift == definitions.end() || shift->second.opcode != OpShiftLeftLogical) continue;
            const auto amount = constants.find(shift->second.operands[3]);
            if (amount == constants.end() || amount->second != 32) continue;
            if (pushMember(shift->second.operands[2]) == kHeapBaseHighMember && pushMember(definition.operands[other]) == kHeapBaseLowMember) heapBases.insert(result);
        }
    }
    std::uint32_t bound = module.Words()[3];
    Emitter emit(bound);
    const std::uint32_t translate = emit.Id();
    AddressTranslation translated;
    translated.usesCodeBase = usesCodeBase;
    std::set<std::size_t> rewrittenAt;
    std::vector<std::uint32_t>& out = module.Words();
    for (const Instruction* addition : additions) {
        const std::uint32_t type = module.Word(*addition, 1);
        const std::uint32_t left = module.Word(*addition, 3);
        const std::uint32_t right = module.Word(*addition, 4);
        if (type != ulongType || addition->count != 5) continue;
        const bool leftBase = heapBases.count(left) != 0;
        const bool rightBase = heapBases.count(right) != 0;
        if (leftBase == rightBase) continue;
        out[addition->at] = (5u << 16) | OpFunctionCall;
        out[addition->at + 3] = translate;
        out[addition->at + 4] = leftBase ? right : left;
        ++translated.rewritten;
        rewrittenAt.insert(addition->at);
    }
    std::size_t heapMemberChains = 0;
    for (const Instruction& instruction : module.Instructions()) {
        if (rewrittenAt.count(instruction.at) != 0) continue;
        if (instruction.opcode == OpBitwiseOr && instruction.count > 2 && heapBases.count(words[instruction.at + 2]) != 0) continue;
        if (instruction.opcode == OpAccessChain && instruction.count == 5 && words[instruction.at + 3] == pushVariable) {
            const auto index = constants.find(words[instruction.at + 4]);
            if (index != constants.end() && (index->second == kHeapBaseLowMember || index->second == kHeapBaseHighMember)) ++heapMemberChains;
        }
        for (std::uint32_t index = 1; index < instruction.count; ++index) {
            if (heapBases.count(words[instruction.at + index]) != 0) ++translated.unrecognizedHeapUses;
        }
    }
    if (heapBases.empty()) translated.unrecognizedHeapUses += heapMemberChains;
    if (translated.rewritten == 0) {
        translated.words.assign(words.begin(), words.end());
        return translated;
    }
    std::vector<std::uint32_t> declarations;
    if (functionType == 0) {
        functionType = emit.Id();
        emit.Emit(declarations, OpTypeFunction, {functionType, ulongType, ulongType});
    }
    if (pushUintPointer == 0) {
        pushUintPointer = emit.Id();
        emit.Emit(declarations, OpTypePointer, {pushUintPointer, kStoragePushConstant, uintType});
    }
    if (psbUlongPointer == 0) {
        psbUlongPointer = emit.Id();
        emit.Emit(declarations, OpTypePointer, {psbUlongPointer, kStoragePhysicalStorageBuffer, ulongType});
    }
    const auto uintConstant = [&](std::uint32_t value) {
        const std::uint32_t id = emit.Id();
        emit.Emit(declarations, OpConstant, {uintType, id, value});
        return id;
    };
    const auto ulongConstant = [&](std::uint64_t value) {
        const std::uint32_t id = emit.Id();
        emit.Emit(declarations, OpConstant, {ulongType, id, static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(value >> 32)});
        return id;
    };
    const std::uint32_t lowIndex = uintConstant(kHeapBaseLowMember);
    const std::uint32_t highIndex = uintConstant(kHeapBaseHighMember);
    const std::uint32_t thirtyTwo = ulongConstant(32);
    const std::uint32_t sixteen = ulongConstant(16);
    const std::uint32_t three = ulongConstant(3);
    const std::uint32_t regionMask = ulongConstant(kRegionMask);
    const std::uint32_t pageMask = ulongConstant(kPageMask);

    std::vector<std::uint32_t> function;
    const std::uint32_t address = emit.Id();
    emit.Emit(function, OpFunction, {ulongType, translate, 0, functionType});
    emit.Emit(function, OpFunctionParameter, {ulongType, address});
    emit.Emit(function, OpLabel, {emit.Id()});
    const auto op = [&](std::uint32_t opcode, std::uint32_t type, std::initializer_list<std::uint32_t> operands) {
        const std::uint32_t id = emit.Id();
        std::vector<std::uint32_t> all{type, id};
        all.insert(all.end(), operands.begin(), operands.end());
        function.push_back(static_cast<std::uint32_t>((all.size() + 1) << 16) | opcode);
        function.insert(function.end(), all.begin(), all.end());
        return id;
    };
    const std::uint32_t lowPointer = op(OpAccessChain, pushUintPointer, {pushVariable, lowIndex});
    const std::uint32_t highPointer = op(OpAccessChain, pushUintPointer, {pushVariable, highIndex});
    const std::uint32_t low = op(OpUConvert, ulongType, {op(OpLoad, uintType, {lowPointer})});
    const std::uint32_t high = op(OpUConvert, ulongType, {op(OpLoad, uintType, {highPointer})});
    const std::uint32_t root = op(OpBitwiseOr, ulongType, {op(OpShiftLeftLogical, ulongType, {high, thirtyTwo}), low});
    const std::uint32_t region = op(OpBitwiseAnd, ulongType, {op(OpShiftRightLogical, ulongType, {address, thirtyTwo}), regionMask});
    const std::uint32_t regionEntry = op(OpIAdd, ulongType, {root, op(OpShiftLeftLogical, ulongType, {region, three})});
    const std::uint32_t table = op(OpLoad, ulongType, {op(OpConvertUToPtr, psbUlongPointer, {regionEntry}), kMemoryAccessAligned, 8});
    const std::uint32_t page = op(OpBitwiseAnd, ulongType, {op(OpShiftRightLogical, ulongType, {address, sixteen}), pageMask});
    const std::uint32_t pageEntry = op(OpIAdd, ulongType, {table, op(OpShiftLeftLogical, ulongType, {page, three})});
    const std::uint32_t base = op(OpLoad, ulongType, {op(OpConvertUToPtr, psbUlongPointer, {pageEntry}), kMemoryAccessAligned, 8});
    const std::uint32_t result = op(OpIAdd, ulongType, {base, op(OpBitwiseAnd, ulongType, {address, pageMask})});
    emit.Emit(function, OpReturnValue, {result});
    emit.Emit(function, OpFunctionEnd, {});

    translated.words.reserve(out.size() + declarations.size() + function.size());
    translated.words.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(firstFunction));
    translated.words.insert(translated.words.end(), declarations.begin(), declarations.end());
    translated.words.insert(translated.words.end(), out.begin() + static_cast<std::ptrdiff_t>(firstFunction), out.end());
    translated.words.insert(translated.words.end(), function.begin(), function.end());
    translated.words[3] = bound;
    return translated;
}

}
