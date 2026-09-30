// JobQueue.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <myengine/jobs/JobSystem.h>

namespace myengine::jobs
{
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier: the padding is intended

	// One unit of work for a worker: a single Execute or one group of a Dispatch
	// Aligned to a cache line so that two workers never write to the same line (no false sharing)
	struct alignas(64) Job
	{
		JobFunction function;
		Context* context = nullptr;
		std::uint32_t groupId = 0;
		std::uint32_t firstJobIndex = 0; // first jobIndex of the group
		std::uint32_t endJobIndex = 0; // one past the last jobIndex of the group
	};

#pragma warning(pop)

	// Queue of jobs protected by a mutex
	// The owner works with the back (newest jobs), other threads steal from the front (oldest jobs)
	class JobQueue
	{
	public:
		void Push(Job&& job);

		// Takes the newest job. Returns false if the queue is empty
		bool TryPop(Job& job);

		// Takes the oldest job. Returns false if the queue is empty
		bool TrySteal(Job& job);

	private:
		void Grow();

		std::mutex mutex_;
		std::vector<Job> buffer_; // ring buffer, its size is zero or a power of two
		std::size_t head_ = 0; // position of the oldest job
		std::size_t count_ = 0;
	};
}