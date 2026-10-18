// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "video_core/renderer_d3d12/d3d12_compile_policy.h"

namespace D3D12 {
// Small draws can build persistent textures only once, so they wait for their PSO. Stream
// output also has side effects that cannot be dropped. Only the count of the draw being made
// decides: the other one is left over from an earlier draw (a 3-vertex fullscreen pass made every
// later indexed mesh look small, so Wonder waited on them for 69 s of a 380 s run).
constexpr bool CanSkipPendingGraphicsPso(bool enabled, std::uint32_t draw_count,
                                         bool transform_feedback) {
    return enabled && draw_count > 6 && !transform_feedback;
}

// Jobs own immutable inputs. Only this map is shared with the requesting thread;
// expensive compilation never runs with its mutex held. One pool serves both stages.
//
// A driver compile can block forever (BotW has compute shaders the Xbox driver never finishes),
// and a thread cannot be cancelled. A job running longer than `stall_limit` stops counting
// against the queue capacity and a replacement worker takes over, up to `max_threads`. Shutdown
// detaches every busy worker (any of them may never return); each keeps the shared state alive,
// and Stop() reports how many so the owner can keep what the jobs still reference.
template <typename Key, typename Result, typename Hash = std::hash<Key>>
class AsyncPsoWorkerPool {
public:
    enum class Status { Missing, Pending, Ready, Failed };
    struct Entry {
        Status status{Status::Pending};
        std::shared_ptr<Result> result;
        std::string error;
        bool prewarmed{};
    };
    using Job = std::function<std::shared_ptr<Result>()>;
    using Clock = std::chrono::steady_clock;
    using NativeHandle = std::thread::native_handle_type;
    using PriorityHook = std::function<void(NativeHandle, int)>;
    static void SetPriority(NativeHandle handle, int priority) {
#ifdef _WIN32
        SetThreadPriority(handle, priority);
#else
        (void)handle;
        (void)priority;
#endif
    }
    explicit AsyncPsoWorkerPool(size_t thread_count = 2, size_t capacity_ = 4,
                                Clock::duration stall_limit = std::chrono::seconds{5},
                                size_t max_threads = 4, PriorityHook priority_hook = SetPriority)
        : state{std::make_shared<State>()} {
        state->priority_hook = std::move(priority_hook);
        state->base_priority = PsoWorkerPriority();
        state->capacity = base_capacity = capacity_;
        state->stall_limit = stall_limit;
        base_threads = std::clamp(thread_count, size_t{1}, size_t{3});
        thread_limit = std::max(max_threads, base_threads);
        state->target = base_threads;
        try {
            std::scoped_lock lock{state->mutex};
            for (size_t i = 0; i < base_threads; ++i)
                Spawn();
        } catch (...) {
            Stop();
            throw;
        }
    }
    ~AsyncPsoWorkerPool() {
        Stop();
    }
    /// Resize the non-stalled pool; busy surplus workers retire before taking another job.
    void SetWorkerTarget(size_t threads) {
        std::scoped_lock lock{state->mutex};
        if (state->stopping)
            return;
        const size_t target = std::clamp(threads, size_t{1}, size_t{6});
        state->resized |= target != state->target;
        state->target = target;
        state->capacity = state->target > base_threads ? std::max(base_capacity, 2 * state->target)
                                                       : base_capacity;
        while (state->active - CountStalled() < state->target && state->active < WorkerLimit())
            Spawn();
        state->condition.notify_all();
    }
    /// Live workers, including those occupied by stalled jobs.
    size_t WorkerCount() {
        std::scoped_lock lock{state->mutex};
        return state->active;
    }
    Entry Lookup(const Key& key) {
        std::scoped_lock lock{state->mutex};
        const auto it = state->entries.find(key);
        return it == state->entries.end() ? Entry{Status::Missing, {}, {}} : it->second;
    }
    bool CanSubmit() {
        std::scoped_lock lock{state->mutex};
        return !state->stopping && Admittable();
    }
    bool Submit(const Key& key, Job job, bool prewarmed = false) {
        std::scoped_lock lock{state->mutex};
        if (state->stopping || !Admittable() || state->entries.contains(key))
            return false;
        Entry entry;
        entry.prewarmed = prewarmed;
        state->entries.emplace(key, std::move(entry));
        ++state->pending;
        state->queue.emplace_back(key, std::move(job));
        state->condition.notify_one();
        return true;
    }
    /// Publish a policy rejection without occupying a worker or queue slot.
    bool Reject(const Key& key, std::string error) {
        std::scoped_lock lock{state->mutex};
        return state->entries.emplace(key, Entry{Status::Failed, {}, std::move(error)}).second;
    }
    /// Drops a prewarm that failed or was cancelled so the runtime can build the key itself.
    bool ForgetFailedPrewarm(const Key& key) {
        std::scoped_lock lock{state->mutex};
        const auto it = state->entries.find(key);
        if (it == state->entries.end() || !it->second.prewarmed ||
            it->second.status != Status::Failed)
            return false;
        state->entries.erase(it);
        return true;
    }
    /// Blocks until `key` leaves Pending or `timeout` passes, then returns its entry.
    Entry WaitFor(const Key& key, Clock::duration timeout) {
        std::unique_lock lock{state->mutex};
        const auto settled = [&] {
            const auto it = state->entries.find(key);
            return it == state->entries.end() || it->second.status != Status::Pending;
        };
        if (!settled()) {
            const auto queued = std::find_if(state->queue.begin(), state->queue.end(),
                                             [&](const auto& item) { return item.first == key; });
            if (queued != state->queue.end())
                std::rotate(state->queue.begin(), queued, std::next(queued));
            ++state->waiters[key];
            UpdatePriority(*state, key);
            state->finished.wait_for(lock, timeout, settled);
            if (--state->waiters.at(key) == 0) {
                state->waiters.erase(key);
                UpdatePriority(*state, key);
            }
        }
        const auto it = state->entries.find(key);
        return it == state->entries.end() ? Entry{Status::Missing, {}, {}} : it->second;
    }
    size_t WaiterCount(const Key& key) {
        std::scoped_lock lock{state->mutex};
        const auto it = state->waiters.find(key);
        return it == state->waiters.end() ? 0 : it->second;
    }
    /// Jobs that have run past the stall limit and still hold a worker.
    size_t Stalled() {
        std::scoped_lock lock{state->mutex};
        return CountStalled();
    }
    /// Whether no job is queued or running, not counting stalled ones.
    bool Idle() {
        std::scoped_lock lock{state->mutex};
        return state->pending == CountStalled();
    }
    /// Whether `key` has been running for at least `limit`.
    bool IsStalled(const Key& key, Clock::duration limit) {
        std::scoped_lock lock{state->mutex};
        const auto now = Clock::now();
        for (size_t i = 0; i < state->busy_since.size(); ++i)
            if (state->busy_since[i] && now - *state->busy_since[i] >= limit &&
                state->running[i] && *state->running[i] == key)
                return true;
        return false;
    }
    /// Returns how many busy workers were detached instead of joined.
    size_t Stop() {
        std::vector<bool> busy;
        size_t detached = 0;
        {
            std::scoped_lock lock{state->mutex};
            if (state->stopping)
                return 0;
            state->stopping = true;
            for (const auto& item : state->queue) {
                state->entries.at(item.first) = {Status::Failed, {}, "Compilation cancelled"};
                --state->pending;
            }
            state->queue.clear();
            for (size_t i = 0; i < state->busy_since.size(); ++i) {
                busy.push_back(state->busy_since[i].has_value());
                // detach closes the native Windows handle; never use it after Stop.
                if (busy.back())
                    state->priority_hook(state->handles[i], state->base_priority);
            }
        }
        state->condition.notify_all();
        state->finished.notify_all();
        for (size_t i = 0; i < workers.size(); ++i) {
            if (!workers[i].joinable())
                continue;
            if (busy[i]) {
                workers[i].detach();
                ++detached;
            } else {
                workers[i].join();
            }
        }
        return detached;
    }

private:
    struct State {
        PriorityHook priority_hook;
        int base_priority{};
        std::vector<NativeHandle> handles;
        std::unordered_map<Key, size_t, Hash> waiters;
        size_t capacity{}, pending{}, target{}, active{};
        std::vector<bool> retired;
        Clock::duration stall_limit{};
        bool resized{}, stopping{};
        std::mutex mutex;
        std::condition_variable condition;
        std::condition_variable finished;
        std::deque<std::pair<Key, Job>> queue;
        std::unordered_map<Key, Entry, Hash> entries;
        std::vector<std::optional<Clock::time_point>> busy_since; // Indexed by worker.
        std::vector<std::optional<Key>> running;
    };

