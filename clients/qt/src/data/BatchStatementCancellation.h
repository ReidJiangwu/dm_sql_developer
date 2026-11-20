#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <utility>

// Reuse DpiSession's synchronous cancellation strategy for isolated batch
// sessions. Each statement stays alive until its asynchronous cancel returns.
// Recovery statements do not register with the cancelled batch.
class BatchStatementCancellation final {
    struct Entry {
        std::mutex mutex;
        bool active = true;
        bool sent = false;
        std::function<void()> cancel;
        std::future<void> pending;
    };
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        bool stopping = false;
        std::vector<std::shared_ptr<Entry>> entries;
    };
public:
    class Guard final {
    public:
        Guard() = default;
        Guard(std::shared_ptr<State> state, std::shared_ptr<Entry> entry)
            : state_(std::move(state)), entry_(std::move(entry)) {}
        Guard(Guard&& other) noexcept = default;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        ~Guard() { release(); }
        void release() {
            if (!entry_) return;
            std::future<void> pending;
            {
                std::lock_guard<std::mutex> lock(entry_->mutex);
                entry_->active = false;
                pending = std::move(entry_->pending);
            }
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                auto& entries = state_->entries;
                entries.erase(std::remove(entries.begin(), entries.end(), entry_), entries.end());
            }
            // Never free/reuse a statement while dpi_cancel can still see it.
            if (pending.valid()) { try { pending.get(); } catch (...) {} }
            entry_.reset();
        }
    private:
        std::shared_ptr<State> state_;
        std::shared_ptr<Entry> entry_;
    };

    explicit BatchStatementCancellation(std::shared_ptr<std::atomic_bool> cancelled)
        : cancelled_(std::move(cancelled)), state_(std::make_shared<State>()),
          monitor_([state = state_, cancelled = cancelled_]() {
            std::unique_lock<std::mutex> lock(state->mutex);
            while (!state->stopping) {
                state->wake.wait_for(lock, std::chrono::milliseconds(25));
                if (state->stopping || !cancelled->load()) continue;
                const auto entries = state->entries;
                lock.unlock();
                for (const auto& entry : entries) {
                    std::lock_guard<std::mutex> entryLock(entry->mutex);
                    if (!entry->active || entry->sent) continue;
                    try {
                        // One slow cancellation must not block cancellation of
                        // the other independent worker sessions.
                        entry->pending = std::async(std::launch::async, entry->cancel);
                        entry->sent = true;
                    } catch (...) { /* Retry on the next tick; never detach. */ }
                }
                lock.lock();
            }
        }) {}
    ~BatchStatementCancellation() {
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->stopping = true; }
        state_->wake.notify_all();
        monitor_.join();
    }
    bool cancelled() const { return cancelled_->load(); }
    Guard watch(std::function<void()> cancel) {
        auto entry = std::make_shared<Entry>();
        entry->cancel = std::move(cancel);
        { std::lock_guard<std::mutex> lock(state_->mutex); state_->entries.push_back(entry); }
        state_->wake.notify_all();
        return Guard(state_, std::move(entry));
    }
private:
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::shared_ptr<State> state_;
    std::thread monitor_;
};
