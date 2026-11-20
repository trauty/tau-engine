#include "jolt_job_system_int.h"

#include "tau/jobs.h"

#include <chrono>
#include <thread>

namespace tau
{
    jolt_job_system_integration_t::jolt_job_system_integration_t() : JPH::JobSystemWithBarrier(1024)
    { m_jobs.Init(MAX_JOBS, MAX_JOBS); }

    int jolt_job_system_integration_t::GetMaxConcurrency() const { return (int)tau::jobs::get_worker_count(); }

    JPH::JobHandle jolt_job_system_integration_t::CreateJob(const char* in_name, JPH::ColorArg in_color,
                                                            const JPH::JobSystem::JobFunction& in_job_func,
                                                            JPH::uint32 in_num_deps)
    {
        JPH::uint32 index;
        while ((index = m_jobs.ConstructObject(in_name, in_color, this, in_job_func, in_num_deps)) ==
               JPH::FixedSizeFreeList<JPH::JobSystem::Job>::cInvalidObjectIndex)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        JPH::JobSystem::Job* job = &m_jobs.Get(index);
        JPH::JobHandle handle(job);

        if (in_num_deps == 0) { QueueJob(job); }
        return handle;
    }

    void jolt_job_system_integration_t::QueueJob(JPH::JobSystem::Job* in_job)
    {
        in_job->AddRef();
        tau::jobs::kick(
            [in_job]()
            {
                in_job->Execute();
                in_job->Release();
            },
            nullptr, tau::jobs::priority_e::HIGH);
    }

    void jolt_job_system_integration_t::QueueJobs(JPH::JobSystem::Job** in_jobs, JPH::uint32 in_num_jobs)
    {
        for (JPH::uint32 i = 0; i < in_num_jobs; i++) { QueueJob(in_jobs[i]); }
    }

    void jolt_job_system_integration_t::FreeJob(JPH::JobSystem::Job* in_job) { m_jobs.DestructObject(in_job); }
} // namespace tau
