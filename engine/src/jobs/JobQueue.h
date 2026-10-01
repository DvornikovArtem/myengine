// JobQueue.h

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
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

	// Bounded Chase-Lev deque. One owner pushes and pops at the bottom, other threads steal at the top
#pragma warning(push)
#pragma warning(disable : 4324) // top and bottom intentionally live on separate cache lines

	class JobQueue
	{
	public:
		JobQueue();

		// Returns false when the bounded deque cannot accept the job. The caller keeps ownership of it
		bool Push(Job&& job);

		// Takes the newest job. Returns false if the queue is empty
		bool TryPop(Job& job);

		// Takes the oldest job. Returns false if the queue is empty
		bool TrySteal(Job& job);

	private:
		static constexpr std::size_t kCapacity = 1024;

		struct Slot
		{
			// Logical index that may be written into this slot. index + 1 means that the job is ready
			std::atomic<std::size_t> sequence{ 0 };
			Job job;
		};

		// nextIndex: the logical index that will be written into this slot next
		void Take(std::size_t index, std::size_t nextIndex, Job& job);

		std::unique_ptr<Slot[]> slots_;
		alignas(64) std::atomic<std::size_t> top_{ 0 };
		alignas(64) std::atomic<std::size_t> bottom_{ 0 };
	};

#pragma warning(pop)

	// Jobs submitted from outside a pool have no single owner, so this queue stays on a mutex
	class SharedJobQueue
	{
	public:
		void Push(Job&& job);
		bool TrySteal(Job& job);

	private:
		void Grow();

		std::mutex mutex_;
		std::vector<Job> buffer_; // ring buffer, its size is zero or a power of two
		std::size_t head_ = 0; // position of the oldest job
		std::size_t count_ = 0;
	};
}
