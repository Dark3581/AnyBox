#include "xdll/xgameruntime/xgameruntime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t HandleSignature = 0x58545155;
constexpr HRESULT InvalidHandle = static_cast<HRESULT>(0x8924010Cu);

using Clock = std::chrono::steady_clock;

struct Entry {
    void* context;
    XTaskQueueCallback* callback;
};

class Queue;
class Port;

struct Item {
    Entry entry;
    std::shared_ptr<Queue> owner;
};

struct Attachment {
    const Queue* identity;
    std::weak_ptr<Queue> queue;
    XTaskQueuePort role;
};

struct Monitor {
    std::uint64_t token = 0;
    void* context = nullptr;
    XTaskQueueMonitorCallback* callback = nullptr;
    std::size_t active = 0;
    bool removed = false;
};

struct Waiter : std::enable_shared_from_this<Waiter> {
    std::weak_ptr<Queue> queue;
    XTaskQueuePort port = XTaskQueuePort::Work;
    HANDLE handle = nullptr;
    Entry entry{};
    PTP_WAIT wait = nullptr;
    std::mutex mutex;
    bool armed = false;
    bool owed = false;
};

std::vector<const Queue*>& RunningQueues() {
    thread_local std::vector<const Queue*>* const queues = new std::vector<const Queue*>();
    return *queues;
}

std::vector<const Monitor*>& ActiveMonitors() {
    thread_local std::vector<const Monitor*>* const monitors = new std::vector<const Monitor*>();
    return *monitors;
}

thread_local const Waiter* t_signaledWaiter = nullptr;

void CALLBACK WaitSignaled(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT result);

class Running {
public:
    explicit Running(const Queue* queue) { RunningQueues().push_back(queue); }
    ~Running() { RunningQueues().pop_back(); }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
};

}

struct XTaskQueueObject {
    std::uint32_t signature = HandleSignature;
    std::shared_ptr<Queue> queue;
};

struct XTaskQueuePortObject {
    Port* port;
};

namespace {

std::mutex g_portMutex;
std::set<const XTaskQueuePortObject*> g_ports;

class Port : public std::enable_shared_from_this<Port> {
public:
    explicit Port(XTaskQueueDispatchMode mode) : mode_(mode) {}

    ~Port() {
        std::lock_guard lock(g_portMutex);
        g_ports.erase(&handle_);
    }

    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    XTaskQueuePortHandle Handle() { return &handle_; }

    void Publish(const std::shared_ptr<Queue>& owner) {
        std::lock_guard lock(g_portMutex);
        owner_ = owner;
        g_ports.insert(&handle_);
    }

    std::shared_ptr<Queue> Owner() const { return owner_.lock(); }

    void Attach(const Queue* identity, std::weak_ptr<Queue> queue, XTaskQueuePort role) {
        std::lock_guard lock(mutex_);
        attached_.push_back({identity, std::move(queue), role});
    }

    void Detach(const Queue* queue) {
        std::lock_guard lock(mutex_);
        std::erase_if(attached_, [queue](const Attachment& attachment) { return attachment.identity == queue; });
    }

    HRESULT Submit(Item item);
    bool Dispatch(std::uint32_t timeoutMs);

    bool IsEmpty() {
        std::lock_guard lock(mutex_);
        return pending_.empty() && inFlight_ == 0 && !draining_;
    }

