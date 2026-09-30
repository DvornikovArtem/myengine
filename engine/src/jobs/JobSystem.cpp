// JobSystem.cpp

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

#include "Fiber.h"
#include "JobQueue.h"

// A job paused in Wait may continue on another thread, so its zones begin and end on different threads.
// Tracy accepts that only in the fiber mode
#ifndef TracyFiberEnter
#error "Define TRACY_FIBERS together with TRACY_ENABLE: the job system runs jobs on fibers"
#endif

namespace myengine::jobs
{
	namespace
	{
		constexpr std::uint32_t kStreamingWorkerCount = 2;
		// How many jobs can be paused in Wait or running on workers at the same time. When every fiber is taken,
		// a worker runs the job on its own stack and waits there the way the main thread does
		constexpr std::uint32_t kFiberCount = 128;
		// A job that needs a deeper stack crashes with a stack overflow
		constexpr std::size_t kFiberStackSize = 128 * 1024;

		// Worker threads of one priority and everything they share
		struct Pool
		{
			const char* threadNamePrefix = "";
			std::vector<std::thread> threads;
			std::uint32_t workerCount = 0;
			bool usesFibers = false; // High: jobs run on pooled fibers. Streaming: on the stack of the thread

			// One queue per worker: only its owner pushes and pops, the others steal
			std::unique_ptr<JobQueue[]> workerQueues;
			// Jobs from the threads that are not workers of this pool (the main thread, the other pool)
			SharedJobQueue sharedQueue;
			// Jobs in all the queues of the pool. Grows under sleepMutex, so that a worker cannot miss new work
			std::atomic<std::uint32_t> queuedJobCount{ 0 };

			// Paused fibers whose context is done: any worker of the pool may continue them. Guarded by sleepMutex
			std::vector<Fiber*> readyFibers;
			// Size of readyFibers, readable without the mutex
			std::atomic<std::uint32_t> readyFiberCount{ 0 };

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

		FiberPool gFiberPool;

		// The fiber a worker thread was converted to. It runs the worker loop and switches to the pooled fibers.
		// A pooled fiber reads it every time it switches back: it may be running on another thread by then
		thread_local void* tSchedulerFiber = nullptr;

		// The pooled fiber running on this thread right now. nullptr on a scheduler fiber and on threads without fibers
		thread_local Fiber* tCurrentFiber = nullptr;

		// How many jobs are running on top of each other on the stack of this thread because of Wait:
		// a job taken while waiting may wait too and take one more job on top of itself, and so on
		thread_local std::uint32_t tNestedWaitDepth = 0;
		constexpr std::uint32_t kMaxNestedWaitDepth = 8;

		Pool& GetPool(Priority priority)
		{
			return gPools[static_cast<std::size_t>(priority)];
		}

		// A spinlock: it is held for a few instructions only, so spinning is cheaper than putting the thread to sleep
		void LockContext(Context& context)
		{
			std::uint32_t attempts = 0;
			while (context.waitLock.exchange(true))
			{
				if (++attempts < 64)
				{
					_mm_pause();
				}
				else
				{
					// The owner was probably taken off its core: let it run
					std::this_thread::yield();
				}
			}
		}

		void UnlockContext(Context& context)
		{
			context.waitLock.store(false);
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
				if (pool.workerQueues[tWorkerIndex].Push(std::move(job)))
				{
					return;
				}
			}

			// External submissions and the rare local overflow go through the multi-producer queue
			pool.sharedQueue.Push(std::move(job));
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

		// Moves the paused fibers of a finished context to the workers. Fibers always belong to the High pool
		void MakeFibersReady(Fiber* firstFiber)
		{
			Pool& pool = GetPool(Priority::High);

			std::uint32_t fiberCount = 0;
			{
				std::lock_guard<std::mutex> lock(pool.sleepMutex);
				for (Fiber* fiber = firstFiber; fiber != nullptr;)
				{
					// Read the link first: once the fiber is in readyFibers, it does not belong to this list any more
					Fiber* next = fiber->nextWaiting;
					fiber->nextWaiting = nullptr;
					pool.readyFibers.push_back(fiber);
					++fiberCount;
					fiber = next;
				}
				pool.readyFiberCount.fetch_add(fiberCount);
			}

			if (fiberCount == 1)
			{
				pool.wakeUp.notify_one();
			}
			else
			{
				pool.wakeUp.notify_all();
			}
		}

