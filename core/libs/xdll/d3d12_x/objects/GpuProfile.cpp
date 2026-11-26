#include "GpuProfile.hpp"

#include "AbxApi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace abx::d3d12x {

namespace {

constexpr std::uint32_t kSlots = 1u << 16;
constexpr std::uint32_t kFreeSlotsToStart = 1u << 14;
constexpr auto kAnswerLimit = std::chrono::seconds(15);
constexpr std::size_t kTopPipelines = 16;

enum class MarkKind : std::uint8_t { ListBegin, Work, ListEnd };

struct Mark {
    std::uint64_t list = 0;
    const char* function = nullptr;
    std::string shaders;
    MarkKind kind = MarkKind::ListBegin;
};

enum class State { Off, Idle, Active, Collecting };

struct Profile {
    const VulkanDevice* device = nullptr;
    PFN_vkCmdWriteTimestamp writeTimestamp = nullptr;
    PFN_vkCmdResetQueryPool resetQueryPool = nullptr;
    PFN_vkGetQueryPoolResults queryResults = nullptr;
    VkQueryPool pool = VK_NULL_HANDLE;
    double nanosecondsPerTick = 0.0;
    std::chrono::seconds period{0};
    std::mutex lock;
    std::vector<Mark> marks;
    State state = State::Off;
    std::uint32_t generation = 0;
    std::uint32_t first = 0;
    std::uint32_t cursor = 0;
    std::uint64_t lists = 0;
    std::uint64_t frame = 0;
    bool answeredAll = true;
    std::chrono::steady_clock::time_point nextAt{};
    std::chrono::steady_clock::time_point frameBegin{};
    std::chrono::steady_clock::time_point frameEnd{};
    std::optional<VulkanDevice::PendingSubmit> reset;
};

std::atomic<std::uint32_t> g_open{0};

Profile& Instance() {
    static Profile* profile = new Profile();
    return *profile;
}

bool ResetPool(Profile& profile) {
    if (profile.reset) {
        if (profile.device->Finish(*profile.reset, false) == VK_NOT_READY) return false;
        profile.reset.reset();
    }
    std::string error;
    const VkQueryPool pool = profile.pool;
    const PFN_vkCmdResetQueryPool reset = profile.resetQueryPool;
    profile.reset = profile.device->SubmitWithoutWaiting([pool, reset](VkCommandBuffer commands) { reset(commands, pool, 0, kSlots); }, error);
    if (!profile.reset) {
        AbxLog("d3d12_x", "GPU profile: the timestamp queries could not be reset (%s); no further frame is profiled", error.c_str());
        profile.state = State::Off;
        return false;
    }
    profile.first = 0;
    profile.cursor = 0;
    profile.answeredAll = true;
    return true;
}

void WriteMark(Profile& profile, VkCommandBuffer commands, std::uint32_t generation, Mark mark) {
    std::uint32_t slot = 0;
    {
        const std::lock_guard guard(profile.lock);
        if (generation != profile.generation || (profile.state != State::Active && profile.state != State::Collecting) || profile.cursor >= kSlots) return;
        slot = profile.cursor++;
        profile.marks[slot] = std::move(mark);
    }
    profile.writeTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profile.pool, slot);
}

struct Spent {
    double milliseconds = 0.0;
    double longest = 0.0;
    std::uint64_t calls = 0;
};

