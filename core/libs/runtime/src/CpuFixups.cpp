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

LONG WINAPI OnIllegalInstruction(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    const ImageRange image = TitleImage();
    const std::uintptr_t rip = pointers->ContextRecord->Rip;
    if (rip < image.start || rip >= image.end) return EXCEPTION_CONTINUE_SEARCH;
    auto* code = reinterpret_cast<std::uint8_t*>(rip);
    const std::size_t available = (std::min)(kMaxInstruction, static_cast<std::size_t>(image.end - rip));
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