		Fiber* TryGetReadyFiber(Pool& pool)
		{
			if (pool.readyFiberCount.load() == 0)
			{
				return nullptr;
			}

			std::lock_guard<std::mutex> lock(pool.sleepMutex);
			if (pool.readyFibers.empty())
			{
				return nullptr;
			}

			Fiber* fiber = pool.readyFibers.back();
			pool.readyFibers.pop_back();
			pool.readyFiberCount.fetch_sub(1);
			return fiber;
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

			// The counter changes under the lock of the context. Unlocking is the last touch of the context:
			// Wait takes the same lock before it returns, so the context cannot be destroyed in between
			// The decrement and taking the list of paused fibers are one step under the lock:
			// a fiber is added to the list under the same lock, so it cannot be added after the list was taken
			Context& context = *job.context;
			Fiber* waitingFibers = nullptr;
			LockContext(context);
			if (context.counter.fetch_sub(1) == 1)
			{
				waitingFibers = context.waitingFibers;
				context.waitingFibers = nullptr;
			}
			UnlockContext(context);

			if (waitingFibers != nullptr)
			{
				MakeFibersReady(waitingFibers);
			}

			// After everything else: the jobs scheduled by this job are already counted
			gUnfinishedJobCount.fetch_sub(1);
		}

		// The body of every pooled fiber: runs the job it was given, then more jobs while there are any
		void FiberMain(Fiber& fiber)
		{
			Pool& pool = GetPool(Priority::High);
			while (true)
			{
				RunJob(fiber.job);

				// Staying on the same fiber is cheaper than switching twice for every job.
				// Paused fibers that are ready go first: only a scheduler can continue them
				while (pool.readyFiberCount.load() == 0 && TryGetJob(pool, fiber.job))
				{
					RunJob(fiber.job);
				}

				// Back to the scheduler of the thread this fiber is running on now
				SwitchToFiberHandle(tSchedulerFiber);
			}
		}

		// Called on a scheduler fiber: switches to a pooled fiber and deals with it when it switches back
		void RunFiber(Fiber* fiber)
		{
			while (true)
			{
				tCurrentFiber = fiber;

				// Everything this thread records from now on belongs to the fiber, not to the thread
				TracyFiberEnter(fiber->name);
				SwitchToFiberHandle(fiber->handle);
				TracyFiberLeave;

				tCurrentFiber = nullptr;

				// The scheduler continues here when the fiber has switched back, so nobody uses its stack any more.
				// Only from this moment the fiber may be given to other threads
				Context* waitContext = fiber->waitContext;
				if (waitContext == nullptr)
				{
					gFiberPool.Release(fiber);
					return;
				}
				fiber->waitContext = nullptr;

				// The fiber asked to be paused. The check and adding to the list are under the same lock as the decrement
				// in RunJob, so the last job cannot slip in between and leave the fiber paused forever
				LockContext(*waitContext);
				const bool isBusy = waitContext->counter.load() > 0;
				if (isBusy)
				{
					fiber->nextWaiting = waitContext->waitingFibers;
					waitContext->waitingFibers = fiber;
				}
				UnlockContext(*waitContext);

				if (isBusy)
				{
					// The fiber is in the list: another thread may be running it already, so it must not be touched here
					return;
				}

				// The context finished while the fiber was switching: continue the fiber right away
			}
		}

		// Called on a scheduler fiber
		void RunJobOnFiber(Job& job)
		{
			Fiber* fiber = gFiberPool.TryAcquire();
			if (fiber == nullptr)
			{
				// Every fiber is busy: the job runs on the stack of the scheduler itself
				RunJob(job);
				return;
			}

			fiber->job = std::move(job);
			RunFiber(fiber);
		}

