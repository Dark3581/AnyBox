#include "LaneRegisters.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
#include <utility>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::size_t kHeaderWords = 5;
constexpr std::uint32_t kOpExtInst = 12;
constexpr std::uint32_t kOpEntryPoint = 15;
constexpr std::uint32_t kOpExecutionMode = 16;
constexpr std::uint32_t kOpTypeVoid = 19;
constexpr std::uint32_t kOpTypeBool = 20;
constexpr std::uint32_t kOpTypeInt = 21;
constexpr std::uint32_t kOpTypeArray = 28;
constexpr std::uint32_t kOpTypePointer = 32;
constexpr std::uint32_t kOpTypeLast = 38;
constexpr std::uint32_t kOpConstant = 43;
constexpr std::uint32_t kOpConstantComposite = 44;
constexpr std::uint32_t kOpFunction = 54;
constexpr std::uint32_t kOpFunctionEnd = 56;
constexpr std::uint32_t kOpFunctionCall = 57;
constexpr std::uint32_t kOpVariable = 59;
constexpr std::uint32_t kOpLoad = 61;
constexpr std::uint32_t kOpStore = 62;
constexpr std::uint32_t kOpAccessChain = 65;
constexpr std::uint32_t kOpInBoundsAccessChain = 66;
constexpr std::uint32_t kOpDecorate = 71;
constexpr std::uint32_t kOpVectorExtractDynamic = 77;
constexpr std::uint32_t kOpVectorInsertDynamic = 78;
constexpr std::uint32_t kOpVectorShuffle = 79;
constexpr std::uint32_t kOpCompositeConstruct = 80;
constexpr std::uint32_t kOpCompositeExtract = 81;
constexpr std::uint32_t kOpCompositeInsert = 82;
constexpr std::uint32_t kOpCopyObject = 83;
constexpr std::uint32_t kOpTranspose = 84;
constexpr std::uint32_t kOpSampledImage = 86;
constexpr std::uint32_t kOpImageSampleImplicitLod = 87;
constexpr std::uint32_t kOpImageSampleExplicitLod = 88;
constexpr std::uint32_t kOpImageFetch = 95;
constexpr std::uint32_t kOpImageGather = 96;
constexpr std::uint32_t kOpImageRead = 98;
constexpr std::uint32_t kOpImageWrite = 99;
constexpr std::uint32_t kOpImage = 100;
constexpr std::uint32_t kOpConvertFToU = 109;
constexpr std::uint32_t kOpConvertUToPtr = 120;
constexpr std::uint32_t kOpBitcast = 124;
constexpr std::uint32_t kOpSNegate = 126;
constexpr std::uint32_t kOpIAdd = 128;
constexpr std::uint32_t kOpIMul = 132;
constexpr std::uint32_t kOpSMulExtended = 152;
constexpr std::uint32_t kOpAny = 154;
constexpr std::uint32_t kOpULessThan = 176;
constexpr std::uint32_t kOpFUnordGreaterThanEqual = 191;
constexpr std::uint32_t kOpShiftRightLogical = 194;
constexpr std::uint32_t kOpBitCount = 205;
constexpr std::uint32_t kOpControlBarrier = 224;
constexpr std::uint32_t kOpMemoryBarrier = 225;
constexpr std::uint32_t kOpAtomicLoad = 227;
constexpr std::uint32_t kOpAtomicStore = 228;
constexpr std::uint32_t kOpAtomicExchange = 229;
constexpr std::uint32_t kOpAtomicXor = 242;
constexpr std::uint32_t kOpPhi = 245;
constexpr std::uint32_t kOpLoopMerge = 246;
constexpr std::uint32_t kOpSelectionMerge = 247;
constexpr std::uint32_t kOpLabel = 248;
constexpr std::uint32_t kOpBranch = 249;
constexpr std::uint32_t kOpBranchConditional = 250;
constexpr std::uint32_t kOpSwitch = 251;
constexpr std::uint32_t kOpKill = 252;
constexpr std::uint32_t kOpReturn = 253;
constexpr std::uint32_t kOpReturnValue = 254;
constexpr std::uint32_t kOpUnreachable = 255;
constexpr std::uint32_t kOpExecutionModeId = 331;
constexpr std::uint32_t kOpSetMeshOutputs = 5295;
constexpr std::uint32_t kModelMesh = 5365;
constexpr std::uint32_t kModeLocalSize = 17;
constexpr std::uint32_t kDecorationBuiltIn = 11;
constexpr std::uint32_t kBuiltInWorkgroupSize = 25;
constexpr std::uint32_t kBuiltInLocalInvocationId = 27;
constexpr std::uint32_t kBuiltInGlobalInvocationId = 28;
constexpr std::uint32_t kBuiltInLocalInvocationIndex = 29;
constexpr std::uint32_t kBuiltInNumSubgroups = 38;
constexpr std::uint32_t kBuiltInSubgroupId = 40;
constexpr std::uint32_t kBuiltInSubgroupLocalInvocationId = 41;
constexpr std::uint32_t kStorageFunction = 7;
constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr std::size_t kNowhere = static_cast<std::size_t>(-1);

struct Instruction {
    std::size_t at = 0;
    std::uint32_t op = 0;
    std::uint32_t count = 0;
};

struct Fields {
    bool type = false;
    bool result = false;
    std::vector<std::uint32_t> ids;
};

struct Block {
    std::uint32_t id = 0;
    std::size_t label = 0;
    std::size_t merge = kNowhere;
    std::size_t last = 0;
};

enum class Step { Open, Close, Body, Sync, Plain, Link, BackEdge };

struct Item {
    Step step = Step::Plain;
    std::size_t instruction = 0;
    std::uint32_t region = kNone;
    std::uint32_t label = 0;
};