void Report(Profile& profile, const std::vector<std::uint64_t>& answers, std::uint32_t count) {
    const auto answered = [&](std::uint32_t index) { return answers[static_cast<std::size_t>(index) * 2 + 1] != 0; };
    const auto ticks = [&](std::uint32_t index) { return answers[static_cast<std::size_t>(index) * 2]; };
    std::map<std::uint64_t, std::vector<std::uint32_t>> byList;
    std::uint32_t missing = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        byList[profile.marks[profile.first + index].list].push_back(index);
        if (!answered(index)) ++missing;
    }
    std::map<std::string, Spent> byPipeline;
    std::map<std::string, Spent> byCall;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t works = 0;
    double longestList = 0.0;
    std::size_t longestListCalls = 0;
    for (const auto& [list, indices] : byList) {
        std::optional<std::uint64_t> begin;
        std::optional<std::uint64_t> end;
        for (std::size_t at = 0; at < indices.size(); ++at) {
            const std::uint32_t index = indices[at];
            const Mark& mark = profile.marks[profile.first + index];
            if (answered(index)) {
                if (!begin) begin = ticks(index);
                end = ticks(index);
            }
            if (mark.kind == MarkKind::Work) ++works;
            if (at + 1 >= indices.size() || !answered(index) || !answered(indices[at + 1]) || ticks(indices[at + 1]) < ticks(index)) continue;
            const double milliseconds = static_cast<double>(ticks(indices[at + 1]) - ticks(index)) * profile.nanosecondsPerTick / 1.0e6;
            const std::string call = mark.kind == MarkKind::Work ? std::string(mark.function != nullptr ? mark.function : "work") : std::string("list start to its first draw or dispatch");
            const std::string pipeline = mark.kind == MarkKind::Work ? call + " " + mark.shaders : call;
            for (Spent* spent : {&byPipeline[pipeline], &byCall[call]}) {
                spent->milliseconds += milliseconds;
                spent->longest = (std::max)(spent->longest, milliseconds);
                ++spent->calls;
            }
        }
        if (begin && end && *end >= *begin) {
            spans.emplace_back(*begin, *end);
            const double milliseconds = static_cast<double>(*end - *begin) * profile.nanosecondsPerTick / 1.0e6;
            if (milliseconds > longestList) {
                longestList = milliseconds;
                longestListCalls = indices.size();
            }
        }
    }
    std::sort(spans.begin(), spans.end());
    std::uint64_t busy = 0;
    std::uint64_t coveredTo = 0;
    for (const auto& [begin, end] : spans) {
        const std::uint64_t from = (std::max)(begin, coveredTo);
        if (end > from) busy += end - from;
        coveredTo = (std::max)(coveredTo, end);
    }
    const double busyMs = static_cast<double>(busy) * profile.nanosecondsPerTick / 1.0e6;
    const double spanMs = spans.empty() ? 0.0 : static_cast<double>(coveredTo - spans.front().first) * profile.nanosecondsPerTick / 1.0e6;
    const double frameMs = std::chrono::duration<double, std::milli>(profile.frameEnd - profile.frameBegin).count();
    AbxLog("d3d12_x",
           "GPU profile of frame %llu (%.1f ms from present to present; one frame every %lld s, d3d12_x.gpu_profile_seconds): %zu command lists begun in that frame, %llu draws and "
           "dispatches timed, %u of %u timestamps not answered; the GPU spent %.1f ms inside these lists, %.1f ms from the first one's start to the last one's end; the "
           "longest list took %.1f ms (%zu timestamps)",
           static_cast<unsigned long long>(profile.frame), frameMs, static_cast<long long>(profile.period.count()), byList.size(), static_cast<unsigned long long>(works), missing,
           count, busyMs, spanMs, longestList, longestListCalls);
    std::string calls;
    for (const auto& [call, spent] : byCall) {
        char text[224];
        std::snprintf(text, sizeof(text), "%s%s %.1f ms in %llu", calls.empty() ? "" : " | ", call.c_str(), spent.milliseconds, static_cast<unsigned long long>(spent.calls));
        calls += text;
    }
    AbxLog("d3d12_x", "GPU profile by call (time from each call to the next timestamp of its list, so barriers, copies and clears that follow count with it): %s", calls.c_str());
    std::vector<std::pair<std::string, Spent>> ordered(byPipeline.begin(), byPipeline.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) { return left.second.milliseconds > right.second.milliseconds; });
    for (std::size_t index = 0; index < ordered.size() && index < kTopPipelines; ++index) {
        const Spent& spent = ordered[index].second;
        AbxLog("d3d12_x", "GPU profile %2zu: %8.1f ms, %5.1f %% of the lists' time, %llu call(s), longest %.1f ms: %s", index + 1, spent.milliseconds,
               busyMs > 0.0 ? spent.milliseconds * 100.0 / busyMs : 0.0, static_cast<unsigned long long>(spent.calls), spent.longest, ordered[index].first.c_str());
    }
}

}

