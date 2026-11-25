#include "TitleAsserts.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>

namespace abx {

namespace {

constexpr std::uint8_t kBreakpoint = 0xCC;
constexpr std::uint8_t kShortConditionalFirst = 0x70;
constexpr std::uint8_t kShortConditionalLast = 0x7F;
constexpr std::uint8_t kTwoByteEscape = 0x0F;
constexpr std::uint8_t kNearConditionalFirst = 0x80;
constexpr std::uint8_t kNearConditionalLast = 0x8F;

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

bool Enabled() {
    static const bool enabled = [] {
        const char* value = AbxSetting("runtime.continue_title_asserts");
        if (value == nullptr) return false;
        const std::string_view text(value);
        return text == "1" || text == "true" || text == "yes";
    }();
    return enabled;
}

std::mutex g_lock;
std::set<std::uintptr_t> g_reported;

LONG CALLBACK OnTitleBreak(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto at = reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    const ImageRange image = TitleImage();
    if (at < image.start || at >= image.end || !Enabled()) return EXCEPTION_CONTINUE_SEARCH;
    if (!IsSkippableAssertBreak(reinterpret_cast<const std::uint8_t*>(at), at - image.start)) return EXCEPTION_CONTINUE_SEARCH;
    {
        const std::lock_guard guard(g_lock);
        if (g_reported.insert(at).second) {
            AbxLog("runtime",
                   "title+0x%llx: the title's own check failed and reached its breakpoint; continuing on the path the check skips "
                   "(runtime.continue_title_asserts; later hits of this site are not logged)",
                   static_cast<unsigned long long>(at - image.start));
        }
    }
    pointers->ContextRecord->Rip = at + 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

}

bool IsSkippableAssertBreak(const std::uint8_t* breakpoint, std::size_t bytesBefore) {
    if (breakpoint == nullptr || breakpoint[0] != kBreakpoint) return false;
    if (bytesBefore >= 2 && breakpoint[-2] >= kShortConditionalFirst && breakpoint[-2] <= kShortConditionalLast && breakpoint[-1] == 0x01) return true;
    if (bytesBefore >= 6 && breakpoint[-6] == kTwoByteEscape && breakpoint[-5] >= kNearConditionalFirst && breakpoint[-5] <= kNearConditionalLast) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, breakpoint - 4, sizeof(displacement));
        return displacement == 1;
    }
    return false;
}

void InstallTitleAssertPolicy() {
    AddVectoredExceptionHandler(0, OnTitleBreak);
}

}