struct Pointer {
    std::uint32_t storage = 0;
    std::uint32_t pointee = 0;
};

struct RegionCode {
    std::uint32_t header = 0;
    std::uint32_t body = 0;
    std::uint32_t next = 0;
    std::uint32_t merge = 0;
    std::uint32_t turn = 0;
    std::map<std::uint32_t, std::uint32_t> renamed;
    std::map<std::uint32_t, std::uint32_t> slots;
};

bool PlainValue(std::uint32_t op) {
    return op == kOpFunctionCall || op == kOpVectorExtractDynamic || op == kOpVectorInsertDynamic || op == kOpCompositeConstruct || op == kOpCopyObject || op == kOpTranspose || op == kOpSampledImage ||
           op == kOpImage || (op >= kOpConvertFToU && op <= kOpConvertUToPtr) || op == kOpBitcast || (op >= kOpSNegate && op <= kOpSMulExtended) || (op >= kOpAny && op <= kOpFUnordGreaterThanEqual) ||
           (op >= kOpShiftRightLogical && op <= kOpBitCount) || op == kOpAtomicLoad || (op >= kOpAtomicExchange && op <= kOpAtomicXor) || op == kOpPhi || op == kOpAccessChain ||
           op == kOpInBoundsAccessChain;
}

bool Describe(const Instruction& instruction, Fields& fields) {
    fields.type = false;
    fields.result = false;
    fields.ids.clear();
    const std::uint32_t op = instruction.op;
    const std::uint32_t count = instruction.count;
    const auto range = [&](std::uint32_t from, std::uint32_t to) {
        for (std::uint32_t word = from; word < to && word < count; ++word) fields.ids.push_back(word);
    };
    const auto typed = [&](std::uint32_t least) {
        fields.type = true;
        fields.result = true;
        return count >= least;
    };
    if (PlainValue(op)) {
        if (!typed(3)) return false;
        range(3, count);
        return true;
    }
    switch (op) {
    case kOpLabel:
        fields.result = true;
        return count == 2;
    case kOpVariable:
        if (!typed(4)) return false;
        range(4, count);
        return true;
    case kOpLoad:
        if (!typed(4)) return false;
        range(3, 4);
        return true;
    case kOpStore:
        range(1, 3);
        return count >= 3;
    case kOpExtInst:
        if (!typed(5)) return false;
        range(3, 4);
        range(5, count);
        return true;
    case kOpCompositeExtract:
        if (!typed(4)) return false;
        range(3, 4);
        return true;
    case kOpCompositeInsert:
    case kOpVectorShuffle:
        if (!typed(5)) return false;
        range(3, 5);
        return true;
    case kOpImageSampleImplicitLod:
    case kOpImageSampleExplicitLod:
    case kOpImageFetch:
    case kOpImageRead:
        if (!typed(5)) return false;
        range(3, 5);
        range(6, count);
        return true;
    case kOpImageGather:
        if (!typed(6)) return false;
        range(3, 6);
        range(7, count);
        return true;
    case kOpImageWrite:
        range(1, 4);
        range(5, count);
        return count >= 4;
    case kOpSwitch:
        range(1, 3);
        if (count < 3 || (count - 3) % 2 != 0) return false;
        for (std::uint32_t word = 4; word < count; word += 2) fields.ids.push_back(word);
        return true;
    case kOpLoopMerge:
        range(1, 3);
        return count >= 4;
    case kOpSelectionMerge:
        range(1, 2);
        return count == 3;
    case kOpBranch:
        range(1, 2);
        return count == 2;
    case kOpBranchConditional:
        range(1, 4);
        return count >= 4;
    case kOpReturn:
    case kOpUnreachable:
        return count == 1;
    case kOpReturnValue:
        range(1, 2);
        return count == 2;
    case kOpControlBarrier:
        range(1, 4);
        return count == 4;
    case kOpMemoryBarrier:
        range(1, 3);
        return count == 3;
    case kOpAtomicStore:
        range(1, 5);
        return count == 5;
    case kOpSetMeshOutputs:
        range(1, 3);
        return count == 3;
    default:
        return false;
    }
}

bool Synchronizes(std::uint32_t op) {
    return op == kOpControlBarrier || op == kOpSetMeshOutputs;
}

bool Ends(std::uint32_t op) {
    return op == kOpBranch || op == kOpBranchConditional || op == kOpSwitch || op == kOpKill || op == kOpReturn || op == kOpReturnValue || op == kOpUnreachable;
}

void Emit(std::vector<std::uint32_t>& out, std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) {
    out.push_back(static_cast<std::uint32_t>((operands.size() + 1) << 16) | opcode);
    out.insert(out.end(), operands.begin(), operands.end());
}

class Turns {
public:
    Turns(std::vector<std::uint32_t>& words, std::uint32_t lanes, std::string& error) : words_(words), lanes_(lanes), error_(error) {}

    std::optional<LaneTurns> Run() {
        if (words_.size() < kHeaderWords || words_[0] != kMagic) return Refuse("not a SPIR-V module");
        if (lanes_ == 0) return Refuse("no lane count to run a group in");
        bound_ = words_[3];
        bool wanted = false;
        if (!Globals(wanted)) return std::nullopt;
        if (!wanted) return LaneTurns{};
        if (!Functions() || !Plan() || !Carry()) return std::nullopt;
        Write();
        LaneTurns result;
        result.turns = turns_;
        result.regions = regions_;
        result.variables = variables_.size();
        result.carried = carriers_.size();
        return result;
    }

private:
    std::nullopt_t Refuse(const std::string& why) {
        error_ = why;
        return std::nullopt;
    }

    bool Fail(const std::string& why) {
        error_ = why;
        return false;
    }

