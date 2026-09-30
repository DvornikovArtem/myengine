// JobSystem.h

#pragma once

#include <atomic>
#include <cstdint>

#include <myengine/jobs/SmallFunction.h>

namespace myengine::jobs
{
	enum class Priority
	{
		High, // frame work: physics, ECS systems
		Streaming, // resource loading: blocking IO is allowed
		Count
	};

	// Counter of unfinished jobs. Lives on the caller's stack and must outlive Wait
	struct Context
	{
		std::atomic<std::uint32_t> counter{ 0 };
		Priority priority = Priority::High;
	};

	struct JobArgs
	{
		std::uint32_t jobIndex = 0; // index inside Dispatch
		std::uint32_t groupId = 0; // group number
		std::uint32_t groupIndex = 0; // index inside the group
		bool isFirstJobInGroup = false;
		bool isLastJobInGroup = false;
	};

	// A job must not capture more than 64 bytes: capture a pointer to a struct that lives until Wait
	using JobFunction = SmallFunction<void(JobArgs), 64>;

	// Creates worker threads once per session. ~0u means "as many as the CPU allows"
	void Initialize(std::uint32_t maxWorkerCount = ~0u);

	// Finishes every queued and running job, including the jobs they schedule, then stops the workers.
	// Main thread only, never from a job. Safe to call more than once
	void Shutdown();

	std::uint32_t GetWorkerCount(Priority priority = Priority::High);

	// One job
	void Execute(Context& context, JobFunction job);

	// jobCount jobs packed into groups of groupSize. A group runs as one job, one index after another
	void Dispatch(Context& context, std::uint32_t jobCount, std::uint32_t groupSize, JobFunction job);

	bool IsBusy(const Context& context);

	// Returns when context.counter reaches 0. The main thread runs other jobs while waiting
	void Wait(Context& context);
}