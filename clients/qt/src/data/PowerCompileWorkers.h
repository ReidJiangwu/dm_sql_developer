#pragma once
#include "../core/ParallelExecutionSettings.h"

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

// A session is constructed, used and destroyed on one persistent worker thread.
// Construction waits for every login; a partial startup never accepts jobs.
template<class Session>
class PowerCompileWorkers final {
public:
    using Factory = std::function<std::unique_ptr<Session>(int)>;
    PowerCompileWorkers(int count, Factory factory,
            int maximumCount = ParallelExecutionSettings::maximumWorkers) {
        if (maximumCount < 1 || maximumCount > ParallelExecutionSettings::maximumOrdinaryCompileWorkers
                || count < 1 || count > maximumCount)
            throw std::invalid_argument("Invalid compile worker count");
        std::vector<std::future<void>> started;
        try {
            for (int id = 1; id <= count; ++id) {
                auto ready = std::make_shared<std::promise<void>>();
                started.push_back(ready->get_future());
                threads_.emplace_back([this, factory, ready, id] {
                    std::unique_ptr<Session> session;
                    try { session = factory(id); }
                    catch (...) { ready->set_exception(std::current_exception()); return; }
                    if (!session) {
                        ready->set_exception(std::make_exception_ptr(
                            std::runtime_error("Compile session was not created")));
                        return;
                    }
                    ready->set_value();
                    for (;;) {
                        Job job;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                            if (jobs_.empty() && stopping_) return;
                            job = std::move(jobs_.front());
                            jobs_.pop();
                        }
                        job(*session, id);
                    }
                });
            }
            for (auto& ready : started) ready.get();
        } catch (...) { shutdown(); throw; }
    }
    ~PowerCompileWorkers() { shutdown(); }
    PowerCompileWorkers(const PowerCompileWorkers&) = delete;
    PowerCompileWorkers& operator=(const PowerCompileWorkers&) = delete;

    template<class Function>
    auto submit(Function function) -> std::future<std::invoke_result_t<Function, Session&, int>> {
        using Result = std::invoke_result_t<Function, Session&, int>;
        auto task = std::make_shared<std::packaged_task<Result(Session&, int)>>(std::move(function));
        auto result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("Compile workers are stopping");
            jobs_.push([task](Session& session, int id) { (*task)(session, id); });
        }
        wake_.notify_one();
        return result;
    }
private:
    using Job = std::function<void(Session&, int)>;
    void shutdown() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        wake_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::queue<Job> jobs_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};
