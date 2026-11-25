#include "CpuFixups.hpp"

#include "AbxApi.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace abx {

namespace {

constexpr std::uint8_t kOperandSizePrefix = 0x66;
constexpr std::uint8_t kTwoByteEscape = 0x0F;
constexpr std::uint8_t kGroup15 = 0xAE;
constexpr std::uint8_t kCacheWritebackReg = 6;
constexpr std::uint8_t kShortJump = 0xEB;
constexpr std::size_t kMaxInstruction = 15;

std::size_t AddressingBytes(std::uint8_t modrm, const std::uint8_t* rest, std::size_t available) {
    const std::uint8_t mod = modrm >> 6;
    const std::uint8_t rm = modrm & 7;
    std::size_t bytes = 0;
    if (rm == 4) {
        if (available < 1) return SIZE_MAX;
        const std::uint8_t base = rest[0] & 7;
        bytes = 1;
        if (mod == 0 && base == 5) return bytes + 4;
    } else if (mod == 0 && rm == 5) {
        return 4;
    }
    if (mod == 1) return bytes + 1;
    if (mod == 2) return bytes + 4;
    return bytes;
}

struct ImageRange {
    std::uintptr_t start = 0;
    std::uintptr_t end = 0;
};

ImageRange TitleImage() {
    static const ImageRange range = [] {
        ImageRange found;
        const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        if (base == nullptr) return found;
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, base, sizeof(dos));
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, base + dos.e_lfanew, sizeof(nt));
        found.start = reinterpret_cast<std::uintptr_t>(base);
        found.end = found.start + nt.OptionalHeader.SizeOfImage;
        return found;
    }();
    return range;
}

std::mutex g_patchLock;
std::atomic<unsigned> g_patched{0};

bool PatchToJump(std::uint8_t* at, std::size_t length) {
    DWORD previous = 0;
    if (!VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &previous)) return false;
    const SHORT jump = static_cast<SHORT>(kShortJump | (static_cast<unsigned>(length - 2) << 8));
#if defined(_MSC_VER)
    _InterlockedExchange16(reinterpret_cast<short volatile*>(at), jump);
#else
    __atomic_exchange_n(reinterpret_cast<short volatile*>(at), jump, __ATOMIC_SEQ_CST);
#endif
    DWORD ignored = 0;
    VirtualProtect(at, 2, previous, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, length);
    return true;
}

constexpr std::size_t kCacheLine = 64;

void ReportOnce(std::atomic<bool>& reported, const char* text, std::uintptr_t offset) {
    if (!reported.exchange(true)) {
        AbxLog("runtime", "title+0x%llx: %s (later sites are handled the same way without a log line)", static_cast<unsigned long long>(offset), text);
    }
}

std::uint64_t InsertBits(std::uint64_t destination, std::uint64_t source, std::uint32_t length, std::uint32_t index) {
    length &= 0x3F;
    index &= 0x3F;
    const std::uint64_t mask = length == 0 ? ~std::uint64_t{0} : ((std::uint64_t{1} << length) - 1);
    return (destination & ~(mask << index)) | ((source & mask) << index);
}