		void WorkerLoop(Pool& pool, std::uint32_t workerIndex)
		{
			tCurrentPool = &pool;
			tWorkerIndex = workerIndex;

			const std::string threadName = std::string(pool.threadNamePrefix) + " " + std::to_string(workerIndex);
			tracy::SetThreadName(threadName.c_str());

			if (pool.usesFibers)
			{
				tSchedulerFiber = ConvertThreadToSchedulerFiber();
			}

			Job job;
			while (true)
			{
				// Paused jobs that can continue go before new jobs: they were started earlier
				if (pool.usesFibers)
				{
					if (Fiber* readyFiber = TryGetReadyFiber(pool))
					{
						RunFiber(readyFiber);
						continue;
					}
				}

				if (TryGetJob(pool, job))
				{
					if (pool.usesFibers)
					{
						RunJobOnFiber(job);
					}
					else
					{
						RunJob(job);
					}
					continue;
				}

				// The check and falling asleep are under the same mutex as AnnounceJobs and MakeFibersReady,
				// so a wake-up cannot be lost
				std::unique_lock<std::mutex> lock(pool.sleepMutex);
				pool.wakeUp.wait(lock, [&pool] {
					return pool.stopRequested || pool.queuedJobCount.load() > 0 || pool.readyFiberCount.load() > 0;
				});

				// Queued jobs and ready fibers are finished even when the pool is stopping
				if (pool.stopRequested && pool.queuedJobCount.load() == 0 && pool.readyFiberCount.load() == 0)
				{
					break;
				}
			}

			if (pool.usesFibers)
			{
				ConvertSchedulerFiberToThread();
				tSchedulerFiber = nullptr;
			}
		}

		void StartPool(Pool& pool, const char* threadNamePrefix, std::uint32_t workerCount, bool usesFibers)
		{
			pool.threadNamePrefix = threadNamePrefix;
			pool.workerCount = workerCount;
			pool.usesFibers = usesFibers;
			pool.workerQueues = std::make_unique<JobQueue[]>(workerCount);
			pool.queuedJobCount = 0;
			pool.readyFibers.clear();
			pool.readyFibers.reserve(usesFibers ? kFiberCount : 0); // no allocations under sleepMutex later
			pool.readyFiberCount = 0;
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

		// Before the workers start: they take fibers from the pool right away
		gFiberPool.Create(kFiberCount, kFiberStackSize, FiberMain);

		StartPool(GetPool(Priority::High), "Worker", highWorkerCount, true);
		StartPool(GetPool(Priority::Streaming), "Streaming", kStreamingWorkerCount, false);

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

		// The threads are joined, so every fiber is back in the pool
		gFiberPool.Destroy();

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

		if (Fiber* fiber = tCurrentFiber)
		{
			// Inside a job on a pooled fiber: the job is paused and the worker takes other work
			while (IsBusy(context))
			{
				// The fiber does not add itself to the list of the context: another thread could continue it
				// while it is still on its stack. The scheduler does it after the switch, see RunFiber
				fiber->waitContext = &context;
				SwitchToFiberHandle(tSchedulerFiber);

				// The job continues here when the context is done, maybe on another thread
			}
		}
		else
		{
			// Not on a pooled fiber: the main thread, a Streaming worker,
			// or a scheduler that ran the job itself because every fiber was busy
			Pool& pool = GetPool(context.priority);
			Pool& fiberPool = GetPool(Priority::High);
			const bool isScheduler = tSchedulerFiber != nullptr;

			// A thread that is not a worker only helps, the workers finish the jobs without it. So it may stop
			// taking jobs when they are nested too deep on its stack. A worker must always go on:
			// with every worker of its pool waiting there would be nobody left to run the jobs
			const bool canRunJobs = tCurrentPool != nullptr || tNestedWaitDepth < kMaxNestedWaitDepth;

			Job job;
			while (IsBusy(context))
			{
				// Only a scheduler can continue paused fibers, and this context may depend on one of them
				if (isScheduler)
				{
					if (Fiber* readyFiber = TryGetReadyFiber(fiberPool))
					{
						RunFiber(readyFiber);
						continue;
					}
				}

				// The waiting thread does not idle: it runs jobs of the same pool itself
				if (canRunJobs && TryGetJob(pool, job))
				{
					++tNestedWaitDepth;
					RunJob(job);
					--tNestedWaitDepth;
				}
				else
				{
					// The remaining jobs are already taken by other threads: let them finish
					std::this_thread::yield();
				}
			}
		}

		// The job that made the counter 0 may still be unlocking the context: wait until it has left
		LockContext(context);
		UnlockContext(context);
	}
}
