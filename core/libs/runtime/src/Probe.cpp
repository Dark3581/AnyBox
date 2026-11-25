#include "Probe.hpp"

#include "AbxApi.hpp"

#include <windows.h>

#include <algorithm>
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
    bool text = false;
};

struct Probe {
    std::uintptr_t address = 0;
    std::uint8_t original = 0;
    bool armed = false;
    std::uint32_t hits = 0;
    std::uint32_t limit = 1;
    std::vector<Watch> watches;
};

constexpr DWORD kTrapFlag = 0x100;

std::mutex g_lock;
std::vector<Probe> g_probes;
thread_local Probe* t_rearm = nullptr;

void WriteByte(std::uintptr_t at, std::uint8_t value) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<std::uint8_t*>(at) = value;
    VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
}

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
    if (name == "title") return reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    return 0;
}

LONG CALLBACK OnBreakpoint(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm != nullptr) {
        const std::lock_guard guard(g_lock);
        if (t_rearm->armed) WriteByte(t_rearm->address, kBreakpoint);
        t_rearm = nullptr;
        pointers->ContextRecord->EFlags &= ~kTrapFlag;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    const auto at = reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress);
    const std::lock_guard guard(g_lock);
    for (Probe& probe : g_probes) {
        if (!probe.armed || probe.address != at) continue;
        CONTEXT& c = *pointers->ContextRecord;
        ++probe.hits;
        AbxLog("probe", "hit %u at %p: rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx rsp %llx r8 %llx r9 %llx r10 %llx r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
               probe.hits, reinterpret_cast<void*>(at), c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);
        for (const Watch& watch : probe.watches) {
            const std::uintptr_t address = static_cast<std::uintptr_t>(Register(c, watch.reg) + watch.offset);
            std::string text;
            if (watch.text) {
                char characters[97]{};
                if (Readable(address, 1)) {
                    for (std::size_t index = 0; index + 1 < sizeof(characters) && Readable(address + index, 1); ++index) {
                        const char value = reinterpret_cast<const char*>(address)[index];
                        if (value == 0) break;
                        characters[index] = value >= 32 && value < 127 ? value : '.';
                    }
                }
                AbxLog("probe", "  \"%s\" at [%s%+lld]", characters, watch.reg.c_str(), static_cast<long long>(watch.offset));
                continue;
            }
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
        WriteByte(at, probe.original);
        probe.armed = probe.hits < probe.limit;
        if (probe.armed) {
            t_rearm = &probe;
            c.EFlags |= kTrapFlag;
        }
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
        watch.text = item.rfind("s:", 0) == 0;
        watch.reg = item.substr(watch.text ? 2 : 0, sign == std::string::npos ? std::string::npos : sign - (watch.text ? 2 : 0));
        if (sign != std::string::npos) watch.offset = std::strtoll(item.c_str() + sign, nullptr, 16);
        if (!watch.reg.empty()) watches.push_back(watch);
        start = end + 1;
    }
    return watches;
}

}

void InstallProbes() {
    const DWORD needed = GetEnvironmentVariableA("ANYBOX_PROBE", nullptr, 0);
    if (needed <= 1) return;
    std::string spec(needed, ' ');
    const DWORD length = GetEnvironmentVariableA("ANYBOX_PROBE", spec.data(), needed);
    if (length == 0 || length >= needed) return;
    spec.resize(length);
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::size_t start = 0;
    const std::lock_guard guard(g_lock);
    while (start < spec.size()) {
        std::size_t end = spec.find(',', start);
        if (end == std::string::npos) end = spec.size();
        const std::string item = spec.substr(start, end - start);
        const std::size_t at = item.find('@');
        Probe probe;
        const std::string head = item.substr(0, at);
        const std::size_t count = head.find('#');
        probe.address = base + std::strtoull(head.substr(0, count).c_str(), nullptr, 16);
        if (count != std::string::npos) probe.limit = static_cast<std::uint32_t>((std::max)(1ul, std::strtoul(head.c_str() + count + 1, nullptr, 10)));
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