    std::vector<Item> Extract(const Queue* owner) {
        std::lock_guard lock(mutex_);
        std::vector<Item> extracted;
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->owner.get() == owner) {
                extracted.push_back(std::move(*it));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        return extracted;
    }

private:
    template <typename Work>
    static HRESULT Post(Work work) {
        auto* heap = new Work(std::move(work));
        const auto trampoline = [](PTP_CALLBACK_INSTANCE, void* data) {
            std::unique_ptr<Work> owned(static_cast<Work*>(data));
            (*owned)();
        };
        if (!TrySubmitThreadpoolCallback(trampoline, heap, nullptr)) {
            delete heap;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return S_OK;
    }

    void RunPooled(Item item);
    void Drain();

    std::weak_ptr<Queue> owner_;
    XTaskQueueDispatchMode mode_;
    XTaskQueuePortObject handle_{this};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Item> pending_;
    std::size_t inFlight_ = 0;
    bool draining_ = false;
    std::vector<Attachment> attached_;
};

class Queue : public std::enable_shared_from_this<Queue> {
public:
    Queue(std::shared_ptr<Port> work, std::shared_ptr<Port> completion, std::shared_ptr<Queue> workSource, std::shared_ptr<Queue> completionSource)
        : ports_{std::move(work), std::move(completion)}, sources_{std::move(workSource), std::move(completionSource)} {}

    static std::shared_ptr<Queue> Create(XTaskQueueDispatchMode work, XTaskQueueDispatchMode completion) {
        auto queue = std::make_shared<Queue>(std::make_shared<Port>(work), std::make_shared<Port>(completion), nullptr, nullptr);
        queue->ports_[0]->Publish(queue);
        queue->ports_[1]->Publish(queue);
        queue->AttachPorts();
        return queue;
    }

    static std::shared_ptr<Queue> CreateComposite(std::shared_ptr<Port> work, std::shared_ptr<Port> completion, std::shared_ptr<Queue> workSource, std::shared_ptr<Queue> completionSource) {
        auto queue = std::make_shared<Queue>(std::move(work), std::move(completion), std::move(workSource), std::move(completionSource));
        queue->AttachPorts();
        return queue;
    }

    ~Queue() {
        for (const std::shared_ptr<Port>& port : ports_) {
            port->Detach(this);
        }
        for (auto& [token, waiter] : waiters_) {
            Disarm(*waiter);
        }
    }

    XTaskQueueHandle SelfHandle() { return &self_; }

    const std::shared_ptr<Port>& PortOf(XTaskQueuePort port) const { return ports_[Index(port)]; }

    HRESULT Submit(XTaskQueuePort port, Entry entry) {
        return PortOf(port)->Submit({entry, shared_from_this()});
    }

    bool Admit() {
        std::lock_guard lock(mutex_);
        if (terminated_) {
            return false;
        }
        ++outstanding_;
        return true;
    }

    void Retire() {
        std::lock_guard lock(mutex_);
        --outstanding_;
        changed_.notify_all();
    }

    std::vector<Item> BeginTerminate() {
        {
            std::lock_guard lock(mutex_);
            terminated_ = true;
        }
        std::vector<Item> canceled = ports_[0]->Extract(this);
        if (ports_[1] != ports_[0]) {
            std::vector<Item> completion = ports_[1]->Extract(this);
            std::move(completion.begin(), completion.end(), std::back_inserter(canceled));
        }
        return canceled;
    }

