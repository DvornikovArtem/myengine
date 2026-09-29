// JobSystem.cpp

#include <myengine/jobs/JobSystem.h>

#include <algorithm>
#include <cassert>

namespace myengine::jobs
{
	namespace
	{
		bool gInitialized = false;
	}

	void Initialize(std::uint32_t maxWorkerCount)
	{
		(void)maxWorkerCount;

		if (gInitialized)
		{
			return;
		}
		gInitialized = true;
	}

	void Shutdown()
	{
		gInitialized = false;
	}

	std::uint32_t GetWorkerCount(Priority priority)
	{
		(void)priority;
		return 1; // only the calling thread
	}

	void Execute(Context& context, JobFunction job)
	{
		assert(gInitialized && "jobs::Initialize was not called");

		context.counter.fetch_add(1);

		JobArgs args;
		args.isFirstJobInGroup = true;
		args.isLastJobInGroup = true;
		job(args);

		context.counter.fetch_sub(1);
	}

	void Dispatch(Context& context, std::uint32_t jobCount, std::uint32_t groupSize, JobFunction job)
	{
		assert(gInitialized && "jobs::Initialize was not called");

		if (jobCount == 0 || groupSize == 0)
		{
			return;
		}

		// Round up: 100 jobs by 32 give 4 groups, the last one has 4 jobs
		const std::uint32_t groupCount = (jobCount + groupSize - 1) / groupSize;
		context.counter.fetch_add(groupCount);

		for (std::uint32_t groupId = 0; groupId < groupCount; ++groupId)
		{
			const std::uint32_t groupBegin = groupId * groupSize;
			const std::uint32_t groupEnd = groupBegin + std::min(groupSize, jobCount - groupBegin);

			JobArgs args;
			args.groupId = groupId;
			for (std::uint32_t jobIndex = groupBegin; jobIndex < groupEnd; ++jobIndex)
			{
				args.jobIndex = jobIndex;
				args.groupIndex = jobIndex - groupBegin;
				args.isFirstJobInGroup = jobIndex == groupBegin;
				args.isLastJobInGroup = jobIndex == groupEnd - 1;
				job(args);
			}

			// One group is one job for the counter
			context.counter.fetch_sub(1);
		}
	}

	bool IsBusy(const Context& context)
	{
		return context.counter.load() > 0;
	}

	void Wait(Context& context)
	{
		// Every job has already finished inside Execute / Dispatch
		assert(!IsBusy(context) && "Stub job system cannot have unfinished jobs");
		(void)context; // assert disappears in Release
	}
}