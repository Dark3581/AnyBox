#include "LaneRegisters.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace abx::d3d12x;

int g_failures = 0;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

enum : std::uint32_t {
    kEntryPoint = 15,
    kExecutionMode = 16,
    kTypeVoid = 19,
    kTypeBool = 20,
    kTypeInt = 21,
    kTypeArray = 28,
    kTypePointer = 32,
    kTypeFunction = 33,
    kConstant = 43,
    kConstantComposite = 44,
    kFunction = 54,
    kFunctionEnd = 56,
    kVariable = 59,
    kLoad = 61,
    kStore = 62,
    kAccessChain = 65,
    kDecorate = 71,
    kIAdd = 128,
    kIMul = 132,
    kULessThan = 176,
    kControlBarrier = 224,
    kAtomicIAdd = 234,
    kPhi = 245,
    kLoopMerge = 246,
    kSelectionMerge = 247,
    kLabel = 248,
    kBranch = 249,
    kBranchConditional = 250,
    kReturn = 253,
    kSetMeshOutputs = 5295,
    kModelFragment = 4,
    kModelMesh = 5365,
    kModeLocalSize = 17,
    kBuiltIn = 11,
    kLocalInvocationIndex = 29,
    kInputStorage = 1,
    kWorkgroupStorage = 4,
    kFunctionStorage = 7,
    kMainName = 0x6e69616d,
};

enum : std::uint32_t {
    kVoid = 1,
    kBool,
    kUint,
    kMainType,
    kZero,
    kOne,
    kTwo,
    kFive,
    kRegisters,
    kWordPointer,
    kRegistersPointer,
    kInputPointer,
    kSharedPointer,
    kLaneIndex,
    kShared,
    kMain,
    kEntry,
    kCount,
    kFile,
    kLane,
    kSlot,
    kHead,
    kBody,
    kHeld,
    kSmall,
    kThen,
    kBumped,
    kJoin,
    kMixed,
    kVote,
    kTail,
    kSeen,
    kNext,
    kAgain,
    kExit,
    kTotal,
    kLast,
    kBound,
};

void Emit(std::vector<std::uint32_t>& out, std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) {
    out.push_back(static_cast<std::uint32_t>((operands.size() + 1) << 16) | opcode);
    out.insert(out.end(), operands.begin(), operands.end());
}