    void WaitIdle() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return outstanding_ == 0; });
    }

    bool Terminated() {
        std::lock_guard lock(mutex_);
        return terminated_;
    }

    void RegisterMonitor(void* context, XTaskQueueMonitorCallback* callback, XTaskQueueRegistrationToken* token) {
        auto monitor = std::make_shared<Monitor>();
        monitor->token = ++nextToken_;
        monitor->context = context;
        monitor->callback = callback;
        std::lock_guard lock(monitorMutex_);
        monitors_.push_back(monitor);
        token->token = monitor->token;
    }

    void UnregisterMonitor(std::uint64_t token, const char* function) {
        std::unique_lock lock(monitorMutex_);
        const auto it = std::find_if(monitors_.begin(), monitors_.end(), [token](const auto& monitor) { return monitor->token == token; });
        if (it == monitors_.end()) {
            return;
        }
        const std::shared_ptr<Monitor> monitor = *it;
        monitors_.erase(it);
        monitor->removed = true;
        if (std::find(ActiveMonitors().begin(), ActiveMonitors().end(), monitor.get()) != ActiveMonitors().end()) {
            lock.unlock();
            AbxUnsupported(function, "unregistering a monitor from inside its own callback");
        }
        monitorIdle_.wait(lock, [&] { return monitor->active == 0; });
    }

    void InvokeMonitors(XTaskQueuePort role) {
        std::vector<std::shared_ptr<Monitor>> snapshot;
        {
            std::lock_guard lock(monitorMutex_);
            snapshot = monitors_;
        }
        for (const std::shared_ptr<Monitor>& monitor : snapshot) {
            {
                std::lock_guard lock(monitorMutex_);
                if (monitor->removed) {
                    continue;
                }
                ++monitor->active;
            }
            ActiveMonitors().push_back(monitor.get());
            monitor->callback(monitor->context, &self_, role);
            ActiveMonitors().pop_back();
            {
                std::lock_guard lock(monitorMutex_);
                --monitor->active;
            }
            monitorIdle_.notify_all();
        }
    }

    HRESULT RegisterWaiter(XTaskQueuePort port, HANDLE handle, Entry entry, XTaskQueueRegistrationToken* token) {
        auto waiter = std::make_shared<Waiter>();
        waiter->queue = weak_from_this();
        waiter->port = port;
        waiter->handle = handle;
        waiter->entry = entry;
        waiter->wait = CreateThreadpoolWait(WaitSignaled, waiter.get(), nullptr);
        if (waiter->wait == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        std::lock_guard lock(mutex_);
        if (terminated_) {
            CloseThreadpoolWait(waiter->wait);
            return E_ABORT;
        }
        const std::uint64_t value = ++nextToken_;
        waiters_.emplace(value, waiter);
        {
            std::lock_guard armLock(waiter->mutex);
            waiter->armed = true;
            SetThreadpoolWait(waiter->wait, handle, nullptr);
        }
        token->token = value;
        return S_OK;
    }

    void UnregisterWaiter(std::uint64_t token) {
        std::shared_ptr<Waiter> waiter;
        {
            std::lock_guard lock(mutex_);
            const auto it = waiters_.find(token);
            if (it == waiters_.end()) {
                return;
            }
            waiter = std::move(it->second);
            waiters_.erase(it);
        }
        Disarm(*waiter);
    }

    std::vector<Entry> CancelWaiters() {
        std::map<std::uint64_t, std::shared_ptr<Waiter>> waiters;
        {
            std::lock_guard lock(mutex_);
            waiters.swap(waiters_);
        }
        std::vector<Entry> canceled;
        for (auto& [token, waiter] : waiters) {
            if (!Disarm(*waiter)) {
                canceled.push_back(waiter->entry);
            }
        }
        return canceled;
    }

    static void Run(const Entry& entry, bool canceled) {
        entry.callback(entry.context, canceled);
    }

    static void Finish(Item& item, bool canceled) {
        {
            Running running(item.owner.get());
            Run(item.entry, canceled);
        }
        item.owner->Retire();
    }

private:
    static std::size_t Index(XTaskQueuePort port) { return port == XTaskQueuePort::Work ? 0 : 1; }

    void AttachPorts() {
        ports_[0]->Attach(this, weak_from_this(), XTaskQueuePort::Work);
        ports_[1]->Attach(this, weak_from_this(), XTaskQueuePort::Completion);
    }

    static bool Disarm(Waiter& waiter) {
        bool owed = false;
        {
            std::lock_guard lock(waiter.mutex);
            waiter.armed = false;
            SetThreadpoolWait(waiter.wait, nullptr, nullptr);
            owed = waiter.owed;
        }
        if (t_signaledWaiter != &waiter) {
            WaitForThreadpoolWaitCallbacks(waiter.wait, TRUE);
        }
        CloseThreadpoolWait(waiter.wait);
        return owed;
    }

    std::shared_ptr<Port> ports_[2];
    std::shared_ptr<Queue> sources_[2];
    XTaskQueueObject self_{HandleSignature, std::shared_ptr<Queue>(std::shared_ptr<Queue>(), this)};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t outstanding_ = 0;
    bool terminated_ = false;
    std::map<std::uint64_t, std::shared_ptr<Waiter>> waiters_;
    std::mutex monitorMutex_;
    std::condition_variable monitorIdle_;
    std::vector<std::shared_ptr<Monitor>> monitors_;
    std::atomic<std::uint64_t> nextToken_{0};
};