    bool Globals(bool& wanted) {
        std::uint32_t entries = 0;
        std::map<std::uint32_t, std::uint32_t> builtIns;
        for (std::size_t at = kHeaderWords; at < words_.size();) {
            const std::uint32_t count = words_[at] >> 16;
            const std::uint32_t op = words_[at] & 0xFFFFu;
            if (count == 0 || at + count > words_.size()) return Fail("a malformed instruction");
            if (op == kOpFunction) {
                firstFunction_ = at;
                break;
            }
            if (op == kOpEntryPoint && count >= 3 && words_[at + 1] == kModelMesh) {
                ++entries;
                entry_ = words_[at + 2];
            } else if (op == kOpExecutionMode && count == 6 && words_[at + 2] == kModeLocalSize) {
                localSize_[words_[at + 1]] = at;
            } else if (op == kOpExecutionModeId) {
                return Fail("an execution mode given by constants");
            } else if (op == kOpDecorate && count == 4 && words_[at + 2] == kDecorationBuiltIn) {
                builtIns[words_[at + 1]] = words_[at + 3];
            } else if (op >= kOpTypeVoid && op <= kOpTypeLast && count >= 2) {
                types_.insert(words_[at + 1]);
                if (op == kOpTypeInt && count == 4 && words_[at + 2] == 32) {
                    wordTypes_.insert(words_[at + 1]);
                    if (words_[at + 3] == 0 && uint_ == 0) uint_ = words_[at + 1];
                } else if (op == kOpTypeBool && bool_ == 0) {
                    bool_ = words_[at + 1];
                } else if (op == kOpTypePointer && count == 4) {
                    pointers_[words_[at + 1]] = {words_[at + 2], words_[at + 3]};
                }
            } else if (op == kOpConstant && count == 4) {
                constants_[words_[at + 2]] = {words_[at + 1], words_[at + 3]};
            } else if (op == kOpVariable && count >= 4) {
                const auto builtIn = builtIns.find(words_[at + 2]);
                if (builtIn != builtIns.end()) {
                    if (builtIn->second == kBuiltInLocalInvocationIndex) {
                        laneIndex_ = words_[at + 2];
                    } else if (builtIn->second == kBuiltInLocalInvocationId || builtIn->second == kBuiltInGlobalInvocationId || builtIn->second == kBuiltInNumSubgroups ||
                               builtIn->second == kBuiltInSubgroupId || builtIn->second == kBuiltInSubgroupLocalInvocationId) {
                        laneBound_.insert(words_[at + 2]);
                    }
                }
            }
            if (op == kOpDecorate && count == 4 && words_[at + 2] == kDecorationBuiltIn && words_[at + 3] == kBuiltInWorkgroupSize) return Fail("a group size given by a constant");
            at += count;
        }
        if (firstFunction_ == 0 || entries == 0) return true;
        if (entries != 1) return Fail("more than one mesh entry point");
        const auto local = localSize_.find(entry_);
        if (local == localSize_.end()) return Fail("a mesh entry point without a group size");
        const std::uint32_t size = words_[local->second + 3];
        if (words_[local->second + 4] != 1 || words_[local->second + 5] != 1) return Fail("a mesh group that is not one row of lanes");
        if (size <= lanes_) return true;
        if (size % lanes_ != 0) return Fail("a group size that is no multiple of the lane count");
        if (uint_ == 0) return Fail("no 32-bit unsigned type");
        turns_ = size / lanes_;
        wanted = true;
        return true;
    }

    bool Functions() {
        for (std::size_t at = firstFunction_; at < words_.size();) {
            const std::uint32_t count = words_[at] >> 16;
            const std::uint32_t op = words_[at] & 0xFFFFu;
            if (count == 0 || at + count > words_.size()) return Fail("a malformed instruction");
            if (op == kOpFunction) {
                if (count != 5) return Fail("a malformed function");
                inMain_ = words_[at + 2] == entry_;
                if (inMain_) mainAt_ = at;
            } else if (op == kOpFunctionEnd) {
                if (inMain_) mainEnd_ = at;
                inMain_ = false;
            } else if (inMain_) {
                code_.push_back({at, op, count});
            } else {
                if (Synchronizes(op)) return Fail("a barrier outside the entry point");
                if ((op == kOpLoad || op == kOpAccessChain || op == kOpInBoundsAccessChain) && count >= 4 && (words_[at + 3] == laneIndex_ || laneBound_.count(words_[at + 3]) != 0)) {
                    return Fail("a lane's own index read outside the entry point");
                }
            }
            at += count;
        }
        if (mainAt_ == 0 || mainEnd_ == 0 || code_.empty()) return Fail("no body for the mesh entry point");
        owner_.assign(code_.size(), 0);
        for (std::size_t index = 0; index < code_.size(); ++index) {
            const Instruction& instruction = code_[index];
            if (instruction.op == kOpLabel) {
                if (!blocks_.empty() && blocks_.back().last == kNowhere) return Fail("a block without an end");
                Block block;
                block.id = words_[instruction.at + 1];
                block.label = index;
                block.last = kNowhere;
                blockOf_[block.id] = blocks_.size();
                blocks_.push_back(block);
                owner_[index] = blocks_.size() - 1;
                continue;
            }
            if (blocks_.empty() || blocks_.back().last != kNowhere) return Fail("an instruction outside a block");
            owner_[index] = blocks_.size() - 1;
            if (instruction.op == kOpLoopMerge || instruction.op == kOpSelectionMerge) blocks_.back().merge = index;
            if (Ends(instruction.op)) blocks_.back().last = index;
        }
        if (blocks_.empty() || blocks_.back().last == kNowhere) return Fail("a block without an end");
        for (const Block& block : blocks_) {
            if (block.merge != kNowhere && block.merge + 1 != block.last) return Fail("a merge that is not the last instruction before the branch");
        }
        Fields fields;
        for (std::size_t index = blocks_[0].label + 1; index < code_.size() && code_[index].op == kOpVariable; ++index) {
            if (!Describe(code_[index], fields) || words_[code_[index].at + 3] != kStorageFunction) return Fail("a malformed function variable");
            if (pointers_.count(words_[code_[index].at + 1]) == 0) return Fail("a function variable without a pointer type");
            variables_[words_[code_[index].at + 2]] = index;
            variablesEnd_ = index + 1;
        }
        if (variablesEnd_ == 0) variablesEnd_ = blocks_[0].label + 1;
        return true;
    }