bool EmulateAmdOnly(const std::uint8_t* code, std::size_t available, CONTEXT& context, std::uintptr_t imageStart) {
    static std::atomic<bool> zeroReported{false};
    static std::atomic<bool> monitorReported{false};
    static std::atomic<bool> insertReported{false};
    const std::uintptr_t offset = context.Rip - imageStart;
    if (available >= 3 && code[0] == 0x0F && code[1] == 0x01) {
        if (code[2] == 0xFC) {
            std::memset(reinterpret_cast<void*>(context.Rax & ~static_cast<std::uint64_t>(kCacheLine - 1)), 0, kCacheLine);
            context.Rip += 3;
            ReportOnce(zeroReported, "clzero is not supported by this CPU; the 64-byte line is zeroed by AnyBox", offset);
            return true;
        }
        if (code[2] == 0xFA || code[2] == 0xFB) {
            if (code[2] == 0xFB) YieldProcessor();
            context.Rip += 3;
            ReportOnce(monitorReported, "monitorx/mwaitx are not supported by this CPU; the wait returns at once, as a spurious wake-up may", offset);
            return true;
        }
    }
    if (available >= 5 && code[0] == 0xF2) {
        std::size_t at = 1;
        std::uint8_t rex = 0;
        if ((code[at] & 0xF0) == 0x40) rex = code[at++];
        if (available < at + 3 || code[at] != 0x0F || (code[at + 1] != 0x78 && code[at + 1] != 0x79)) return false;
        const bool immediate = code[at + 1] == 0x78;
        const std::uint8_t modrm = code[at + 2];
        if ((modrm >> 6) != 3) return false;
        const std::uint32_t destination = ((modrm >> 3) & 7) | ((rex & 0x4) != 0 ? 8u : 0u);
        const std::uint32_t source = (modrm & 7) | ((rex & 0x1) != 0 ? 8u : 0u);
        const std::size_t length = at + 3 + (immediate ? 2 : 0);
        if (available < length) return false;
        M128A* xmm = &context.Xmm0;
        const std::uint64_t field = static_cast<std::uint64_t>(xmm[source].Low);
        std::uint32_t bits = 0;
        std::uint32_t index = 0;
        if (immediate) {
            bits = code[at + 3];
            index = code[at + 4];
        } else {
            const std::uint64_t high = static_cast<std::uint64_t>(xmm[source].High);
            bits = static_cast<std::uint32_t>(high & 0x3F);
            index = static_cast<std::uint32_t>((high >> 8) & 0x3F);
        }
        xmm[destination].Low = InsertBits(static_cast<std::uint64_t>(xmm[destination].Low), field, bits, index);
        context.Rip += length;
        ReportOnce(insertReported, "insertq (SSE4a) is not supported by this CPU; AnyBox emulates it", offset);
        return true;
    }
    return false;
}

LONG WINAPI OnIllegalInstruction(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    const ImageRange image = TitleImage();
    const std::uintptr_t rip = pointers->ContextRecord->Rip;
    if (rip < image.start || rip >= image.end) return EXCEPTION_CONTINUE_SEARCH;
    auto* code = reinterpret_cast<std::uint8_t*>(rip);
    const std::size_t available = (std::min)(kMaxInstruction, static_cast<std::size_t>(image.end - rip));
    if (EmulateAmdOnly(code, available, *pointers->ContextRecord, image.start)) return EXCEPTION_CONTINUE_EXECUTION;
    const std::lock_guard guard(g_patchLock);
    if (code[0] == kShortJump) return EXCEPTION_CONTINUE_EXECUTION;
    const std::size_t length = CacheLineWritebackLength(code, available);
    if (length == 0 || !PatchToJump(code, length)) return EXCEPTION_CONTINUE_SEARCH;
    ++g_patched;
    AbxLog("runtime", "title+0x%llx: clwb is not supported by this CPU; the instruction is skipped (memory AnyBox shares with the GPU is host-coherent)",
           static_cast<unsigned long long>(rip - image.start));
    return EXCEPTION_CONTINUE_EXECUTION;
}

}

std::size_t CacheLineWritebackLength(const std::uint8_t* code, std::size_t available) {
    if (code == nullptr || available < 4 || code[0] != kOperandSizePrefix) return 0;
    std::size_t at = 1;
    if ((code[at] & 0xF0) == 0x40) ++at;
    if (available < at + 3 || code[at] != kTwoByteEscape || code[at + 1] != kGroup15) return 0;
    const std::uint8_t modrm = code[at + 2];
    if ((modrm >> 6) == 3 || ((modrm >> 3) & 7) != kCacheWritebackReg) return 0;
    at += 3;
    const std::size_t extra = AddressingBytes(modrm, code + at, available - at);
    if (extra == SIZE_MAX || available < at + extra) return 0;
    return at + extra;
}

void InstallCpuFixups() {
    AddVectoredExceptionHandler(1, OnIllegalInstruction);
}

}