HRESULT Port::Submit(Item item) {
    std::unique_lock lock(mutex_);
    if (!item.owner->Admit()) {
        return E_ABORT;
    }
    HRESULT result = S_OK;
    switch (mode_) {
    case XTaskQueueDispatchMode::Immediate:
        ++inFlight_;
        break;
    case XTaskQueueDispatchMode::Manual:
        pending_.push_back(item);
        changed_.notify_all();
        break;
    case XTaskQueueDispatchMode::ThreadPool:
        ++inFlight_;
        result = Post([self = shared_from_this(), item] { self->RunPooled(item); });
        if (FAILED(result)) {
            --inFlight_;
        }
        break;
    case XTaskQueueDispatchMode::SerializedThreadPool:
        pending_.push_back(item);
        if (!draining_) {
            draining_ = true;
            result = Post([self = shared_from_this()] { self->Drain(); });
            if (FAILED(result)) {
                pending_.pop_back();
                draining_ = false;
            }
        }
        break;
    }
    if (FAILED(result)) {
        item.owner->Retire();
        return result;
    }
    std::vector<std::pair<std::shared_ptr<Queue>, XTaskQueuePort>> listeners;
    for (const Attachment& attachment : attached_) {
        if (std::shared_ptr<Queue> queue = attachment.queue.lock()) {
            listeners.emplace_back(std::move(queue), attachment.role);
        }
    }
    lock.unlock();
    for (const auto& [queue, role] : listeners) {
        queue->InvokeMonitors(role);
    }
    listeners.clear();
    if (mode_ == XTaskQueueDispatchMode::Immediate) {
        Queue::Finish(item, false);
        std::lock_guard relock(mutex_);
        --inFlight_;
        changed_.notify_all();
    }
    return S_OK;
}

bool Port::Dispatch(std::uint32_t timeoutMs) {
    std::unique_lock lock(mutex_);
    if (mode_ != XTaskQueueDispatchMode::Manual) {
        return false;
    }
    const auto ready = [&] { return !pending_.empty(); };
    if (timeoutMs == XTaskQueueInfinite) {
        changed_.wait(lock, ready);
    } else if (!changed_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) {
        return false;
    }
    Item item = std::move(pending_.front());
    pending_.pop_front();
    ++inFlight_;
    lock.unlock();
    Queue::Finish(item, false);
    lock.lock();
    --inFlight_;
    changed_.notify_all();
    lock.unlock();
    return true;
}

void Port::RunPooled(Item item) {
    {
        Running running(item.owner.get());
        Queue::Run(item.entry, false);
    }
    {
        std::lock_guard lock(mutex_);
        --inFlight_;
        changed_.notify_all();
    }
    item.owner->Retire();
}

void Port::Drain() {
    std::unique_lock lock(mutex_);
    while (!pending_.empty()) {
        {
            Item item = std::move(pending_.front());
            pending_.pop_front();
            lock.unlock();
            Queue::Finish(item, false);
        }
        lock.lock();
    }
    draining_ = false;
    changed_.notify_all();
}

void RunWaiter(void* context, bool canceled) {
    std::unique_ptr<std::shared_ptr<Waiter>> holder(static_cast<std::shared_ptr<Waiter>*>(context));
    Waiter& waiter = **holder;
    Queue::Run(waiter.entry, canceled);
    if (canceled) {
        return;
    }
    std::lock_guard lock(waiter.mutex);
    waiter.owed = false;
    if (waiter.armed) {
        SetThreadpoolWait(waiter.wait, waiter.handle, nullptr);
    }
}