    std::vector<std::uint32_t> Targets(const Block& block) const {
        const Instruction& last = code_[block.last];
        std::vector<std::uint32_t> targets;
        if (last.op == kOpBranch) {
            targets.push_back(words_[last.at + 1]);
        } else if (last.op == kOpBranchConditional) {
            targets.push_back(words_[last.at + 2]);
            targets.push_back(words_[last.at + 3]);
        } else if (last.op == kOpSwitch) {
            targets.push_back(words_[last.at + 2]);
            for (std::uint32_t word = 4; word < last.count; word += 2) targets.push_back(words_[last.at + word]);
        }
        return targets;
    }

    bool Inside(std::size_t header, std::uint32_t merge, std::uint32_t stop, std::uint32_t leave, std::set<std::size_t>& inside, bool& continues) {
        std::vector<std::size_t> pending{header};
        while (!pending.empty()) {
            const std::size_t at = pending.back();
            pending.pop_back();
            for (const std::uint32_t target : Targets(blocks_[at])) {
                if (target == merge) continue;
                if (stop != 0 && target == stop) {
                    continues = true;
                    continue;
                }
                if (leave != 0 && target == leave) return Fail("a lane leaves a loop that holds a barrier");
                const auto found = blockOf_.find(target);
                if (found == blockOf_.end()) return Fail("a branch to no block");
                if (found->second == header) continue;
                if (inside.insert(found->second).second) pending.push_back(found->second);
            }
        }
        return true;
    }

    bool Holds(const std::set<std::size_t>& inside) const {
        for (const std::size_t at : inside) {
            for (std::size_t index = blocks_[at].label; index <= blocks_[at].last; ++index) {
                if (Synchronizes(code_[index].op)) return true;
            }
        }
        return false;
    }

    void Push(Step step, std::size_t instruction, std::uint32_t region, std::uint32_t label = 0) {
        plan_.push_back({step, instruction, region, label});
        if (step == Step::Body || step == Step::Sync || step == Step::Plain || step == Step::BackEdge) ++planned_[instruction];
    }

    void Begin(std::uint32_t& open) {
        if (open != kNone) return;
        open = static_cast<std::uint32_t>(regions_++);
        next_.push_back(0);
        Push(Step::Open, 0, open);
    }

    void End(std::uint32_t& open, std::uint32_t label = 0) {
        if (open == kNone) return;
        next_[open] = label;
        Push(Step::Close, 0, open, label);
        open = kNone;
    }

    bool Lines(std::size_t from, std::size_t to, std::uint32_t& open) {
        for (std::size_t index = from; index < to; ++index) {
            if (Synchronizes(code_[index].op)) {
                End(open);
                Push(Step::Sync, index, kNone);
                continue;
            }
            if (code_[index].op == kOpPhi && open == kNone) return Fail("a merged value where a turn starts");
            Begin(open);
            Push(Step::Body, index, open);
        }
        return true;
    }

