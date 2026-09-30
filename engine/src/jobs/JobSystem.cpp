// JobSystem.cpp

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

#include "JobQueue.h"

namespace myengine::jobs
{
	namespace
	{
		constexpr std::uint32_t kStreamingWorkerCount = 2;

		// Worker threads of one priority and everything they share
		struct Pool
		{
			const char* threadNamePrefix = "";
			std::vector<std::thread> threads;
			std::uint32_t workerCount = 0;

			// One queue per worker: only its owner pushes and pops, the others steal
			std::unique_ptr<JobQueue[]> workerQueues;
			// Jobs from the threads that are not workers of this pool (the main thread, the other pool)
			JobQueue sharedQueue;
			// Jobs in all the queues of the pool. Grows under sleepMutex, so that a worker cannot miss new work
			std::atomic<std::uint32_t> queuedJobCount{ 0 };

			std::mutex sleepMutex;
			std::condition_variable wakeUp;
			bool stopRequested = false; // guarded by sleepMutex
		};

		Pool gPools[static_cast<std::size_t>(Priority::Count)];
		std::atomic<bool> gInitialized{ false };

		// Jobs of all the pools that are queued or running. Shutdown stops the threads only when it is 0
		std::atomic<std::uint32_t> gUnfinishedJobCount{ 0 };

		// Which pool the current thread works for. nullptr for the threads that are not workers
		thread_local Pool* tCurrentPool = nullptr;
		thread_local std::uint32_t tWorkerIndex = 0;

		Pool& GetPool(Priority priority)
		{
			return gPools[static_cast<std::size_t>(priority)];
		}

		// Tells the pool that jobCount jobs are about to be pushed. Must be called before PushJob
		void AnnounceJobs(Pool& pool, std::uint32_t jobCount)
		{
			gUnfinishedJobCount.fetch_add(jobCount);

			std::lock_guard<std::mutex> lock(pool.sleepMutex);
			pool.queuedJobCount.fetch_add(jobCount);
		}

		void PushJob(Pool& pool, Job&& job)
		{
			if (tCurrentPool == &pool)
			{
				pool.workerQueues[tWorkerIndex].Push(std::move(job));
			}
			else
			{
				pool.sharedQueue.Push(std::move(job));
			}
		}

		// Order: own queue, then the shared one, then stealing from the other workers in a circle
		bool TryGetJob(Pool& pool, Job& job)
		{
			const bool isWorkerOfPool = tCurrentPool == &pool;

			bool found = isWorkerOfPool && pool.workerQueues[tWorkerIndex].TryPop(job);

			if (!found)
			{
				found = pool.sharedQueue.TrySteal(job);
			}

			if (!found)
			{
				const std::uint32_t firstVictim = isWorkerOfPool ? tWorkerIndex + 1 : 0;
				for (std::uint32_t i = 0; i < pool.workerCount && !found; ++i)
				{
					const std::uint32_t victim = (firstVictim + i) % pool.workerCount;
					if (isWorkerOfPool && victim == tWorkerIndex)
					{
						continue;
					}
					found = pool.workerQueues[victim].TrySteal(job);
				}
			}

			if (found)
			{
				pool.queuedJobCount.fetch_sub(1);
			}
			return found;
		}

		void RunJob(Job& job)
		{
			ZoneScopedN("Jobs::RunJob");

			JobArgs args;
			args.groupId = job.groupId;
			for (std::uint32_t jobIndex = job.firstJobIndex; jobIndex < job.endJobIndex; ++jobIndex)
			{
				args.jobIndex = jobIndex;
				args.groupIndex = jobIndex - job.firstJobIndex;
				args.isFirstJobInGroup = jobIndex == job.firstJobIndex;
				args.isLastJobInGroup = jobIndex == job.endJobIndex - 1;
				job.function(args);
			}

			// Release the captured data first: after the decrement the waiting thread may destroy what it refers to
			job.function.Reset();

			// The last touch of the context: it may be destroyed right after the counter reaches 0
			job.context->counter.fetch_sub(1);

			// After everything else: the jobs scheduled by this job are already counted
			gUnfinishedJobCount.fetch_sub(1);
		}

		void WorkerLoop(Pool& pool, std::uint32_t workerIndex)
		{
			tCurrentPool = &pool;
			tWorkerIndex = workerIndex;

			const std::string threadName = std::string(pool.threadNamePrefix) + " " + std::to_string(workerIndex);
			tracy::SetThreadName(threadName.c_str());

			Job job;
			while (true)
			{
				if (TryGetJob(pool, job))
				{
					RunJob(job);
					continue;
				}

				// The check and falling asleep are under the same mutex as AnnounceJobs, so a wake-up cannot be lost
				std::unique_lock<std::mutex> lock(pool.sleepMutex);
				pool.wakeUp.wait(lock, [&pool] { return pool.stopRequested || pool.queuedJobCount.load() > 0; });

				// Queued jobs are finished even when the pool is stopping
				if (pool.stopRequested && pool.queuedJobCount.load() == 0)
				{
					return;
				}
			}
		}