std::vector<std::uint32_t> Module(std::uint32_t model, std::uint32_t lanes, bool barrierInSelection) {
    std::vector<std::uint32_t> words{0x07230203, 0x00010500, 0, kBound, 0};
    Emit(words, kEntryPoint, {model, kMain, kMainName, 0, kLaneIndex});
    Emit(words, kExecutionMode, {kMain, kModeLocalSize, lanes, 1, 1});
    Emit(words, kDecorate, {kLaneIndex, kBuiltIn, kLocalInvocationIndex});
    Emit(words, kTypeVoid, {kVoid});
    Emit(words, kTypeBool, {kBool});
    Emit(words, kTypeInt, {kUint, 32, 0});
    Emit(words, kTypeFunction, {kMainType, kVoid});
    Emit(words, kConstant, {kUint, kZero, 0});
    Emit(words, kConstant, {kUint, kOne, 1});
    Emit(words, kConstant, {kUint, kTwo, 2});
    Emit(words, kConstant, {kUint, kFive, 5});
    Emit(words, kTypeArray, {kRegisters, kUint, kFive});
    Emit(words, kTypePointer, {kWordPointer, kFunctionStorage, kUint});
    Emit(words, kTypePointer, {kRegistersPointer, kFunctionStorage, kRegisters});
    Emit(words, kTypePointer, {kInputPointer, kInputStorage, kUint});
    Emit(words, kTypePointer, {kSharedPointer, kWorkgroupStorage, kUint});
    Emit(words, kVariable, {kInputPointer, kLaneIndex, kInputStorage});
    Emit(words, kVariable, {kSharedPointer, kShared, kWorkgroupStorage});
    Emit(words, kFunction, {kVoid, kMain, 0, kMainType});
    Emit(words, kLabel, {kEntry});
    Emit(words, kVariable, {kWordPointer, kCount, kFunctionStorage, kZero});
    Emit(words, kVariable, {kRegistersPointer, kFile, kFunctionStorage});
    Emit(words, kLoad, {kUint, kLane, kLaneIndex});
    Emit(words, kAccessChain, {kWordPointer, kSlot, kFile, kOne});
    Emit(words, kStore, {kSlot, kLane});
    Emit(words, kStore, {kCount, kLane});
    Emit(words, kControlBarrier, {kTwo, kTwo, kZero});
    Emit(words, kBranch, {kHead});
    Emit(words, kLabel, {kHead});
    Emit(words, kLoopMerge, {kExit, kTail, 0});
    Emit(words, kBranch, {kBody});
    Emit(words, kLabel, {kBody});
    Emit(words, kLoad, {kUint, kHeld, kCount});
    Emit(words, kULessThan, {kBool, kSmall, kHeld, kFive});
    Emit(words, kSelectionMerge, {kJoin, 0});
    Emit(words, kBranchConditional, {kSmall, kThen, kJoin});
    Emit(words, kLabel, {kThen});
    Emit(words, kIAdd, {kUint, kBumped, kHeld, kOne});
    Emit(words, kStore, {kSlot, kBumped});
    if (barrierInSelection) Emit(words, kControlBarrier, {kTwo, kTwo, kZero});
    Emit(words, kBranch, {kJoin});
    Emit(words, kLabel, {kJoin});
    Emit(words, kPhi, {kUint, kMixed, kBumped, kThen, kHeld, kBody});
    Emit(words, kAtomicIAdd, {kUint, kVote, kShared, kTwo, kZero, kMixed});
    Emit(words, kBranch, {kTail});
    Emit(words, kLabel, {kTail});
    Emit(words, kControlBarrier, {kTwo, kTwo, kZero});
    Emit(words, kLoad, {kUint, kSeen, kShared});
    Emit(words, kIAdd, {kUint, kNext, kMixed, kSeen});
    Emit(words, kStore, {kCount, kNext});
    Emit(words, kULessThan, {kBool, kAgain, kSeen, kFive});
    Emit(words, kBranchConditional, {kAgain, kHead, kExit});
    Emit(words, kLabel, {kExit});
    Emit(words, kControlBarrier, {kTwo, kTwo, kZero});
    Emit(words, kLoad, {kUint, kTotal, kShared});
    Emit(words, kSetMeshOutputs, {kTotal, kTotal});
    Emit(words, kLoad, {kUint, kLast, kCount});
    Emit(words, kStore, {kShared, kLast});
    Emit(words, kReturn, {});
    Emit(words, kFunctionEnd, {});
    return words;
}

struct Instruction {
    std::uint32_t opcode = 0;
    std::vector<std::uint32_t> operands;
};

std::vector<Instruction> Parse(const std::vector<std::uint32_t>& words) {
    std::vector<Instruction> list;
    for (std::size_t at = 5; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        if (count == 0 || at + count > words.size()) return {};
        list.push_back({words[at] & 0xFFFFu, std::vector<std::uint32_t>(words.begin() + static_cast<std::ptrdiff_t>(at + 1), words.begin() + static_cast<std::ptrdiff_t>(at + count))});
        at += count;
    }
    return list;
}

std::uint32_t ResultOf(const Instruction& instruction) {
    switch (instruction.opcode) {
    case kTypeVoid:
    case kTypeBool:
    case kTypeInt:
    case kTypeArray:
    case kTypePointer:
    case kTypeFunction:
    case kLabel:
        return instruction.operands[0];
    case kConstant:
    case kConstantComposite:
    case kFunction:
    case kVariable:
    case kLoad:
    case kAccessChain:
    case kIAdd:
    case kIMul:
    case kULessThan:
    case kAtomicIAdd:
    case kPhi:
        return instruction.operands[1];
    default:
        return 0;
    }
}