    bool Chain(std::size_t at, std::uint32_t stop, std::uint32_t leave, std::uint32_t& open) {
        for (std::size_t steps = 0; steps <= blocks_.size(); ++steps) {
            const Block& block = blocks_[at];
            Push(open == kNone ? Step::Plain : Step::Body, block.label, open);
            std::size_t first = block.label + 1;
            if (at == 0) {
                for (; first < variablesEnd_; ++first) Push(Step::Plain, first, kNone);
            }
            const std::size_t end = block.merge != kNowhere ? block.merge : block.last;
            if (!Lines(first, end, open)) return false;
            const Instruction& last = code_[block.last];
            if (block.merge != kNowhere) {
                const std::uint32_t merge = words_[code_[block.merge].at + 1];
                const auto after = blockOf_.find(merge);
                if (after == blockOf_.end()) return Fail("a merge at no block");
                std::set<std::size_t> inside;
                bool continues = false;
                const bool loop = code_[block.merge].op == kOpLoopMerge;
                if (!Inside(at, merge, loop ? 0 : stop, loop ? 0 : leave, inside, continues)) return false;
                if (!Holds(inside)) {
                    if (loop) {
                        for (const std::size_t member : inside) {
                            for (const std::uint32_t target : Targets(blocks_[member])) {
                                if ((stop != 0 && target == stop) || (leave != 0 && target == leave)) return Fail("a lane leaves two loops at once");
                            }
                        }
                    }
                    Begin(open);
                    if (continues) continuing_.insert(open);
                    Push(Step::Body, block.merge, open);
                    Push(Step::Body, block.last, open);
                    for (const std::size_t member : inside) {
                        for (std::size_t index = blocks_[member].label; index <= blocks_[member].last; ++index) {
                            const std::uint32_t op = code_[index].op;
                            if (op == kOpReturn || op == kOpReturnValue || op == kOpKill) return Fail("a lane returns before the group does");
                            Push(Step::Body, index, open);
                        }
                    }
                    at = after->second;
                    continue;
                }
                if (!loop) return Fail("a barrier inside a selection");
                if (stop != 0) return Fail("a barrier in a loop inside a loop");
                if (end != first || last.op != kOpBranch || open != kNone) return Fail("a loop with a barrier whose header holds code");
                const std::uint32_t next = words_[code_[block.merge].at + 2];
                const auto body = blockOf_.find(words_[last.at + 1]);
                const auto tail = blockOf_.find(next);
                if (body == blockOf_.end() || tail == blockOf_.end()) return Fail("a loop without a body");
                const std::uint32_t fresh = bound_++;
                replaced_[block.merge] = fresh;
                Push(Step::Plain, block.merge, kNone);
                Push(Step::Plain, block.last, kNone);
                std::uint32_t inner = kNone;
                const std::size_t before = regions_;
                if (!Chain(body->second, next, merge, inner)) return false;
                for (const std::uint32_t region : continuing_) {
                    if (region >= before && region != inner) return Fail("a lane skips to the end of a round across a barrier");
                }
                const Block& closing = blocks_[tail->second];
                if (closing.merge != kNowhere) return Fail("a loop with a barrier that ends in a construct");
                if (inner != kNone) {
                    End(inner, next);
                } else {
                    std::size_t entering = 0;
                    for (const Block& other : blocks_) {
                        for (const std::uint32_t target : Targets(other)) entering += target == next ? 1 : 0;
                    }
                    if (entering != 1) return Fail("a lane skips to the end of a round across a barrier");
                    Push(Step::Link, 0, kNone, next);
                }
                ++planned_[closing.label];
                if (!Lines(closing.label + 1, closing.last, inner)) return false;
                const Instruction& back = code_[closing.last];
                if (back.op != kOpBranchConditional) return Fail("a loop with a barrier that does not end in a condition");
                const std::uint32_t onTrue = words_[back.at + 2];
                const std::uint32_t onFalse = words_[back.at + 3];
                if (!((onTrue == merge && onFalse == block.id) || (onTrue == block.id && onFalse == merge))) return Fail("a loop with a barrier that ends somewhere else");
                End(inner);
                Push(Step::BackEdge, closing.last, kNone, fresh);
                headers_[closing.last] = block.id;
                at = after->second;
                open = kNone;
                continue;
            }
            if (last.op == kOpBranch) {
                const std::uint32_t target = words_[last.at + 1];
                if (stop != 0 && target == stop) {
                    ++planned_[block.last];
                    return true;
                }
                if (leave != 0 && target == leave) return Fail("a lane leaves a loop that holds a barrier");
                const auto found = blockOf_.find(target);
                if (found == blockOf_.end()) return Fail("a branch to no block");
                const Block& following = blocks_[found->second];
                if (following.merge != kNowhere && code_[following.merge].op == kOpLoopMerge) {
                    std::set<std::size_t> inside;
                    bool continues = false;
                    if (!Inside(found->second, words_[code_[following.merge].at + 1], 0, 0, inside, continues)) return false;
                    if (Holds(inside)) End(open);
                }
                Push(open == kNone ? Step::Plain : Step::Body, block.last, open);
                at = found->second;
                continue;
            }
            if (last.op == kOpReturn && stop == 0) {
                End(open);
                Push(Step::Plain, block.last, kNone);
                return true;
            }
            return Fail("a branch at the top of the entry point that is neither a link nor a return");
        }
        return Fail("a chain of blocks that does not end");
    }

    bool Plan() {
        planned_.assign(code_.size(), 0);
        std::uint32_t open = kNone;
        if (!Chain(0, 0, 0, open)) return false;
        for (std::size_t index = 0; index < code_.size(); ++index) {
            if (planned_[index] != 1) return Fail("blocks the walk through the entry point did not reach once");
        }
        if (regions_ == 0) return Fail("an entry point without code to run in turns");
        return true;
    }

    bool Need(std::uint32_t region, std::uint32_t id) {
        if (!needs_[region].insert(id).second) return true;
        if (pointers_.count(typeOf_[id]) == 0) {
            carriers_.emplace(id, 0);
            return true;
        }
        const Instruction& made = code_[madeAt_[id]];
        if (made.op != kOpAccessChain && made.op != kOpInBoundsAccessChain && made.op != kOpCopyObject && made.op != kOpConvertUToPtr && made.op != kOpBitcast) {
            return Fail("a pointer used after a barrier that cannot be made again");
        }
        Fields fields;
        if (!Describe(made, fields)) return Fail("an instruction this pass does not know");
        for (const std::uint32_t word : fields.ids) {
            const std::uint32_t operand = words_[made.at + word];
            if (variables_.count(operand) != 0) {
                if (word != 3 || made.op == kOpCopyObject || made.op == kOpBitcast) return Fail("a function variable used as a value");
                continue;
            }
            if (operand < madeIn_.size() && madeIn_[operand] != kNone && madeIn_[operand] != region && !Need(region, operand)) return false;
        }
        return true;
    }

