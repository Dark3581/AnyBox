#include "ThreadLimit.hpp"

#include <cstddef>
#include <map>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::size_t kHeaderWords = 5;
constexpr std::uint32_t kOpEntryPoint = 15;
constexpr std::uint32_t kOpExecutionMode = 16;
constexpr std::uint32_t kOpTypeVoid = 19;
constexpr std::uint32_t kOpTypeBool = 20;
constexpr std::uint32_t kOpTypeInt = 21;
constexpr std::uint32_t kOpTypeVector = 23;
constexpr std::uint32_t kOpTypeArray = 28;
constexpr std::uint32_t kOpTypeStruct = 30;
constexpr std::uint32_t kOpTypePointer = 32;
constexpr std::uint32_t kOpConstant = 43;
constexpr std::uint32_t kOpFunction = 54;
constexpr std::uint32_t kOpVariable = 59;
constexpr std::uint32_t kOpLoad = 61;
constexpr std::uint32_t kOpAccessChain = 65;
constexpr std::uint32_t kOpDecorate = 71;
constexpr std::uint32_t kOpMemberDecorate = 72;
constexpr std::uint32_t kOpCompositeExtract = 81;
constexpr std::uint32_t kOpULessThan = 176;
constexpr std::uint32_t kOpSelectionMerge = 247;
constexpr std::uint32_t kOpLabel = 248;
constexpr std::uint32_t kOpBranchConditional = 250;
constexpr std::uint32_t kOpReturn = 253;
constexpr std::uint32_t kOpExecutionModeId = 331;
constexpr std::uint32_t kModelCompute = 5;
constexpr std::uint32_t kModeLocalSize = 17;
constexpr std::uint32_t kModeLocalSizeId = 38;
constexpr std::uint32_t kDecorationBuiltIn = 11;
constexpr std::uint32_t kBuiltInWorkgroupSize = 25;
constexpr std::uint32_t kBuiltInGlobalInvocationId = 28;
constexpr std::uint32_t kStorageInput = 1;
constexpr std::uint32_t kStoragePushConstant = 9;

struct Instruction {
    std::size_t at = 0;
    std::uint32_t op = 0;
    std::uint32_t count = 0;
};

bool Walk(const std::vector<std::uint32_t>& words, std::vector<Instruction>& out, std::string& error) {
    if (words.size() < kHeaderWords || words[0] != kMagic) {
        error = "not a SPIR-V module";
        return false;
    }
    for (std::size_t at = kHeaderWords; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size()) {
            error = "a malformed instruction";
            return false;
        }
        out.push_back({at, words[at] & 0xFFFFu, count});
        at += count;
    }
    return true;
}

std::size_t ComputeEntry(const std::vector<std::uint32_t>& words, const std::vector<Instruction>& all, std::string& error) {
    std::size_t found = all.size();
    for (std::size_t index = 0; index < all.size(); ++index) {
        const Instruction& i = all[index];
        if (i.op != kOpEntryPoint || i.count < 4 || words[i.at + 1] != kModelCompute) continue;
        if (found != all.size()) {
            error = "more than one compute entry point";
            return all.size();
        }
        found = index;
    }
    if (found == all.size()) error = "no compute entry point";
    return found;
}

void Emit(std::vector<std::uint32_t>& out, std::uint32_t op, std::initializer_list<std::uint32_t> operands) {
    out.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | op);
    out.insert(out.end(), operands.begin(), operands.end());
}

}

std::optional<std::array<std::uint32_t, 3>> ComputeGroupSize(const std::vector<std::uint32_t>& words, std::string& error) {
    std::vector<Instruction> all;
    if (!Walk(words, all, error)) return std::nullopt;
    const std::size_t entryIndex = ComputeEntry(words, all, error);
    if (entryIndex == all.size()) return std::nullopt;
    const std::uint32_t entry = words[all[entryIndex].at + 2];
    std::optional<std::array<std::uint32_t, 3>> size;
    for (const Instruction& i : all) {
        if (i.op == kOpExecutionModeId && i.count >= 3 && words[i.at + 1] == entry && words[i.at + 2] == kModeLocalSizeId) {
            error = "a thread group size given by constant ids";
            return std::nullopt;
        }
        if (i.op == kOpDecorate && i.count == 4 && words[i.at + 2] == kDecorationBuiltIn && words[i.at + 3] == kBuiltInWorkgroupSize) {
            error = "a thread group size given by a WorkgroupSize constant";
            return std::nullopt;
        }
        if (i.op == kOpExecutionMode && i.count == 6 && words[i.at + 1] == entry && words[i.at + 2] == kModeLocalSize) {
            size = std::array<std::uint32_t, 3>{words[i.at + 3], words[i.at + 4], words[i.at + 5]};
        }
    }
    if (!size) {
        error = "no LocalSize execution mode on the compute entry point";
        return std::nullopt;
    }
    if ((*size)[0] == 0 || (*size)[1] == 0 || (*size)[2] == 0) {
        error = "a thread group size of 0";
        return std::nullopt;
    }
    return size;
}