void CALLBACK WaitSignaled(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT result) {
    Waiter* waiter = static_cast<Waiter*>(context);
    if (result != WAIT_OBJECT_0) {
        AbxUnsupported("XTaskQueueRegisterWaiter", "wait handle signaled as an abandoned mutex");
    }
    const std::shared_ptr<Waiter> self = waiter->shared_from_this();
    {
        std::lock_guard lock(waiter->mutex);
        if (!waiter->armed) {
            return;
        }
        waiter->owed = true;
    }
    t_signaledWaiter = waiter;
    if (std::shared_ptr<Queue> queue = waiter->queue.lock()) {
        auto* holder = new std::shared_ptr<Waiter>(self);
        const HRESULT queued = queue->Submit(waiter->port, {holder, RunWaiter});
        if (queued == E_ABORT) {
            RunWaiter(holder, true);
        } else if (FAILED(queued)) {
            delete holder;
            std::lock_guard lock(waiter->mutex);
            waiter->owed = false;
            if (waiter->armed) {
                SetThreadpoolWait(waiter->wait, waiter->handle, nullptr);
            }
        }
    } else {
        std::lock_guard lock(waiter->mutex);
        waiter->owed = false;
    }
    t_signaledWaiter = nullptr;
}

struct Delayed {
    std::shared_ptr<Queue> queue;
    XTaskQueuePort port;
    Entry entry;
};

class Scheduler {
public:
    void Add(Clock::time_point due, Delayed item) {
        std::lock_guard lock(mutex_);
        items_.emplace(due, std::move(item));
        if (!started_) {
            started_ = true;
            std::thread([this] { Loop(); }).detach();
        }
        changed_.notify_all();
    }

    std::vector<Entry> Cancel(const Queue* queue) {
        std::lock_guard lock(mutex_);
        std::vector<Entry> canceled;
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->second.queue.get() == queue) {
                canceled.push_back(it->second.entry);
                it = items_.erase(it);
            } else {
                ++it;
            }
        }
        return canceled;
    }

private:
    void Loop() {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (items_.empty()) {
                changed_.wait(lock);
                continue;
            }
            const auto due = items_.begin()->first;
            if (Clock::now() < due) {
                changed_.wait_until(lock, due);
                continue;
            }
            Delayed item = std::move(items_.begin()->second);
            items_.erase(items_.begin());
            lock.unlock();
            if (FAILED(item.queue->Submit(item.port, item.entry))) {
                Queue::Run(item.entry, true);
            }
            item.queue.reset();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::multimap<Clock::time_point, Delayed> items_;
    bool started_ = false;
};

Scheduler& DelayScheduler() {
    static Scheduler* scheduler = new Scheduler();
    return *scheduler;
}

std::mutex g_processMutex;
XTaskQueueHandle g_processQueue = nullptr;
bool g_processQueueSet = false;

XTaskQueueHandle NewHandle(std::shared_ptr<Queue> queue) {
    return new XTaskQueueObject{HandleSignature, std::move(queue)};
}

std::shared_ptr<Queue> Resolve(XTaskQueueHandle handle, const char* function) {
    if (handle == nullptr) {
        XTaskQueueHandle process = nullptr;
        if (!XTaskQueueGetCurrentProcessTaskQueue(&process)) {
            AbxUnsupported(function, "null queue with no process task queue");
        }
        std::shared_ptr<Queue> queue = process->queue;
        XTaskQueueCloseHandle(process);
        return queue;
    }
    if (handle->signature != HandleSignature) {
        AbxUnsupported(function, "handle is not a task queue created by AnyBox");
    }
    return handle->queue->shared_from_this();
}

struct ResolvedPort {
    std::shared_ptr<Port> port;
    std::shared_ptr<Queue> owner;
};