    bool Carry() {
        madeIn_.assign(bound_, kNone);
        madeAt_.assign(bound_, 0);
        typeOf_.assign(bound_, 0);
        needs_.assign(regions_, {});
        direct_.assign(regions_, {});
        Fields fields;
        for (const Item& item : plan_) {
            if (item.step != Step::Body && item.step != Step::Sync && item.step != Step::Plain && item.step != Step::BackEdge) continue;
            const Instruction& instruction = code_[item.instruction];
            if (!Describe(instruction, fields)) return Fail("an instruction this pass does not know: opcode " + std::to_string(instruction.op));
            if (instruction.op == kOpSwitch) {
                const std::uint32_t selector = words_[instruction.at + 1];
                const std::uint32_t type = selector < typeOf_.size() && typeOf_[selector] != 0 ? typeOf_[selector] : constants_.count(selector) != 0 ? constants_[selector].first : 0;
                if (wordTypes_.count(type) == 0) return Fail("a switch on something that is not a 32-bit word");
            }
            if (!fields.result || instruction.op == kOpLabel) continue;
            const std::uint32_t id = words_[instruction.at + 2];
            if (!fields.type || types_.count(words_[instruction.at + 1]) == 0 || id >= madeIn_.size()) return Fail("a result without a known type: opcode " + std::to_string(instruction.op));
            typeOf_[id] = words_[instruction.at + 1];
            if (item.step != Step::Body) continue;
            madeIn_[id] = item.region;
            madeAt_[id] = item.instruction;
        }
        for (const Item& item : plan_) {
            if (item.step != Step::Body && item.step != Step::Sync && item.step != Step::BackEdge) continue;
            const Instruction& instruction = code_[item.instruction];
            Describe(instruction, fields);
            for (const std::uint32_t word : fields.ids) {
                const std::uint32_t operand = words_[instruction.at + word];
                if (laneBound_.count(operand) != 0) return Fail("a lane position this pass does not move");
                if (operand == laneIndex_ && laneIndex_ != 0) {
                    if (instruction.op != kOpLoad || item.step != Step::Body) return Fail("a lane index used other than by a load in a turn");
                    continue;
                }
                if (variables_.count(operand) != 0) {
                    if (item.step != Step::Body) return Fail("a function variable used at a barrier");
                    const bool chain = (instruction.op == kOpAccessChain || instruction.op == kOpInBoundsAccessChain) && word == 3;
                    const bool pointer = (instruction.op == kOpLoad && word == 3) || (instruction.op == kOpStore && word == 1) ||
                                         ((instruction.op == kOpAtomicLoad || (instruction.op >= kOpAtomicExchange && instruction.op <= kOpAtomicXor)) && word == 3) ||
                                         (instruction.op == kOpAtomicStore && word == 1);
                    if (!chain && !pointer) return Fail("a function variable used as a value");
                    if (pointer) direct_[item.region].insert(operand);
                    continue;
                }
                if (operand >= madeIn_.size() || madeIn_[operand] == kNone) continue;
                if (item.step == Step::Body) {
                    if (madeIn_[operand] != item.region && !Need(item.region, operand)) return false;
                } else {
                    if (pointers_.count(typeOf_[operand]) != 0) return Fail("a pointer used at a barrier");
                    carriers_.emplace(operand, 0);
                }
            }
        }
        return true;
    }

    std::uint32_t Word(std::uint32_t value) {
        for (const auto& [id, constant] : constants_) {
            if (constant.first == uint_ && constant.second == value) return id;
        }
        const std::uint32_t id = bound_++;
        Emit(declared_, kOpConstant, {uint_, id, value});
        constants_[id] = {uint_, value};
        return id;
    }

    std::uint32_t PointerTo(std::uint32_t type) {
        for (const auto& [id, pointer] : pointers_) {
            if (pointer.storage == kStorageFunction && pointer.pointee == type) return id;
        }
        const std::uint32_t id = bound_++;
        Emit(declared_, kOpTypePointer, {id, kStorageFunction, type});
        pointers_[id] = {kStorageFunction, type};
        return id;
    }

    std::uint32_t Row(std::uint32_t type) {
        const auto found = rows_.find(type);
        if (found != rows_.end()) return found->second;
        const std::uint32_t id = bound_++;
        Emit(declared_, kOpTypeArray, {id, type, turnCount_});
        rows_[type] = id;
        return id;
    }

    void Declare() {
        zero_ = Word(0);
        one_ = Word(1);
        turnCount_ = Word(turns_);
        laneCount_ = Word(lanes_);
        if (bool_ == 0) {
            bool_ = bound_++;
            Emit(declared_, kOpTypeBool, {bool_});
        }
        wordPointer_ = PointerTo(uint_);
        turn_ = bound_++;
        for (auto& [id, index] : variables_) {
            const Instruction& instruction = code_[index];
            const std::uint32_t pointee = pointers_[words_[instruction.at + 1]].pointee;
            const std::uint32_t row = Row(pointee);
            VariableCode code;
            code.pointer = PointerTo(row);
            code.element = words_[instruction.at + 1];
            if (instruction.count > 4) {
                code.initial = bound_++;
                std::vector<std::uint32_t> composite{0, row, code.initial};
                composite.insert(composite.end(), turns_, words_[instruction.at + 4]);
                composite[0] = static_cast<std::uint32_t>(composite.size() << 16) | kOpConstantComposite;
                declared_.insert(declared_.end(), composite.begin(), composite.end());
            }
            variableCode_[id] = code;
        }
        for (auto& [id, variable] : carriers_) {
            const std::uint32_t type = typeOf_[id];
            carrierElement_[id] = PointerTo(type);
            carrierPointer_[id] = PointerTo(Row(type));
            variable = bound_++;
        }
    }

    void Rewrite(std::vector<std::uint32_t>& line, const Instruction& instruction, const Fields& fields, const RegionCode& region) {
        bool insert = false;
        for (const std::uint32_t word : fields.ids) {
            const std::uint32_t operand = line[word];
            const auto renamed = region.renamed.find(operand);
            if (renamed != region.renamed.end()) {
                line[word] = renamed->second;
            } else if (variables_.count(operand) != 0) {
                if ((instruction.op == kOpAccessChain || instruction.op == kOpInBoundsAccessChain) && word == 3) {
                    insert = true;
                } else {
                    line[word] = region.slots.at(operand);
                }
            }
        }
        if (insert) {
            line.insert(line.begin() + 4, region.turn);
            line[0] = static_cast<std::uint32_t>(line.size() << 16) | instruction.op;
        }
    }