bool LimitThreads(std::vector<std::uint32_t>& words, std::uint32_t limitRegister, std::string& error) {
    std::vector<Instruction> all;
    if (!Walk(words, all, error)) return false;
    const std::size_t entryIndex = ComputeEntry(words, all, error);
    if (entryIndex == all.size()) return false;
    const std::uint32_t entry = words[all[entryIndex].at + 2];

    std::uint32_t typeVoid = 0, typeBool = 0, typeUint = 0, typeVector = 0, pointerInput = 0, pointerPush = 0;
    std::uint32_t pushVariable = 0, pushPointerType = 0, globalId = 0, zero = 0, index = 0;
    std::map<std::uint32_t, std::uint32_t> pointee;
    std::map<std::uint32_t, std::uint32_t> firstMember;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> arrays;
    std::map<std::uint32_t, std::uint32_t> uintConstants;
    std::map<std::uint32_t, std::uint32_t> functionResult;
    std::size_t firstAnnotation = all.size(), firstFunction = all.size(), entryFunction = all.size();
    for (std::size_t n = 0; n < all.size(); ++n) {
        const Instruction& i = all[n];
        const std::uint32_t* w = &words[i.at];
        switch (i.op) {
        case kOpTypeVoid: typeVoid = w[1]; break;
        case kOpTypeBool: typeBool = w[1]; break;
        case kOpTypeInt:
            if (i.count == 4 && w[2] == 32 && w[3] == 0) typeUint = w[1];
            break;
        case kOpTypeVector:
            if (i.count == 4 && typeUint != 0 && w[2] == typeUint && w[3] == 3) typeVector = w[1];
            break;
        case kOpTypeArray:
            if (i.count == 4) arrays[w[1]] = {w[2], w[3]};
            break;
        case kOpTypeStruct:
            if (i.count >= 3) firstMember[w[1]] = w[2];
            break;
        case kOpTypePointer:
            if (i.count == 4) {
                pointee[w[1]] = w[3];
                if (w[2] == kStorageInput && typeVector != 0 && w[3] == typeVector) pointerInput = w[1];
                if (w[2] == kStoragePushConstant && typeUint != 0 && w[3] == typeUint) pointerPush = w[1];
            }
            break;
        case kOpConstant:
            if (i.count == 4 && typeUint != 0 && w[1] == typeUint) {
                uintConstants[w[2]] = w[3];
                if (w[3] == 0 && zero == 0) zero = w[2];
                if (w[3] == limitRegister && index == 0) index = w[2];
            }
            break;
        case kOpVariable:
            if (i.count >= 4 && w[3] == kStoragePushConstant) {
                if (pushVariable != 0) {
                    error = "more than one push constant block";
                    return false;
                }
                pushVariable = w[2];
                pushPointerType = w[1];
            }
            break;
        case kOpDecorate:
            if (i.count == 4 && w[2] == kDecorationBuiltIn && w[3] == kBuiltInGlobalInvocationId) globalId = w[1];
            if (firstAnnotation == all.size()) firstAnnotation = n;
            break;
        case kOpMemberDecorate:
            if (firstAnnotation == all.size()) firstAnnotation = n;
            break;
        case kOpFunction:
            if (firstFunction == all.size()) firstFunction = n;
            if (i.count == 5) {
                functionResult[w[2]] = w[1];
                if (w[2] == entry) entryFunction = n;
            }
            break;
        default: break;
        }
    }
    if (typeUint == 0 || pushVariable == 0 || firstFunction == all.size() || entryFunction == all.size() || firstAnnotation == all.size()) {
        error = "a compute module without a 32-bit unsigned type, a push constant block, annotations or its entry function";
        return false;
    }
    if (typeVoid == 0 || functionResult[entry] != typeVoid) {
        error = "a compute entry function that returns a value";
        return false;
    }
    const auto block = pointee.find(pushPointerType);
    const auto registers = block != pointee.end() ? firstMember.find(block->second) : firstMember.end();
    const auto array = registers != firstMember.end() ? arrays.find(registers->second) : arrays.end();
    const auto length = array != arrays.end() ? uintConstants.find(array->second.second) : uintConstants.end();
    if (array == arrays.end() || array->second.first != typeUint || length == uintConstants.end() || length->second <= limitRegister) {
        error = "a push constant block whose first member is not an array of user data registers that holds register " + std::to_string(limitRegister);
        return false;
    }
    std::size_t label = entryFunction + 1;
    if (label >= all.size() || all[label].op != kOpLabel) {
        error = "an entry function that takes parameters or has no body";
        return false;
    }
    std::size_t body = label + 1;
    while (body < all.size() && all[body].op == kOpVariable) ++body;
    if (body >= all.size()) {
        error = "an entry function without a body";
        return false;
    }

    std::uint32_t bound = words[3];
    const auto fresh = [&bound] { return bound++; };
    std::vector<std::uint32_t> globals;
    if (typeBool == 0) Emit(globals, kOpTypeBool, {typeBool = fresh()});
    if (zero == 0) Emit(globals, kOpConstant, {typeUint, zero = fresh(), 0});
    if (index == 0) Emit(globals, kOpConstant, {typeUint, index = fresh(), limitRegister});
    if (pointerPush == 0) Emit(globals, kOpTypePointer, {pointerPush = fresh(), kStoragePushConstant, typeUint});
    const bool addGlobalId = globalId == 0;
    if (addGlobalId) {
        if (typeVector == 0) Emit(globals, kOpTypeVector, {typeVector = fresh(), typeUint, 3});
        if (pointerInput == 0) Emit(globals, kOpTypePointer, {pointerInput = fresh(), kStorageInput, typeVector});
        Emit(globals, kOpVariable, {pointerInput, globalId = fresh(), kStorageInput});
    } else if (typeVector == 0) {
        error = "a GlobalInvocationId variable without a three-component unsigned vector type";
        return false;
    }
    const std::uint32_t oldLabel = words[all[label].at + 1];
    const std::uint32_t newLabel = fresh();
    const std::uint32_t outside = fresh();
    const std::uint32_t loaded = fresh();
    const std::uint32_t thread = fresh();
    const std::uint32_t chain = fresh();
    const std::uint32_t limit = fresh();
    const std::uint32_t inside = fresh();
    std::vector<std::uint32_t> guard;
    Emit(guard, kOpLoad, {typeVector, loaded, globalId});
    Emit(guard, kOpCompositeExtract, {typeUint, thread, loaded, 0});
    Emit(guard, kOpAccessChain, {pointerPush, chain, pushVariable, zero, index});
    Emit(guard, kOpLoad, {typeUint, limit, chain});
    Emit(guard, kOpULessThan, {typeBool, inside, thread, limit});
    Emit(guard, kOpSelectionMerge, {oldLabel, 0});
    Emit(guard, kOpBranchConditional, {inside, oldLabel, outside});
    Emit(guard, kOpLabel, {outside});
    Emit(guard, kOpReturn, {});
    Emit(guard, kOpLabel, {oldLabel});

    std::vector<std::uint32_t> out;
    out.reserve(words.size() + globals.size() + guard.size() + 8);
    out.insert(out.end(), words.begin(), words.begin() + kHeaderWords);
    for (std::size_t n = 0; n < all.size(); ++n) {
        const Instruction& i = all[n];
        if (n == firstAnnotation && addGlobalId) Emit(out, kOpDecorate, {globalId, kDecorationBuiltIn, kBuiltInGlobalInvocationId});
        if (n == firstFunction) out.insert(out.end(), globals.begin(), globals.end());
        if (n == body) out.insert(out.end(), guard.begin(), guard.end());
        if (n == entryIndex && addGlobalId) {
            out.push_back(((i.count + 1) << 16) | i.op);
            out.insert(out.end(), words.begin() + i.at + 1, words.begin() + i.at + i.count);
            out.push_back(globalId);
            continue;
        }
        if (n == label) {
            Emit(out, kOpLabel, {newLabel});
            continue;
        }
        out.insert(out.end(), words.begin() + i.at, words.begin() + i.at + i.count);
    }
    out[3] = bound;
    words = std::move(out);
    return true;
}

}