ResolvedPort ResolvePort(XTaskQueuePortHandle handle) {
    std::lock_guard lock(g_portMutex);
    if (!g_ports.contains(handle)) {
        return {};
    }
    ResolvedPort resolved{handle->port->weak_from_this().lock(), handle->port->Owner()};
    if (resolved.port == nullptr || resolved.owner == nullptr) {
        return {};
    }
    return resolved;
}

bool ValidMode(XTaskQueueDispatchMode mode) {
    return static_cast<std::uint32_t>(mode) <= static_cast<std::uint32_t>(XTaskQueueDispatchMode::Immediate);
}

bool ValidPort(XTaskQueuePort port) {
    return port == XTaskQueuePort::Work || port == XTaskQueuePort::Completion;
}

}

ABX_API HRESULT XTaskQueueCreate(XTaskQueueDispatchMode workDispatchMode, XTaskQueueDispatchMode completionDispatchMode, XTaskQueueHandle* queue) {
    if (queue == nullptr || !ValidMode(workDispatchMode) || !ValidMode(completionDispatchMode)) {
        return E_INVALIDARG;
    }
    *queue = NewHandle(Queue::Create(workDispatchMode, completionDispatchMode));
    return S_OK;
}

ABX_API HRESULT XTaskQueueCreateComposite(XTaskQueuePortHandle workPort, XTaskQueuePortHandle completionPort, XTaskQueueHandle* queue) {
    if (queue == nullptr) {
        return E_POINTER;
    }
    *queue = nullptr;
    ResolvedPort work = ResolvePort(workPort);
    ResolvedPort completion = ResolvePort(completionPort);
    if (work.port == nullptr || completion.port == nullptr) {
        return InvalidHandle;
    }
    *queue = NewHandle(Queue::CreateComposite(std::move(work.port), std::move(completion.port), std::move(work.owner), std::move(completion.owner)));
    return S_OK;
}

ABX_API HRESULT XTaskQueueGetPort(XTaskQueueHandle queue, XTaskQueuePort port, XTaskQueuePortHandle* portHandle) {
    if (portHandle == nullptr) {
        return E_POINTER;
    }
    if (!ValidPort(port)) {
        return E_INVALIDARG;
    }
    *portHandle = Resolve(queue, __func__)->PortOf(port)->Handle();
    return S_OK;
}

ABX_API HRESULT XTaskQueueDuplicateHandle(XTaskQueueHandle queue, XTaskQueueHandle* duplicated) {
    if (queue == nullptr || duplicated == nullptr) {
        return E_INVALIDARG;
    }
    *duplicated = NewHandle(Resolve(queue, __func__));
    return S_OK;
}

ABX_API void XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    if (queue == nullptr) {
        return;
    }
    if (queue->signature != HandleSignature) {
        AbxUnsupported(__func__, "handle is not a task queue created by AnyBox");
    }
    if (queue == queue->queue->SelfHandle()) {
        AbxUnsupported(__func__, "closing the queue handle passed to a monitor callback");
    }
    queue->signature = 0;
    delete queue;
}

ABX_API HRESULT XTaskQueueSubmitCallback(XTaskQueueHandle queue, XTaskQueuePort port, void* context, XTaskQueueCallback* callback) {
    if (callback == nullptr || !ValidPort(port)) {
        return E_INVALIDARG;
    }
    return Resolve(queue, __func__)->Submit(port, {context, callback});
}

ABX_API HRESULT XTaskQueueSubmitDelayedCallback(XTaskQueueHandle queue, XTaskQueuePort port, std::uint32_t delayMs, void* context, XTaskQueueCallback* callback) {
    if (callback == nullptr || !ValidPort(port)) {
        return E_INVALIDARG;
    }
    std::shared_ptr<Queue> target = Resolve(queue, __func__);
    if (target->Terminated()) {
        return E_ABORT;
    }
    if (delayMs == 0) {
        return target->Submit(port, {context, callback});
    }
    DelayScheduler().Add(Clock::now() + std::chrono::milliseconds(delayMs), {std::move(target), port, {context, callback}});
    return S_OK;
}

