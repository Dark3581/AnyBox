#include "ThreadStacks.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex g_first;
std::mutex g_second;
std::atomic<bool> g_stop{false};
volatile std::uint64_t g_sink = 0;
int g_failures = 0;

void Expect(bool condition, const std::string& what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

[[gnu::noinline]] void HoldFirstWantSecond() {
    std::lock_guard<std::mutex> first(g_first);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> second(g_second);
    g_sink = g_sink + 1;
}

[[gnu::noinline]] void HoldSecondWantFirst() {
    std::lock_guard<std::mutex> second(g_second);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::lock_guard<std::mutex> first(g_first);
    g_sink = g_sink + 1;
}

[[gnu::noinline]] void SpinForever() {
    while (!g_stop.load()) g_sink = g_sink + 3;
}

[[gnu::noinline]] int Deep(int depth) {
    if (depth == 0) {
        Sleep(600000);
        return 1;
    }
    volatile char pad[512];
    pad[0] = static_cast<char>(depth);
    return Deep(depth - 1) + pad[0];
}

bool Holds(const std::vector<std::string>& lines, const char* first, const char* second = "") {
    for (const std::string& line : lines) {
        if (line.find(first) != std::string::npos && line.find(second) != std::string::npos) return true;
    }
    return false;
}

}

int main() {
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<std::thread> threads;
    threads.emplace_back(HoldFirstWantSecond);
    threads.emplace_back(HoldSecondWantFirst);
    threads.emplace_back(SpinForever);
    for (int index = 0; index < 5; ++index) threads.emplace_back([event] { WaitForSingleObject(event, INFINITE); });
    threads.emplace_back([] { g_sink = g_sink + static_cast<std::uint64_t>(Deep(40)); });
    threads.emplace_back([] {
        SetLastError(ERROR_NOACCESS);
        SleepEx(600000, FALSE);
    });
    Sleep(800);
    for (int round = 0; round < 3; ++round) {
        const std::vector<std::string> lines = abx::d3d12x::DescribeThreadStacks(300, 1800);
        const std::string tag = "round " + std::to_string(round) + ": ";
        std::size_t longest = 0;
        bool spinning = false;
        for (const std::string& line : lines) {
            longest = (std::max)(longest, line.size());
            spinning = spinning || (line.find("SpinForever") != std::string::npos && line.find("(0 ms CPU)") == std::string::npos);
        }
        Expect(spinning, tag + "the spinning thread is named by its function and shows CPU time");
        Expect(Holds(lines, "HoldFirstWantSecond", "pthread_mutex_lock") || Holds(lines, "HoldFirstWantSecond", "ntdll.dll!"), tag + "the first deadlocked thread shows its function under a wait");
        Expect(Holds(lines, "HoldSecondWantFirst"), tag + "the second deadlocked thread shows its function");
        Expect(Holds(lines, "5 threads with the same stack", "WaitForSingleObject"), tag + "five threads with one stack share a line");
        Expect(Holds(lines, "Deep(int)", "Sleep"), tag + "a deep stack is walked from the sleep into the recursion");
        Expect(Holds(lines, "ntdll.dll!RtlUserThreadStart"), tag + "stacks reach the thread start by its export name");
        Expect(longest <= 1900, tag + "no line is longer than the log takes");
        Expect(Holds(lines, "last errors are not shown") || Holds(lines, "last error 998", "Sleep"), tag + "a thread's last error is shown where it can be read");
        if (g_failures != 0 && round == 0) {
            for (const std::string& line : lines) std::printf("%s\n", line.c_str());
        }
    }
    const std::string own = abx::d3d12x::DescribeCode(reinterpret_cast<std::uint64_t>(reinterpret_cast<void*>(&SpinForever)));
    Expect(own.find("SpinForever") != std::string::npos && own.find("+0x0") != std::string::npos, "a function of this module is named from its symbol table: " + own);
    const std::string sleep = abx::d3d12x::DescribeCode(reinterpret_cast<std::uint64_t>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "Sleep"))));
    Expect(sleep.find("!Sleep+0x0") != std::string::npos, "an export of another module is named: " + sleep);
    Expect(abx::d3d12x::DescribeCode(0x10) == "0x10", "an address in no module stays a number");
    std::printf("%d failure(s)\n", g_failures);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), g_failures == 0 ? 0 : 1);
    return 1;
}