void Turns() {
    std::vector<std::uint32_t> words = Module(kModelMesh, 128, false);
    std::string error;
    const std::optional<LaneTurns> turns = RunLaneGroupInTurns(words, 32, error);
    Expect(turns && turns->turns == 4 && turns->regions == 5 && turns->variables == 2 && turns->carried == 3, "a group of 128 lanes runs in 4 turns of 32: five regions, two variables, three carried values");
    if (!turns) {
        std::printf("refused: %s\n", error.c_str());
        return;
    }
    const std::vector<Instruction> list = Parse(words);
    Expect(!list.empty(), "the result is a well formed instruction stream");
    std::map<std::uint32_t, Instruction> made;
    std::map<std::uint32_t, std::uint32_t> blockOf;
    std::set<std::uint32_t> results;
    bool unique = true;
    std::uint32_t block = 0;
    std::size_t barriers = 0, outputs = 0, loops = 0, laneLoads = 0, fileChains = 0, turnLoops = 0;
    std::uint32_t head = 0, headNext = 0, branchOfSelection = 0;
    std::size_t firstFunction = 0;
    for (std::size_t index = 0; index < list.size(); ++index) {
        const Instruction& instruction = list[index];
        const std::uint32_t result = ResultOf(instruction);
        if (result != 0) {
            unique = unique && results.insert(result).second && result < words[3];
            made[result] = instruction;
        }
        if (instruction.opcode == kFunction && firstFunction == 0) firstFunction = index;
        if (instruction.opcode == kExecutionMode) Expect(instruction.operands[2] == 32 && instruction.operands[3] == 1 && instruction.operands[4] == 1, "the group is 32 lanes wide");
        if (instruction.opcode == kLabel) block = result;
        if (result != 0 && firstFunction != 0) blockOf[result] = block;
        if (instruction.opcode == kControlBarrier) ++barriers;
        if (instruction.opcode == kSetMeshOutputs) ++outputs;
        if (instruction.opcode == kSelectionMerge) branchOfSelection = block;
        if (instruction.opcode == kLoopMerge) {
            ++loops;
            if (block == kHead) {
                head = instruction.operands[0];
                headNext = instruction.operands[1];
            } else {
                ++turnLoops;
            }
        }
        if (instruction.opcode == kLoad && instruction.operands[2] == kLaneIndex) {
            ++laneLoads;
            const Instruction& scaled = list[index + 1];
            const Instruction& moved = list[index + 2];
            Expect(scaled.opcode == kIMul && made[scaled.operands[3]].opcode == kConstant && made[scaled.operands[3]].operands[2] == 32, "the lane index is offset by the turn times 32");
            Expect(moved.opcode == kIAdd && moved.operands[1] == kLane && moved.operands[2] == instruction.operands[1] && moved.operands[3] == scaled.operands[1], "the lane's own index keeps its id");
        }
        if (instruction.opcode == kAccessChain && instruction.operands[2] == kFile) {
            ++fileChains;
            Expect(instruction.operands.size() == 5 && instruction.operands[4] == kOne && made[instruction.operands[3]].opcode == kLoad, "a register is reached through the turn and then its index");
        }
        if (instruction.opcode == kSetMeshOutputs) {
            const Instruction& value = made[instruction.operands[0]];
            const Instruction& slot = made[value.operands.size() == 3 ? value.operands[2] : 0];
            Expect(instruction.operands[0] != kTotal && value.opcode == kLoad && slot.opcode == kAccessChain && slot.operands[3] == kZero && made[slot.operands[2]].opcode == kVariable,
                   "the output counts are those of the first lane, read from a carried value");
        }
    }
    Expect(unique, "every result id is made once and lies under the bound");
    Expect(barriers == 3 && outputs == 1, "barriers and the output counts stay as often as they were");
    Expect(loops == 6 && turnLoops == 5, "one loop of turns for each region beside the title's loop");
    Expect(laneLoads == 1 && fileChains == 2, "one lane index load, and the register pointer is made again where it is used after a barrier");
    for (const std::uint32_t variable : {static_cast<std::uint32_t>(kCount), static_cast<std::uint32_t>(kFile)}) {
        const Instruction& pointer = made[made[variable].operands[0]];
        const Instruction& row = made[pointer.operands.size() == 3 ? pointer.operands[2] : 0];
        Expect(pointer.opcode == kTypePointer && row.opcode == kTypeArray && row.operands[1] == (variable == kCount ? kUint : kRegisters) && made[row.operands[2]].operands[2] == 4,
               "a function variable holds one value for each turn");
    }
    const Instruction& initial = made[made[kCount].operands.size() == 4 ? made[kCount].operands[3] : 0];
    Expect(initial.opcode == kConstantComposite && initial.operands.size() == 6 && initial.operands[2] == kZero && initial.operands[5] == kZero, "an initial value is repeated for each turn");
    const Instruction& phi = made[kMixed];
    Expect(phi.opcode == kPhi && phi.operands[3] == kThen && phi.operands[5] == branchOfSelection && branchOfSelection != kBody && branchOfSelection != 0,
           "a merged value names the block that now holds the branch");
    Expect(head == kExit && headNext != kTail && headNext != 0, "the title's loop continues at a new block");
    bool backEdge = false;
    for (std::size_t index = 0; index + 2 < list.size(); ++index) {
        const Instruction& instruction = list[index];
        if (instruction.opcode != kBranchConditional || instruction.operands[1] != headNext || instruction.operands[2] != kExit) continue;
        backEdge = list[index + 1].opcode == kLabel && list[index + 1].operands[0] == headNext && list[index + 2].opcode == kBranch && list[index + 2].operands[0] == kHead &&
                   instruction.operands[0] != kAgain && made[instruction.operands[0]].opcode == kLoad;
    }
    Expect(backEdge, "the loop goes on by the first lane's carried condition");
    std::vector<std::uint32_t> again = words;
    const std::optional<LaneTurns> second = RunLaneGroupInTurns(again, 32, error);
    Expect(second && second->turns == 0 && again == words, "a group that is already 32 lanes wide is left alone");
}

