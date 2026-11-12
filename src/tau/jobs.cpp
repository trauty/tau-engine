#include "jobs.h"

#include "tau/defines.h"
#include "tau/log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tau::jobs
{
    constexpr u32_t MAX_JOBS = 4096;
    constexpr u32_t MASK = MAX_JOBS - 1;

    constexpr std::size_t CACHE_LINE = 64;
    constexpr u32_t SPIN_ROUNDS = 64;

    struct job_t
    {
        job_function<64> task;
        counter_t* counter = nullptr;
    };

    struct slot_t
    {
        std::atomic<u32_t> sequence{0};
        job_t job;
    };

    struct job_queue_t
    {
        std::array<slot_t, MAX_JOBS> buffer;
        alignas(CACHE_LINE) std::atomic<u32_t> head{0};
        alignas(CACHE_LINE) std::atomic<u32_t> tail{0};
        alignas(CACHE_LINE) std::atomic<u32_t> sleepers{0};

        std::mutex wake_mutex;
        std::condition_variable wake_cv;

        job_queue_t()
        {
            for (u32_t i = 0; i < MAX_JOBS; i++) { buffer[i].sequence.store(i, std::memory_order_relaxed); }
        }

        bool has_work() const { return head.load(std::memory_order_acquire) != tail.load(std::memory_order_acquire); }

        bool push(job_function<64> task, counter_t* counter)
        {
            u32_t pos = tail.load(std::memory_order_relaxed);
            while (true)
            {
                slot_t& slot = buffer[pos & MASK];
                const u32_t seq = slot.sequence.load(std::memory_order_acquire);
                const i32_t diff = static_cast<i32_t>(seq - pos);

                if (diff == 0)
                {
                    if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        slot.job.task = task;
                        slot.job.counter = counter;
                        slot.sequence.store(pos + 1, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0) { return false; } // full
                else
                {
                    pos = tail.load(std::memory_order_relaxed);
                }
            }
        }

        bool pop(job_t& out_job)
        {
            u32_t pos = head.load(std::memory_order_relaxed);
            while (true)
            {
                slot_t& slot = buffer[pos & MASK];
                const u32_t seq = slot.sequence.load(std::memory_order_acquire);
                const i32_t diff = static_cast<i32_t>(seq - (pos + 1));

                if (diff == 0)
                {
                    if (head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        out_job = slot.job;

                        slot.job.task = job_function<64>{};
                        slot.job.counter = nullptr;

                        slot.sequence.store(pos + MAX_JOBS, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0) { return false; } // empty
                else
                {
                    pos = head.load(std::memory_order_relaxed);
                }
            }
        }
    };

    namespace
    {
        job_queue_t high_priority_queue;
        job_queue_t low_priority_queue;

        std::vector<std::thread> high_priority_workers;
        std::thread low_priority_worker; // one for the assets and general io
        std::atomic<bool> is_running{false};
        // pushed and not yet finished, counted before the push so no gap shows idle
        std::atomic<u32_t> jobs_in_flight{0};

        void run_job(const job_t& job)
        {
            if (job.task) { job.task(); }
            if (job.counter) { job.counter->fetch_sub(1, std::memory_order_release); }
            jobs_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        }

        void park(job_queue_t& queue)
        {
            queue.sleepers.fetch_add(1, std::memory_order_seq_cst);

            {
                std::unique_lock lock(queue.wake_mutex);
                queue.wake_cv.wait(lock, [&queue]
                                   { return !is_running.load(std::memory_order_relaxed) || queue.has_work(); });
            }

            queue.sleepers.fetch_sub(1, std::memory_order_relaxed);
        }

        void worker_loop(job_queue_t& queue)
        {
            while (is_running.load(std::memory_order_relaxed))
            {
                job_t job;
                if (queue.pop(job))
                {
                    run_job(job);
                    continue;
                }

                bool found = false;
                for (u32_t spin = 0; spin < SPIN_ROUNDS; spin++)
                {
                    if (queue.pop(job))
                    {
                        found = true;
                        break;
                    }
                    std::this_thread::yield();
                }

                if (found) { run_job(job); }
                else if (is_running.load(std::memory_order_relaxed)) { park(queue); }
            }
        }
    } // namespace

    namespace detail
    {
        void push_job(job_function<64> task, counter_t* counter, priority_e prio)
        {
            job_queue_t& queue = (prio == priority_e::HIGH) ? high_priority_queue : low_priority_queue;

            jobs_in_flight.fetch_add(1, std::memory_order_acq_rel);
            if (queue.push(task, counter)) { return; }

            TAU_LOG_WARN("JOBS", "Job queue full ({} slots), running job on the submitting thread", MAX_JOBS);

            run_job({task, counter});
        }

        void wake_threads(priority_e prio, bool wake_all)
        {
            job_queue_t& queue = (prio == priority_e::HIGH) ? high_priority_queue : low_priority_queue;

            if (queue.sleepers.load(std::memory_order_seq_cst) == 0 && is_running.load(std::memory_order_relaxed))
            {
                return;
            }

            {
                std::scoped_lock lock(queue.wake_mutex);
            }

            if (wake_all) { queue.wake_cv.notify_all(); }
            else
            {
                queue.wake_cv.notify_one();
            }
        }
    } // namespace detail

    void init()
    {
        if (is_running) { return; }

        is_running = true;

        u32_t cores = std::thread::hardware_concurrency();
        u32_t num_high = std::max(1u, cores - 1);

        for (u32_t i = 0; i < num_high; i++)
        {
            high_priority_workers.emplace_back(worker_loop, std::ref(high_priority_queue));
        }

        low_priority_worker = std::thread(worker_loop, std::ref(low_priority_queue));
    }

    void shutdown()
    {
        is_running = false;

        detail::wake_threads(priority_e::HIGH, true);
        detail::wake_threads(priority_e::LOW, true);

        for (std::thread& worker : high_priority_workers)
        {
            if (worker.joinable()) { worker.join(); }
        }
        high_priority_workers.clear();

        if (low_priority_worker.joinable()) { low_priority_worker.join(); }
    }

    // the waiting thread steals jobs too
    void wait(counter_t* counter)
    {
        if (!counter) { return; }

        while (counter->load(std::memory_order_acquire) > 0)
        {
            job_t job;
            if (high_priority_queue.pop(job)) { run_job(job); }
            else
            {
                std::this_thread::yield();
            }
        }
    }

    bool wait_idle(u32_t timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (jobs_in_flight.load(std::memory_order_acquire) != 0)
        {
            if (std::chrono::steady_clock::now() >= deadline) { return false; }

            job_t job;
            if (high_priority_queue.pop(job)) { run_job(job); }
            else
            {
                std::this_thread::yield();
            }
        }
        return true;
    }

    u32_t get_worker_count() { return static_cast<u32_t>(high_priority_workers.size()); }
} // namespace tau::jobs