    // Caller holds the mutex, including during native priority changes.
    static void UpdatePriority(State& state, const Key& key) {
        if (state.stopping)
            return;
        for (size_t i = 0; i < state.running.size(); ++i)
            if (state.running[i] && *state.running[i] == key)
                state.priority_hook(state.handles[i],
                                    state.waiters.contains(key) ? 1 : state.base_priority);
    }
    // Caller holds the mutex.
    size_t CountStalled() const {
        const auto now = Clock::now();
        return static_cast<size_t>(std::count_if(
            state->busy_since.begin(), state->busy_since.end(),
            [&](const auto& since) { return since && now - *since >= state->stall_limit; }));
    }
    // Preserve the constructor's replacement allowance while the target is raised.
    size_t WorkerLimit() const {
        return std::max(thread_limit, state->target + thread_limit - base_threads);
    }
    // Caller holds the mutex. Replaces stalled workers before deciding on admission.
    bool Admittable() {
        const size_t stalled = CountStalled();
        while (state->active - stalled < state->target && state->active < WorkerLimit())
            Spawn();
        // With every worker stuck, an admitted job would stay Pending forever.
        return state->active > stalled && state->pending - stalled < state->capacity;
    }
    // Caller holds the mutex.
    void Spawn() {
        // Retired slots stay aligned with handles/busy_since/running. Reuse only after
        // joining the exited thread, so index-based priority and Stop code remain valid.
        const auto retired = std::find(state->retired.begin(), state->retired.end(), true);
        const size_t index = std::distance(state->retired.begin(), retired);
        if (index == workers.size()) {
            // Allocate before changing any sizes so allocation failure cannot misalign slots.
            workers.reserve(index + 1);
            state->handles.reserve(index + 1);
            state->busy_since.reserve(index + 1);
            state->running.reserve(index + 1);
            state->retired.reserve(index + 1);
            workers.emplace_back();
            state->handles.emplace_back();
            state->busy_since.emplace_back();
            state->running.emplace_back();
            state->retired.push_back(true);
        } else if (workers[index].joinable()) {
            workers[index].join();
        }
        // The new thread blocks on the caller's mutex until its slot is initialized.
        workers[index] = std::thread([shared = state, index] { Run(*shared, index); });
        state->handles[index] = workers[index].native_handle();
        state->retired[index] = false;
        ++state->active;
    }
    static void Run(State& state, size_t index) {
        for (;;) {
            std::pair<Key, Job> item;
            {
                std::unique_lock lock{state.mutex};
                const auto surplus = [&] {
                    // An untouched pool keeps its original replacement-worker behavior.
                    if (!state.resized)
                        return false;
                    const auto now = Clock::now();
                    const size_t stalled = static_cast<size_t>(std::count_if(
                        state.busy_since.begin(), state.busy_since.end(), [&](const auto& since) {
                            return since && now - *since >= state.stall_limit;
                        }));
                    return state.active - stalled > state.target;
                };
                state.condition.wait(
                    lock, [&] { return state.stopping || surplus() || !state.queue.empty(); });
                if (state.stopping || surplus()) {
                    state.retired[index] = true;
                    state.handles[index] = {};
                    --state.active;
                    return;
                }
                item = std::move(state.queue.front());
                state.queue.pop_front();
                state.busy_since[index] = Clock::now();
                state.running[index] = item.first;
                UpdatePriority(state, item.first);
            }
            Entry result;
            try {
                result.result = item.second();
                result.status = result.result ? Status::Ready : Status::Failed;
                if (!result.result)
                    result.error = "Compilation returned no pipeline";
            } catch (const std::exception& error) {
                result.status = Status::Failed;
                result.error = error.what();
            } catch (...) {
                result.status = Status::Failed;
                result.error = "Unknown pipeline compilation exception";
            }
            // Release captured snapshots before admitting more work.
            item.second = {};
            {
                std::scoped_lock lock{state.mutex};
                result.prewarmed = state.entries.at(item.first).prewarmed;
                state.entries.at(item.first) = std::move(result);
                --state.pending;
                if (!state.stopping)
                    state.priority_hook(state.handles[index], state.base_priority);
                state.busy_since[index].reset();
                state.running[index].reset();
            }
            state.finished.notify_all();
        }
    }
    std::shared_ptr<State> state;
    size_t base_threads{}, thread_limit{}, base_capacity{};
    std::vector<std::thread> workers;
};
} // namespace D3D12