void Refusals() {
    std::string error;
    std::vector<std::uint32_t> none{1, 2, 3};
    Expect(!RunLaneGroupInTurns(none, 32, error) && error == "not a SPIR-V module", "a word stream that is no module is refused");
    std::vector<std::uint32_t> cut = Module(kModelMesh, 128, false);
    cut.pop_back();
    cut.back() = (9u << 16) | kReturn;
    const std::vector<std::uint32_t> cutBefore = cut;
    Expect(!RunLaneGroupInTurns(cut, 32, error) && error == "a malformed instruction" && cut == cutBefore, "an instruction that runs past the end is refused");
    std::vector<std::uint32_t> nested = Module(kModelMesh, 128, true);
    const std::vector<std::uint32_t> nestedBefore = nested;
    Expect(!RunLaneGroupInTurns(nested, 32, error) && error == "a barrier inside a selection" && nested == nestedBefore, "a barrier inside a selection is refused and the module is left as it was");
    std::vector<std::uint32_t> uneven = Module(kModelMesh, 128, false);
    const std::vector<std::uint32_t> unevenBefore = uneven;
    Expect(!RunLaneGroupInTurns(uneven, 48, error) && uneven == unevenBefore, "a lane count that does not divide the group is refused");
    std::vector<std::uint32_t> wide = Module(kModelMesh, 128, false);
    const std::vector<std::uint32_t> wideBefore = wide;
    const std::optional<LaneTurns> kept = RunLaneGroupInTurns(wide, 128, error);
    Expect(kept && kept->turns == 0 && wide == wideBefore, "a device that prefers the whole group leaves the module alone");
    std::vector<std::uint32_t> other = Module(kModelFragment, 128, false);
    const std::vector<std::uint32_t> otherBefore = other;
    const std::optional<LaneTurns> untouched = RunLaneGroupInTurns(other, 32, error);
    Expect(untouched && untouched->turns == 0 && other == otherBefore, "a module without a mesh entry point is left alone");
    std::vector<std::uint32_t> zero = Module(kModelMesh, 128, false);
    Expect(!RunLaneGroupInTurns(zero, 0, error), "no lane count is refused");
}

}

int main() {
    Turns();
    Refusals();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("lane turns ok\n");
    return 0;
}