		void StartPool(Pool& pool, const char* threadNamePrefix, std::uint32_t workerCount)
		{
			pool.threadNamePrefix = threadNamePrefix;
			pool.workerCount = workerCount;
			pool.workerQueues = std::make_unique<JobQueue[]>(workerCount);
			pool.queuedJobCount = 0;
			pool.stopRequested = false;

			// Everything a worker reads must be ready before the first thread starts
			pool.threads.reserve(workerCount);
			for (std::uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
			{
				pool.threads.emplace_back(WorkerLoop, std::ref(pool), workerIndex);
			}
		}

		void StopPool(Pool& pool)
		{
			{
				// Under the mutex, so that a worker cannot miss the request between its check and falling asleep
				std::lock_guard<std::mutex> lock(pool.sleepMutex);
				pool.stopRequested = true;
			}
			pool.wakeUp.notify_all();

			for (std::thread& thread : pool.threads)
			{
				thread.join();
			}
			pool.threads.clear();
			pool.workerQueues.reset();
			pool.workerCount = 0;
		}
	}

	void Initialize(std::uint32_t maxWorkerCount)
	{
		if (gInitialized)
		{
			return;
		}

		// hardware_concurrency returns 0 when the core count is unknown
		const std::uint32_t coreCount = std::max(1u, std::thread::hardware_concurrency());

		// One core is left for the main thread: it runs jobs too while it waits
		const std::uint32_t highWorkerCount = std::clamp(coreCount - 1, 1u, std::max(1u, maxWorkerCount));

		StartPool(GetPool(Priority::High), "Worker", highWorkerCount);
		StartPool(GetPool(Priority::Streaming), "Streaming", kStreamingWorkerCount);

		gInitialized = true;
	}

	void Shutdown()
	{
		// Application::Shutdown runs twice (from main and from the destructor), so this must be safe to repeat
		if (!gInitialized)
		{
			return;
		}

		assert(tCurrentPool == nullptr && "jobs::Shutdown must not be called from a job");

		ZoneScopedN("Jobs::Shutdown");

		// Phase 1: finish all the work. A running job may still schedule jobs into any pool,
		// so every pool keeps working until not a single job is queued or running
		Job job;
		while (gUnfinishedJobCount.load() > 0)
		{
			bool hasRun = false;
			for (Pool& pool : gPools)
			{
				if (TryGetJob(pool, job))
				{
					RunJob(job);
					hasRun = true;
					break;
				}
			}

			if (!hasRun)
			{
				std::this_thread::yield();
			}
		}

		// Phase 2: nothing is running, so nobody can schedule a new job. Now the threads can be stopped
		for (Pool& pool : gPools)
		{
			StopPool(pool);
		}

		gInitialized = false;
	}

	std::uint32_t GetWorkerCount(Priority priority)
	{
		return GetPool(priority).workerCount;
	}

	void Execute(Context& context, JobFunction job)
	{
		assert(gInitialized && "jobs::Initialize was not called");

		Pool& pool = GetPool(context.priority);

		Job queuedJob;
		queuedJob.function = std::move(job);
		queuedJob.context = &context;
		queuedJob.firstJobIndex = 0;
		queuedJob.endJobIndex = 1;

		context.counter.fetch_add(1);
		AnnounceJobs(pool, 1);
		PushJob(pool, std::move(queuedJob));
		pool.wakeUp.notify_one();
	}

	void Dispatch(Context& context, std::uint32_t jobCount, std::uint32_t groupSize, JobFunction job)
	{
		assert(gInitialized && "jobs::Initialize was not called");

		if (jobCount == 0 || groupSize == 0)
		{
			return;
		}

		ZoneScopedN("Jobs::Dispatch");

		Pool& pool = GetPool(context.priority);

		// Round up: 100 jobs by 32 give 4 groups, the last one has 4 jobs
		const std::uint32_t groupCount = (jobCount + groupSize - 1) / groupSize;

		// One group is one job for the counter
		context.counter.fetch_add(groupCount);
		AnnounceJobs(pool, groupCount);

		for (std::uint32_t groupId = 0; groupId < groupCount; ++groupId)
		{
			Job queuedJob;
			queuedJob.function = job; // every group gets its own copy
			queuedJob.context = &context;
			queuedJob.groupId = groupId;
			queuedJob.firstJobIndex = groupId * groupSize;
			queuedJob.endJobIndex = queuedJob.firstJobIndex + std::min(groupSize, jobCount - queuedJob.firstJobIndex);
			PushJob(pool, std::move(queuedJob));
		}

		if (groupCount == 1)
		{
			pool.wakeUp.notify_one();
		}
		else
		{
			pool.wakeUp.notify_all();
		}
	}

	bool IsBusy(const Context& context)
	{
		return context.counter.load() > 0;
	}

	void Wait(Context& context)
	{
		assert((gInitialized || !IsBusy(context)) && "Waiting for jobs that nobody will run: jobs::Shutdown was already called");

		ZoneScopedN("Jobs::Wait");

		Pool& pool = GetPool(context.priority);

		Job job;
		while (IsBusy(context))
		{
			// The waiting thread does not idle: it runs jobs of the same pool itself
			if (TryGetJob(pool, job))
			{
				RunJob(job);
			}
			else
			{
				// The remaining jobs are already taken by other threads: let them finish
				std::this_thread::yield();
			}
		}
	}
}