    void Flush(std::vector<std::uint32_t>& out, const RegionCode& region) {
        for (const std::uint32_t id : waiting_) {
            const std::uint32_t slot = bound_++;
            Emit(out, kOpAccessChain, {carrierElement_[id], slot, carriers_[id], region.turn});
            Emit(out, kOpStore, {slot, id});
        }
        waiting_.clear();
    }

    std::uint32_t Shared(std::vector<std::uint32_t>& out, std::uint32_t id) {
        const auto carrier = carriers_.find(id);
        if (carrier == carriers_.end() || id >= madeIn_.size() || madeIn_[id] == kNone) return id;
        const std::uint32_t slot = bound_++;
        const std::uint32_t value = bound_++;
        Emit(out, kOpAccessChain, {carrierElement_[id], slot, carrier->second, zero_});
        Emit(out, kOpLoad, {typeOf_[id], value, slot});
        return value;
    }

    void Body(std::vector<std::uint32_t>& out) {
        std::vector<RegionCode> regions(regions_);
        for (std::uint32_t region = 0; region < regions_; ++region) {
            regions[region].header = bound_++;
            regions[region].body = bound_++;
            regions[region].next = next_[region] != 0 ? next_[region] : bound_++;
            regions[region].merge = bound_++;
        }
        std::vector<std::pair<std::size_t, std::uint32_t>> parents;
        std::map<std::uint32_t, std::uint32_t> tails;
        std::uint32_t current = 0;
        Fields fields;
        for (const Item& item : plan_) {
            if (item.step == Step::Open) {
                RegionCode& region = regions[item.region];
                Emit(out, kOpStore, {turn_, zero_});
                Emit(out, kOpBranch, {region.header});
                Emit(out, kOpLabel, {region.header});
                Emit(out, kOpLoopMerge, {region.merge, region.next, 0});
                Emit(out, kOpBranch, {region.body});
                Emit(out, kOpLabel, {region.body});
                current = region.body;
                region.turn = bound_++;
                Emit(out, kOpLoad, {uint_, region.turn, turn_});
                for (const std::uint32_t variable : direct_[item.region]) {
                    const std::uint32_t slot = bound_++;
                    Emit(out, kOpAccessChain, {variableCode_[variable].element, slot, variable, region.turn});
                    region.slots[variable] = slot;
                }
                std::vector<std::uint32_t> needed(needs_[item.region].begin(), needs_[item.region].end());
                std::sort(needed.begin(), needed.end(), [&](std::uint32_t left, std::uint32_t right) { return madeAt_[left] < madeAt_[right]; });
                for (const std::uint32_t id : needed) {
                    if (pointers_.count(typeOf_[id]) == 0) {
                        const std::uint32_t slot = bound_++;
                        const std::uint32_t value = bound_++;
                        Emit(out, kOpAccessChain, {carrierElement_[id], slot, carriers_[id], region.turn});
                        Emit(out, kOpLoad, {typeOf_[id], value, slot});
                        region.renamed[id] = value;
                        continue;
                    }
                    const Instruction& made = code_[madeAt_[id]];
                    Describe(made, fields);
                    std::vector<std::uint32_t> line(words_.begin() + static_cast<std::ptrdiff_t>(made.at), words_.begin() + static_cast<std::ptrdiff_t>(made.at + made.count));
                    Rewrite(line, made, fields, region);
                    line[2] = bound_++;
                    region.renamed[id] = line[2];
                    out.insert(out.end(), line.begin(), line.end());
                }
                continue;
            }
            if (item.step == Step::Close) {
                const RegionCode& region = regions[item.region];
                Flush(out, region);
                const std::uint32_t before = bound_++;
                const std::uint32_t after = bound_++;
                const std::uint32_t more = bound_++;
                Emit(out, kOpBranch, {region.next});
                Emit(out, kOpLabel, {region.next});
                Emit(out, kOpLoad, {uint_, before, turn_});
                Emit(out, kOpIAdd, {uint_, after, before, one_});
                Emit(out, kOpStore, {turn_, after});
                Emit(out, kOpULessThan, {bool_, more, after, turnCount_});
                Emit(out, kOpBranchConditional, {more, region.header, region.merge});
                Emit(out, kOpLabel, {region.merge});
                current = region.merge;
                continue;
            }
            if (item.step == Step::Link) {
                Emit(out, kOpBranch, {item.label});
                Emit(out, kOpLabel, {item.label});
                current = item.label;
                continue;
            }
            const Instruction& instruction = code_[item.instruction];
            std::vector<std::uint32_t> line(words_.begin() + static_cast<std::ptrdiff_t>(instruction.at), words_.begin() + static_cast<std::ptrdiff_t>(instruction.at + instruction.count));
            if (Ends(instruction.op)) tails[blocks_[owner_[item.instruction]].id] = current;
            if (item.step == Step::BackEdge) {
                line[1] = Shared(out, line[1]);
                const std::uint32_t header = headers_[item.instruction];
                if (line[2] == header) line[2] = item.label;
                if (line[3] == header) line[3] = item.label;
                out.insert(out.end(), line.begin(), line.end());
                Emit(out, kOpLabel, {item.label});
                Emit(out, kOpBranch, {header});
                current = item.label;
                continue;
            }
            if (item.step == Step::Sync) {
                Describe(instruction, fields);
                for (const std::uint32_t word : fields.ids) line[word] = Shared(out, line[word]);
                out.insert(out.end(), line.begin(), line.end());
                continue;
            }
            if (item.step == Step::Plain) {
                if (instruction.op == kOpVariable) {
                    const VariableCode& code = variableCode_[line[2]];
                    if (code.initial != 0) {
                        Emit(out, kOpVariable, {code.pointer, line[2], kStorageFunction, code.initial});
                    } else {
                        Emit(out, kOpVariable, {code.pointer, line[2], kStorageFunction});
                    }
                    continue;
                }
                const auto replaced = replaced_.find(item.instruction);
                if (replaced != replaced_.end()) line[2] = replaced->second;
                out.insert(out.end(), line.begin(), line.end());
                if (instruction.op == kOpLabel) {
                    current = line[1];
                    if (item.instruction == blocks_[0].label) {
                        Emit(out, kOpVariable, {wordPointer_, turn_, kStorageFunction});
                        for (const auto& [id, variable] : carriers_) Emit(out, kOpVariable, {carrierPointer_[id], variable, kStorageFunction});
                    }
                }
                continue;
            }
            const RegionCode& region = regions[item.region];
            if (instruction.op == kOpLabel) {
                out.insert(out.end(), line.begin(), line.end());
                current = line[1];
                continue;
            }
            if (instruction.op != kOpPhi) Flush(out, region);
            if (instruction.op == kOpLoad && laneIndex_ != 0 && line[3] == laneIndex_) {
                const std::uint32_t lane = bound_++;
                const std::uint32_t offset = bound_++;
                Emit(out, kOpLoad, {line[1], lane, laneIndex_});
                Emit(out, kOpIMul, {line[1], offset, region.turn, laneCount_});
                Emit(out, kOpIAdd, {line[1], line[2], lane, offset});
                if (carriers_.count(line[2]) != 0) waiting_.push_back(line[2]);
                continue;
            }
            Describe(instruction, fields);
            Rewrite(line, instruction, fields, region);
            if (instruction.op == kOpPhi) {
                for (std::uint32_t word = 4; word < line.size(); word += 2) parents.emplace_back(out.size() + word, line[word]);
            }
            out.insert(out.end(), line.begin(), line.end());
            if (fields.result && carriers_.count(line[2]) != 0) waiting_.push_back(line[2]);
        }
        for (const auto& [at, parent] : parents) {
            const auto tail = tails.find(parent);
            if (tail != tails.end()) out[at] = tail->second;
        }
    }