ABX_API bool XTaskQueueDispatch(XTaskQueueHandle queue, XTaskQueuePort port, std::uint32_t timeoutMs) {
    if (!ValidPort(port)) {
        return false;
    }
    return Resolve(queue, __func__)->PortOf(port)->Dispatch(timeoutMs);
}

ABX_API bool XTaskQueueIsEmpty(XTaskQueueHandle queue, XTaskQueuePort port) {
    if (!ValidPort(port)) {
        return true;
    }
    return Resolve(queue, __func__)->PortOf(port)->IsEmpty();
}

ABX_API HRESULT XTaskQueueTerminate(XTaskQueueHandle queue, bool wait, void* context, XTaskQueueTerminatedCallback* callback) {
    std::shared_ptr<Queue> target = Resolve(queue, __func__);
    if (wait && std::find(RunningQueues().begin(), RunningQueues().end(), target.get()) != RunningQueues().end()) {
        AbxUnsupported(__func__, "waiting termination from one of the queue's own callbacks");
    }
    std::vector<Item> canceled = target->BeginTerminate();
    const std::vector<Entry> delayed = DelayScheduler().Cancel(target.get());
    const std::vector<Entry> waiters = target->CancelWaiters();
    for (Item& item : canceled) {
        Queue::Finish(item, true);
    }
    for (const Entry& entry : delayed) {
        Queue::Run(entry, true);
    }
    for (const Entry& entry : waiters) {
        Queue::Run(entry, true);
    }
    if (wait) {
        target->WaitIdle();
        if (callback != nullptr) {
            callback(context);
        }
        return S_OK;
    }
    std::thread([target, context, callback] {
        target->WaitIdle();
        if (callback != nullptr) {
            callback(context);
        }
    }).detach();
    return S_OK;
}

ABX_API HRESULT XTaskQueueRegisterMonitor(XTaskQueueHandle queue, void* context, XTaskQueueMonitorCallback* callback, XTaskQueueRegistrationToken* token) {
    if (callback == nullptr || token == nullptr) {
        return E_POINTER;
    }
    Resolve(queue, __func__)->RegisterMonitor(context, callback, token);
    return S_OK;
}

ABX_API void XTaskQueueUnregisterMonitor(XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
    Resolve(queue, __func__)->UnregisterMonitor(token.token, __func__);
}

ABX_API HRESULT XTaskQueueRegisterWaiter(XTaskQueueHandle queue, XTaskQueuePort port, HANDLE waitHandle, void* context, XTaskQueueCallback* callback, XTaskQueueRegistrationToken* token) {
    if (callback == nullptr || token == nullptr) {
        return E_POINTER;
    }
    if (!ValidPort(port) || waitHandle == nullptr) {
        return E_INVALIDARG;
    }
    return Resolve(queue, __func__)->RegisterWaiter(port, waitHandle, {context, callback}, token);
}

ABX_API void XTaskQueueUnregisterWaiter(XTaskQueueHandle queue, XTaskQueueRegistrationToken token) {
    Resolve(queue, __func__)->UnregisterWaiter(token.token);
}

ABX_API bool XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (queue == nullptr) {
        return false;
    }
    std::lock_guard lock(g_processMutex);
    if (g_processQueue == nullptr && !g_processQueueSet) {
        g_processQueue = NewHandle(Queue::Create(XTaskQueueDispatchMode::ThreadPool, XTaskQueueDispatchMode::ThreadPool));
    }
    if (g_processQueue == nullptr) {
        *queue = nullptr;
        return false;
    }
    *queue = NewHandle(g_processQueue->queue);
    return true;
}

ABX_API void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    XTaskQueueHandle replacement = queue != nullptr ? NewHandle(Resolve(queue, __func__)) : nullptr;
    std::lock_guard lock(g_processMutex);
    delete g_processQueue;
    g_processQueue = replacement;
    g_processQueueSet = true;
}
