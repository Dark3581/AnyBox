#include "MemoryWrites.hpp"

#include <cstddef>
#include <unordered_map>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::size_t kHeaderWords = 5;
constexpr std::uint32_t kOpUndef = 1;
constexpr std::uint32_t kOpTypePointer = 32;
constexpr std::uint32_t kOpTypeForwardPointer = 39;
constexpr std::uint32_t kOpFunctionParameter = 55;
constexpr std::uint32_t kOpFunctionCall = 57;
constexpr std::uint32_t kOpVariable = 59;
constexpr std::uint32_t kOpImageTexelPointer = 60;
constexpr std::uint32_t kOpLoad = 61;
constexpr std::uint32_t kOpStore = 62;
constexpr std::uint32_t kOpCopyMemory = 63;
constexpr std::uint32_t kOpCopyMemorySized = 64;
constexpr std::uint32_t kOpAccessChain = 65;
constexpr std::uint32_t kOpInBoundsAccessChain = 66;
constexpr std::uint32_t kOpPtrAccessChain = 67;
constexpr std::uint32_t kOpInBoundsPtrAccessChain = 70;
constexpr std::uint32_t kOpCompositeExtract = 81;
constexpr std::uint32_t kOpCopyObject = 83;
constexpr std::uint32_t kOpImageWrite = 99;
constexpr std::uint32_t kOpConvertUToPtr = 120;
constexpr std::uint32_t kOpBitcast = 124;
constexpr std::uint32_t kOpSelect = 169;
constexpr std::uint32_t kOpAtomicStore = 228;
constexpr std::uint32_t kOpAtomicExchange = 229;
constexpr std::uint32_t kOpAtomicXor = 242;
constexpr std::uint32_t kOpPhi = 245;
constexpr std::uint32_t kOpAtomicFlagTestAndSet = 318;
constexpr std::uint32_t kOpAtomicFlagClear = 319;
constexpr std::uint32_t kOpAtomicFMinEXT = 5614;
constexpr std::uint32_t kOpAtomicFMaxEXT = 5615;
constexpr std::uint32_t kOpAtomicFAddEXT = 6035;
constexpr std::uint32_t kStorageInput = 1;
constexpr std::uint32_t kStorageOutput = 3;
constexpr std::uint32_t kStorageWorkgroup = 4;
constexpr std::uint32_t kStoragePrivate = 6;
constexpr std::uint32_t kStorageFunction = 7;
constexpr std::uint32_t kStorageTaskPayload = 5402;

bool DefinesTypedValue(std::uint32_t op) {
    switch (op) {
    case kOpUndef:
    case kOpFunctionParameter:
    case kOpFunctionCall:
    case kOpVariable:
    case kOpImageTexelPointer:
    case kOpLoad:
    case kOpAccessChain:
    case kOpInBoundsAccessChain:
    case kOpPtrAccessChain:
    case kOpInBoundsPtrAccessChain:
    case kOpCompositeExtract:
    case kOpCopyObject:
    case kOpConvertUToPtr:
    case kOpBitcast:
    case kOpSelect:
    case kOpPhi: return true;
    default: return false;
    }
}

bool LocalStorage(std::uint32_t storage) {
    return storage == kStorageInput || storage == kStorageOutput || storage == kStorageWorkgroup || storage == kStoragePrivate || storage == kStorageFunction ||
           storage == kStorageTaskPayload;
}

std::size_t WrittenPointerOperand(std::uint32_t op) {
    if (op == kOpStore || op == kOpCopyMemory || op == kOpCopyMemorySized || op == kOpAtomicStore || op == kOpAtomicFlagClear) return 1;
    if ((op >= kOpAtomicExchange && op <= kOpAtomicXor) || op == kOpAtomicFlagTestAndSet || op == kOpAtomicFMinEXT || op == kOpAtomicFMaxEXT || op == kOpAtomicFAddEXT) return 3;
    return 0;
}

}

bool MayWriteMemory(const std::vector<std::uint32_t>& words) {
    if (words.size() < kHeaderWords || words[0] != kMagic) return true;
    std::unordered_map<std::uint32_t, std::uint32_t> pointerStorage;
    std::unordered_map<std::uint32_t, std::uint32_t> valueType;
    for (std::size_t at = kHeaderWords; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        const std::uint32_t op = words[at] & 0xFFFFu;
        if (count == 0 || count > words.size() - at) return true;
        if ((op == kOpTypePointer || op == kOpTypeForwardPointer) && count >= 3) pointerStorage[words[at + 1]] = words[at + 2];
        if (DefinesTypedValue(op) && count >= 3) valueType[words[at + 2]] = words[at + 1];
        at += count;
    }
    const auto local = [&](std::uint32_t pointer) {
        const auto type = valueType.find(pointer);
        if (type == valueType.end()) return false;
        const auto storage = pointerStorage.find(type->second);
        return storage != pointerStorage.end() && LocalStorage(storage->second);
    };
    for (std::size_t at = kHeaderWords; at < words.size();) {
        const std::uint32_t count = words[at] >> 16;
        const std::uint32_t op = words[at] & 0xFFFFu;
        if (op == kOpImageWrite) return true;
        const std::size_t operand = WrittenPointerOperand(op);
        if (operand != 0) {
            if (operand >= count || !local(words[at + operand])) return true;
        }
        at += count;
    }
    return false;
}

}
