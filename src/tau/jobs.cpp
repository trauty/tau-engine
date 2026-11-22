#include "jobs.h"

#include "tau/defines.h"
#include "tau/log.h"
#include "tau/profiling.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
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
        job_queue_t io_queue;

        // frame workers park on the high queue's wake, LOW work wakes them there too
        std::vector<std::thread> high_priority_workers;
        std::thread io_worker;
        std::atomic<bool> is_running{false};
        // LOW jobs running now, capped so frame jobs always find free workers
        std::atomic<u32_t> low_running{0};
        u32_t low_cap = 1;
        // pushed and not yet finished, counted before the push so no gap shows idle
        std::atomic<u32_t> jobs_in_flight{0};

        void run_job(const job_t& job)
        {
            if (job.task) { job.task(); }
            if (job.counter) { job.counter->fetch_sub(1, std::memory_order_release); }
            jobs_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        }

        bool low_takeable()
        { return low_priority_queue.has_work() && low_running.load(std::memory_order_relaxed) < low_cap; }

        // `also_low`: a frame worker, which also wakes for LOW work
        void park(job_queue_t& queue, bool also_low)
        {
            queue.sleepers.fetch_add(1, std::memory_order_seq_cst);

            {
                std::unique_lock lock(queue.wake_mutex);
                queue.wake_cv.wait(lock,
                                   [&queue, also_low]
                                   {
                                       return !is_running.load(std::memory_order_relaxed) || queue.has_work() ||
                                              (also_low && low_takeable());
                                   });
            }

            queue.sleepers.fetch_sub(1, std::memory_order_relaxed);
        }

        // a LOW job under the cap
        bool run_low()
        {
            if (low_running.fetch_add(1, std::memory_order_acq_rel) >= low_cap)
            {
                low_running.fetch_sub(1, std::memory_order_acq_rel);
                return false;
            }

            job_t job;
            const bool found = low_priority_queue.pop(job);
            if (found) { run_job(job); }
            // a parked worker may have skipped LOW work while the cap was reached
            if (low_running.fetch_sub(1, std::memory_order_acq_rel) == low_cap && low_priority_queue.has_work())
            {
                detail::wake_threads(priority_e::LOW, false);
            }
            return found;
        }

        // HIGH first, LOW only while HIGH is empty
        bool run_one_frame_worker()
        {
            job_t job;
            if (high_priority_queue.pop(job))
            {
                run_job(job);
                return true;
            }
            return run_low();
        }

        void frame_worker_loop(u32_t index)
        {
#ifdef TAU_ENABLE_PROFILING
            const std::string name = "Worker " + std::to_string(index);
            tracy::SetThreadName(name.c_str());
#else
            (void)index;
#endif
            while (is_running.load(std::memory_order_relaxed))
            {
                if (run_one_frame_worker()) { continue; }

                bool found = false;
                for (u32_t spin = 0; spin < SPIN_ROUNDS && !found; spin++)
                {
                    found = run_one_frame_worker();
                    if (!found) { std::this_thread::yield(); }
                }

                if (!found && is_running.load(std::memory_order_relaxed)) { park(high_priority_queue, true); }
            }
        }

        void io_worker_loop()
        {
#ifdef TAU_ENABLE_PROFILING
            tracy::SetThreadName("IO");
#endif
            while (is_running.load(std::memory_order_relaxed))
            {
                job_t job;
                if (io_queue.pop(job)) { run_job(job); }
                else if (is_running.load(std::memory_order_relaxed)) { park(io_queue, false); }
            }
        }

        job_queue_t& queue_of(priority_e prio)
        {
            switch (prio)
            {
            case priority_e::HIGH: return high_priority_queue;
            case priority_e::LOW: return low_priority_queue;
            case priority_e::IO: return io_queue;
            }
            return high_priority_queue;
        }
    } // namespace

    namespace detail
    {
        void push_job(job_function<64> task, counter_t* counter, priority_e prio)
        {
            job_queue_t& queue = queue_of(prio);

            jobs_in_flight.fetch_add(1, std::memory_order_acq_rel);
            if (queue.push(task, counter)) { return; }

            TAU_LOG_WARN("JOBS", "Job queue full ({} slots), running job on the submitting thread", MAX_JOBS);

            run_job({task, counter});
        }

        void wake_threads(priority_e prio, bool wake_all)
        {
            // LOW work is run by the frame workers, parked on the high queue
            job_queue_t& queue = prio == priority_e::IO ? io_queue : high_priority_queue;

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
        low_cap = num_high > 2 ? num_high - 2 : 1;

        for (u32_t i = 0; i < num_high; i++) { high_priority_workers.emplace_back(frame_worker_loop, i); }

        io_worker = std::thread(io_worker_loop);
    }

    void shutdown()
    {
        is_running = false;

        detail::wake_threads(priority_e::HIGH, true);
        detail::wake_threads(priority_e::IO, true);

        for (std::thread& worker : high_priority_workers)
        {
            if (worker.joinable()) { worker.join(); }
        }
        high_priority_workers.clear();

        if (io_worker.joinable()) { io_worker.join(); }
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
            if (high_priority_queue.pop(job) || low_priority_queue.pop(job) || io_queue.pop(job)) { run_job(job); }
            else
            {
                std::this_thread::yield();
            }
        }
        return true;
    }

    u32_t get_worker_count() { return static_cast<u32_t>(high_priority_workers.size()); }
} // namespace tau::jobs