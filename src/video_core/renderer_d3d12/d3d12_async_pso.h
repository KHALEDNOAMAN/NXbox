// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace D3D12 {
// Match the upstream asynchronous-shader heuristic: small draws can build persistent
// textures only once. Stream output also has side effects that cannot be dropped.
constexpr bool CanSkipPendingGraphicsPso(bool enabled, std::uint32_t index_count,
                                         std::uint32_t vertex_count, bool transform_feedback) {
    return enabled && index_count > 6 && vertex_count > 6 && !transform_feedback;
}

// Jobs own immutable inputs. Only this map is shared with the requesting thread;
// expensive compilation never runs with its mutex held. One pool serves both stages.
//
// A driver compile can block forever (BotW has compute shaders the Xbox driver never finishes),
// and a thread cannot be cancelled. A job running longer than `stall_limit` stops counting
// against the queue capacity and a replacement worker takes over, up to `max_threads` plus any
// temporary boost. Shutdown detaches every busy worker (any of them may never return); each keeps
// the shared state alive, and Stop() reports how many so the owner can keep what the jobs still
// reference.
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
    explicit AsyncPsoWorkerPool(size_t thread_count = 2, size_t capacity_ = 4,
                                Clock::duration stall_limit = std::chrono::seconds{5},
                                size_t max_threads = 4)
        : state{std::make_shared<State>()} {
        state->capacity = base_capacity = capacity_;
        state->stall_limit = stall_limit;
        base_threads = std::clamp(thread_count, size_t{1}, size_t{3});
        thread_limit = std::max(max_threads, base_threads);
        state->active_threads = base_threads;
        try {
            std::scoped_lock lock{state->mutex};
            for (size_t i = 0; i < base_threads; ++i)
                Spawn();
        } catch (...) {
            Stop();
            throw;
        }
    }
    // Keep the guard inside the loading scope; extra workers park when it is destroyed.
    class ScopedBoost {
    public:
        ScopedBoost(AsyncPsoWorkerPool& pool_, size_t thread_count) : pool{pool_} {
            try {
                pool.ConfigureWorkers(thread_count);
            } catch (...) {
                pool.ResetWorkers();
                throw;
            }
        }
        ~ScopedBoost() {
            pool.ResetWorkers();
        }
        ScopedBoost(const ScopedBoost&) = delete;
        ScopedBoost& operator=(const ScopedBoost&) = delete;

    private:
        AsyncPsoWorkerPool& pool;
    };

    ~AsyncPsoWorkerPool() {
        Stop();
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
        state->condition.notify_all();
        return true;
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
        state->finished.wait_for(lock, timeout, settled);
        const auto it = state->entries.find(key);
        return it == state->entries.end() ? Entry{Status::Missing, {}, {}} : it->second;
    }
    /// Jobs that have run past the stall limit and still hold a worker.
    size_t Stalled() {
        std::scoped_lock lock{state->mutex};
        return CountStalled();
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
            state->stopping = true;
            for (const auto& item : state->queue) {
                state->entries.at(item.first) = {Status::Failed, {}, "Compilation cancelled"};
                --state->pending;
            }
            state->queue.clear();
            for (const auto& since : state->busy_since)
                busy.push_back(since.has_value());
        }
        state->condition.notify_all();
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
        size_t capacity{}, pending{}, active_threads{};
        Clock::duration stall_limit{};
        bool stopping{};
        std::mutex mutex;
        std::condition_variable condition;
        std::condition_variable finished;
        std::deque<std::pair<Key, Job>> queue;
        std::unordered_map<Key, Entry, Hash> entries;
        std::vector<std::optional<Clock::time_point>> busy_since; // Indexed by worker.
        std::vector<std::optional<Key>> running;
    };

    void ConfigureWorkers(size_t thread_count) {
        std::scoped_lock lock{state->mutex};
        if (state->stopping)
            return;
        state->active_threads = std::max(thread_count, base_threads);
        state->capacity = std::max(base_capacity, state->active_threads + 2);
        Admittable();
        state->condition.notify_all();
    }
    void ResetWorkers() {
        std::scoped_lock lock{state->mutex};
        state->active_threads = base_threads;
        state->capacity = base_capacity;
        state->condition.notify_all();
    }
    // Caller holds the mutex. Idle workers above the healthy-worker target stay asleep.
    static bool CanTakeJob(const State& shared, size_t index) {
        const auto now = Clock::now();
        size_t preceding = 0;
        for (size_t i = 0; i < index; ++i)
            if (!shared.busy_since[i] || now - *shared.busy_since[i] < shared.stall_limit)
                ++preceding;
        return preceding < shared.active_threads;
    }
    // Caller holds the mutex.
    size_t CountStalled() const {
        const auto now = Clock::now();
        return static_cast<size_t>(std::count_if(
            state->busy_since.begin(), state->busy_since.end(),
            [&](const auto& since) { return since && now - *since >= state->stall_limit; }));
    }
    // Caller holds the mutex. Replaces stalled workers before deciding on admission.
    bool Admittable() {
        const size_t stalled = CountStalled();
        const size_t limit = thread_limit + state->active_threads - base_threads;
        while (workers.size() - stalled < state->active_threads && workers.size() < limit)
            Spawn();
        // Existing parked workers may now replace one that stalled.
        state->condition.notify_all();
        // With every worker stuck, an admitted job would stay Pending forever.
        return workers.size() > stalled && state->pending - stalled < state->capacity;
    }
    // Caller holds the mutex.
    void Spawn() {
        const size_t index = workers.size();
        // Allocate bookkeeping before starting a thread so allocation failure is safe.
        workers.reserve(index + 1);
        state->busy_since.reserve(index + 1);
        state->running.reserve(index + 1);
        // The new thread blocks on the mutex the caller holds until busy_since has its slot.
        workers.emplace_back([shared = state, index] { Run(*shared, index); });
        state->busy_since.emplace_back();
        state->running.emplace_back();
    }
    static void Run(State& state, size_t index) {
        for (;;) {
            std::pair<Key, Job> item;
            {
                std::unique_lock lock{state.mutex};
                state.condition.wait(lock, [&] {
                    return state.stopping || (!state.queue.empty() && CanTakeJob(state, index));
                });
                if (state.stopping)
                    return;
                item = std::move(state.queue.front());
                state.queue.pop_front();
                state.busy_since[index] = Clock::now();
                state.running[index] = item.first;
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
                state.busy_since[index].reset();
                state.running[index].reset();
            }
            state.finished.notify_all();
            state.condition.notify_all();
        }
    }
    std::shared_ptr<State> state;
    size_t base_threads{}, thread_limit{}, base_capacity{};
    std::vector<std::thread> workers;
};
} // namespace D3D12