    void Write() {
        Declare();
        std::vector<std::uint32_t> out;
        out.reserve(words_.size() + words_.size() / 4);
        out.insert(out.end(), words_.begin(), words_.begin() + static_cast<std::ptrdiff_t>(firstFunction_));
        out[localSize_[entry_] + 3] = lanes_;
        out.insert(out.end(), declared_.begin(), declared_.end());
        out.insert(out.end(), words_.begin() + static_cast<std::ptrdiff_t>(firstFunction_), words_.begin() + static_cast<std::ptrdiff_t>(code_.front().at));
        Body(out);
        out.insert(out.end(), words_.begin() + static_cast<std::ptrdiff_t>(mainEnd_), words_.end());
        out[3] = bound_;
        words_ = std::move(out);
    }

    struct VariableCode {
        std::uint32_t pointer = 0;
        std::uint32_t element = 0;
        std::uint32_t initial = 0;
    };

    std::vector<std::uint32_t>& words_;
    std::uint32_t lanes_;
    std::string& error_;
    std::uint32_t bound_ = 0;
    std::uint32_t turns_ = 0;
    std::uint32_t entry_ = 0;
    std::uint32_t uint_ = 0;
    std::uint32_t bool_ = 0;
    std::uint32_t laneIndex_ = 0;
    std::uint32_t zero_ = 0;
    std::uint32_t one_ = 0;
    std::uint32_t turnCount_ = 0;
    std::uint32_t laneCount_ = 0;
    std::uint32_t wordPointer_ = 0;
    std::uint32_t turn_ = 0;
    std::size_t firstFunction_ = 0;
    std::size_t mainAt_ = 0;
    std::size_t mainEnd_ = 0;
    std::size_t variablesEnd_ = 0;
    std::size_t regions_ = 0;
    bool inMain_ = false;
    std::set<std::uint32_t> types_;
    std::set<std::uint32_t> wordTypes_;
    std::set<std::uint32_t> laneBound_;
    std::set<std::uint32_t> continuing_;
    std::map<std::uint32_t, std::size_t> localSize_;
    std::map<std::uint32_t, Pointer> pointers_;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> constants_;
    std::map<std::uint32_t, std::size_t> variables_;
    std::map<std::uint32_t, VariableCode> variableCode_;
    std::map<std::uint32_t, std::size_t> blockOf_;
    std::map<std::size_t, std::uint32_t> replaced_;
    std::map<std::size_t, std::uint32_t> headers_;
    std::map<std::uint32_t, std::uint32_t> carriers_;
    std::map<std::uint32_t, std::uint32_t> carrierElement_;
    std::map<std::uint32_t, std::uint32_t> carrierPointer_;
    std::map<std::uint32_t, std::uint32_t> rows_;
    std::vector<Instruction> code_;
    std::vector<Block> blocks_;
    std::vector<std::size_t> owner_;
    std::vector<Item> plan_;
    std::vector<std::uint32_t> planned_;
    std::vector<std::uint32_t> next_;
    std::vector<std::uint32_t> madeIn_;
    std::vector<std::size_t> madeAt_;
    std::vector<std::uint32_t> typeOf_;
    std::vector<std::set<std::uint32_t>> needs_;
    std::vector<std::set<std::uint32_t>> direct_;
    std::vector<std::uint32_t> waiting_;
    std::vector<std::uint32_t> declared_;
};

}

std::optional<LaneTurns> RunLaneGroupInTurns(std::vector<std::uint32_t>& words, std::uint32_t lanes, std::string& error) {
    std::vector<std::uint32_t> copy = words;
    Turns turns(copy, lanes, error);
    const std::optional<LaneTurns> result = turns.Run();
    if (result && result->turns != 0) words = std::move(copy);
    return result;
}

}
