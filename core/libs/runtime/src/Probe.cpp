#include "Probe.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace abx {

namespace {

constexpr std::uint8_t kBreakpoint = 0xCC;
constexpr std::size_t kDumpQwords = 16;

struct Watch {
    std::string reg;
    std::int64_t offset = 0;
};

struct Probe {
    std::uintptr_t address = 0;
    std::uint8_t original = 0;
    bool armed = false;
    std::vector<Watch> watches;
};

std::mutex g_lock;
std::vector<Probe> g_probes;

bool Readable(std::uintptr_t address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION region{};
    if (address == 0 || VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != sizeof(region) || region.State != MEM_COMMIT) return false;
    if ((region.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    return reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize - address >= bytes;
}

std::uint64_t Register(const CONTEXT& c, const std::string& name) {
    const struct {
        const char* name;
        std::uint64_t value;
    } registers[] = {{"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi}, {"rdi", c.Rdi}, {"rbp", c.Rbp}, {"rsp", c.Rsp},
                     {"r8", c.R8},   {"r9", c.R9},   {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12}, {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15}};
    for (const auto& entry : registers) {
        if (name == entry.name) return entry.value;
    }
    return 0;
}

LONG CALLBACK OnBreakpoint(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto at = reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    const std::lock_guard guard(g_lock);
    for (Probe& probe : g_probes) {
        if (!probe.armed || probe.address != at) continue;
        CONTEXT& c = *pointers->ContextRecord;
        AbxLog("probe", "hit at %p: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
               reinterpret_cast<void*>(at), c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
        for (const Watch& watch : probe.watches) {
            const std::uintptr_t address = static_cast<std::uintptr_t>(Register(c, watch.reg) + watch.offset);
            std::string text;
            if (Readable(address, kDumpQwords * 8)) {
                std::uint64_t values[kDumpQwords];
                std::memcpy(values, reinterpret_cast<const void*>(address), sizeof(values));
                for (const std::uint64_t value : values) {
                    char word[24];
                    std::snprintf(word, sizeof(word), " %016llx", static_cast<unsigned long long>(value));
                    text += word;
                }
            } else {
                text = " (not readable)";
            }
            AbxLog("probe", "  [%s%+lld] = %p:%s", watch.reg.c_str(), static_cast<long long>(watch.offset), reinterpret_cast<void*>(address), text.c_str());
        }
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<std::uint8_t*>(at) = probe.original;
        VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
        probe.armed = false;
        c.Rip = at;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::vector<Watch> ParseWatches(const std::string& text) {
    std::vector<Watch> watches;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(';', start);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(start, end - start);
        const std::size_t sign = item.find_first_of("+-");
        Watch watch;
        watch.reg = item.substr(0, sign);
        if (sign != std::string::npos) watch.offset = std::strtoll(item.c_str() + sign, nullptr, 16);
        if (!watch.reg.empty()) watches.push_back(watch);
        start = end + 1;
    }
    return watches;
}

}

void InstallProbes() {
    char buffer[512];
    const DWORD length = GetEnvironmentVariableA("ANYBOX_PROBE", buffer, sizeof(buffer));
    if (length == 0 || length >= sizeof(buffer)) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const std::string spec(buffer, length);
    std::size_t start = 0;
    const std::lock_guard guard(g_lock);
    while (start < spec.size()) {
        std::size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(start, end - start);
        const std::size_t at = item.find('@');
        Probe probe;
        probe.address = base + std::strtoull(item.substr(0, at).c_str(), nullptr, 16);
        if (at != std::string::npos) probe.watches = ParseWatches(item.substr(at + 1));
        DWORD old = 0;
        if (VirtualProtect(reinterpret_cast<void*>(probe.address), 1, PAGE_EXECUTE_READWRITE, &old)) {
            probe.original = *reinterpret_cast<std::uint8_t*>(probe.address);
            *reinterpret_cast<std::uint8_t*>(probe.address) = kBreakpoint;
            VirtualProtect(reinterpret_cast<void*>(probe.address), 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(probe.address), 1);
            probe.armed = true;
            AbxLog("probe", "armed at title+0x%llx with %zu memory watches", static_cast<unsigned long long>(probe.address - base), probe.watches.size());
            g_probes.push_back(std::move(probe));
        }
        start = end + 1;
    }
    if (!g_probes.empty()) AddVectoredExceptionHandler(1, OnBreakpoint);
}

}