void GpuProfile::Start(const VulkanDevice& device) {
    Profile& profile = Instance();
    const std::lock_guard guard(profile.lock);
    if (profile.device != nullptr) return;
    profile.device = &device;
    const char* value = AbxSetting("d3d12_x.gpu_profile_seconds");
    const unsigned long seconds = value != nullptr ? std::strtoul(value, nullptr, 10) : 30;
    if (seconds == 0) {
        AbxLog("d3d12_x", "GPU profile is off (d3d12_x.gpu_profile_seconds = 0)");
        return;
    }
    const float tick = device.Properties().limits.timestampPeriod;
    const auto create = reinterpret_cast<PFN_vkCreateQueryPool>(device.DeviceLoader()(device.Handle(), "vkCreateQueryPool"));
    profile.writeTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(device.DeviceLoader()(device.Handle(), "vkCmdWriteTimestamp"));
    profile.resetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(device.DeviceLoader()(device.Handle(), "vkCmdResetQueryPool"));
    profile.queryResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(device.DeviceLoader()(device.Handle(), "vkGetQueryPoolResults"));
    if (!(tick > 0.0f) || device.Properties().limits.timestampComputeAndGraphics != VK_TRUE || create == nullptr || profile.writeTimestamp == nullptr ||
        profile.resetQueryPool == nullptr || profile.queryResults == nullptr) {
        AbxLog("d3d12_x", "GPU profile is off: the device has no timestamp queries on its graphics and compute queues");
        return;
    }
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kSlots;
    if (create(device.Handle(), &info, nullptr, &profile.pool) != VK_SUCCESS) {
        profile.pool = VK_NULL_HANDLE;
        AbxLog("d3d12_x", "GPU profile is off: vkCreateQueryPool failed");
        return;
    }
    profile.nanosecondsPerTick = static_cast<double>(tick);
    profile.period = std::chrono::seconds(seconds);
    profile.marks.resize(kSlots);
    profile.state = State::Idle;
    profile.cursor = kSlots;
    profile.answeredAll = true;
    profile.nextAt = std::chrono::steady_clock::now() + profile.period;
    AbxLog("d3d12_x",
           "GPU profile: every %lu s the command lists begun during one frame carry GPU timestamps before each draw and dispatch, and the log names the pipelines the GPU spent "
           "that frame on (d3d12_x.gpu_profile_seconds, 0 turns it off)",
           seconds);
}

std::uint32_t GpuProfile::BeginList(VkCommandBuffer commands, std::uint64_t& list) {
    const std::uint32_t generation = g_open.load(std::memory_order_acquire);
    if (generation == 0) return 0;
    Profile& profile = Instance();
    {
        const std::lock_guard guard(profile.lock);
        if (generation != profile.generation || profile.state != State::Active) return 0;
        list = ++profile.lists;
    }
    Mark mark;
    mark.list = list;
    mark.kind = MarkKind::ListBegin;
    WriteMark(profile, commands, generation, std::move(mark));
    return generation;
}

void GpuProfile::MarkWork(VkCommandBuffer commands, std::uint32_t generation, std::uint64_t list, const char* function, const std::vector<std::string>& shaders) {
    Mark mark;
    mark.list = list;
    mark.function = function;
    mark.kind = MarkKind::Work;
    for (const std::string& shader : shaders) mark.shaders += (mark.shaders.empty() ? "" : " ") + shader.substr(0, 20);
    WriteMark(Instance(), commands, generation, std::move(mark));
}

void GpuProfile::EndList(VkCommandBuffer commands, std::uint32_t generation, std::uint64_t list) {
    Mark mark;
    mark.list = list;
    mark.kind = MarkKind::ListEnd;
    WriteMark(Instance(), commands, generation, std::move(mark));
}

void GpuProfile::FramePresented(std::uint64_t frame) {
    Profile& profile = Instance();
    const std::lock_guard guard(profile.lock);
    if (profile.state == State::Off) return;
    const auto now = std::chrono::steady_clock::now();
    if (profile.state == State::Active) {
        g_open.store(0, std::memory_order_release);
        profile.frameEnd = now;
        profile.state = State::Collecting;
        return;
    }
    if (profile.state == State::Collecting) {
        const std::uint32_t count = profile.cursor - profile.first;
        std::vector<std::uint64_t> answers(static_cast<std::size_t>(count) * 2, 0);
        bool all = count != 0;
        if (count != 0) {
            const VkResult read = profile.queryResults(profile.device->Handle(), profile.pool, profile.first, count, answers.size() * sizeof(std::uint64_t), answers.data(),
                                                       2 * sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (read != VK_SUCCESS && read != VK_NOT_READY) {
                AbxLog("d3d12_x", "GPU profile: vkGetQueryPoolResults failed with %d; no further frame is profiled", static_cast<int>(read));
                profile.state = State::Off;
                return;
            }
            for (std::uint32_t index = 0; index < count && all; ++index) all = answers[static_cast<std::size_t>(index) * 2 + 1] != 0;
        }
        if (!all && count != 0 && now - profile.frameEnd < kAnswerLimit) return;
        if (count != 0) Report(profile, answers, count);
        profile.answeredAll = profile.answeredAll && (all || count == 0);
        profile.state = State::Idle;
        profile.nextAt = now + profile.period;
        return;
    }
    if (now < profile.nextAt) return;
    if (profile.cursor + kFreeSlotsToStart > kSlots) {
        if (!profile.answeredAll) {
            AbxLog("d3d12_x", "GPU profile: lists of an earlier profiled frame never ran, so their timestamp queries cannot be used again, and too few are left; no further frame is "
                              "profiled");
            profile.state = State::Off;
            return;
        }
        if (!ResetPool(profile)) return;
    }
    profile.first = profile.cursor;
    profile.frame = frame;
    profile.frameBegin = now;
    profile.state = State::Active;
    g_open.store(++profile.generation, std::memory_order_release);
}

}
