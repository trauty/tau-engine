#pragma once

#include "tau/defines.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <type_traits>

namespace tau::jobs
{
    struct counter_t : std::atomic<i32_t>
    {
        counter_t(i32_t v = 0) : std::atomic<i32_t>(v) {}
    };

    enum class priority_e
    {
        LOW, // io assets
        HIGH // everything else basically
    };

    template <size_t Capacity = 64>
    struct job_function
    {
        alignas(16) u8_t storage[Capacity];
        void (*invoke_func)(void*) = nullptr;

        job_function() = default;

        template <typename Lambda, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Lambda>, job_function>>>
        job_function(Lambda&& f)
        {
            using decayed_lambda_t = std::decay_t<Lambda>;

            static_assert(sizeof(decayed_lambda_t) <= Capacity, "Job lambda capture is too big");
            static_assert(
                std::is_trivially_copyable_v<decayed_lambda_t>,
                "Job lambdas can't capture STL containers or heavy objects by value, use kick_heavy() instead");

            std::memcpy(storage, &f, sizeof(decayed_lambda_t));

            invoke_func = [](void* data) { (*static_cast<decayed_lambda_t*>(data))(); };
        }

        void operator()() const
        {
            if (invoke_func) { invoke_func((void*)storage); }
        }

        explicit operator bool() const { return invoke_func != nullptr; }
    };

    namespace detail
    {
        TAU_ENGINE_API void push_job(job_function<64> task, counter_t* counter, priority_e prio);
        TAU_ENGINE_API void wake_threads(priority_e prio, bool wake_all);
    } // namespace detail

    template <typename Lambda>
    void kick(Lambda&& task, counter_t* counter = nullptr, priority_e prio = priority_e::HIGH)
    {
        if (counter) { counter->fetch_add(1, std::memory_order_relaxed); }

        detail::push_job(std::forward<Lambda>(task), counter, prio);
        detail::wake_threads(prio, false);
    }

    template <typename Lambda>
    void kick_heavy(Lambda&& task, counter_t* counter = nullptr, priority_e prio = priority_e::HIGH)
    {
        if (counter) { counter->fetch_add(1, std::memory_order_relaxed); }

        using decayed_lambda_t = std::decay_t<Lambda>;
        decayed_lambda_t* payload = new decayed_lambda_t(std::forward<Lambda>(task));

        auto wrapper_job = [payload]()
        {
            (*payload)();
            delete payload;
        };

        detail::push_job(job_function<64>(wrapper_job), counter, prio);
        detail::wake_threads(prio, false);
    }

    template <typename Lambda>
    void dispatch(u32_t count, u32_t batch_size, Lambda&& task, counter_t* counter = nullptr,
                  priority_e priority = priority_e::HIGH)
    {
        if (count == 0 || batch_size == 0) { return; }

        u32_t job_count = (count + batch_size - 1) / batch_size;

        if (counter) { counter->fetch_add(job_count, std::memory_order_relaxed); }

        for (u32_t i = 0; i < count; i += batch_size)
        {
            u32_t start = i;
            u32_t end = std::min(i + batch_size, count);

            auto batch_task = [task, start, end]() { task(start, end); };

            detail::push_job(job_function<64>(batch_task), counter, priority);
        }

        detail::wake_threads(priority, priority == priority_e::HIGH);
    }

    void init();
    void shutdown();

    TAU_ENGINE_API void wait(counter_t* counter);

    // waits until no job is queued or running, helping meanwhile, for at most `timeout_ms`, false if work remained
    // unloading a game library waits on this, as its jobs run its code
    TAU_ENGINE_API bool wait_idle(u32_t timeout_ms);

    TAU_ENGINE_API u32_t get_worker_count();
} // namespace tau::jobs