// JobQueue.cpp

#include "JobQueue.h"

#include <immintrin.h>
#include <thread>
#include <utility>

namespace myengine::jobs
{
	namespace
	{
		constexpr std::size_t kInitialSharedCapacity = 64;

		void Pause(std::uint32_t& attempts)
		{
			if (++attempts < 64)
			{
				_mm_pause();
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	JobQueue::JobQueue() : slots_(std::make_unique<Slot[]>(kCapacity))
	{
		static_assert((kCapacity & (kCapacity - 1)) == 0, "JobQueue capacity must be a power of two");
		for (std::size_t index = 0; index < kCapacity; ++index)
		{
			slots_[index].sequence.store(index, std::memory_order_relaxed);
		}
	}

	bool JobQueue::Push(Job&& job)
	{
		const std::size_t bottom = bottom_.load(std::memory_order_relaxed);
		const std::size_t top = top_.load(std::memory_order_acquire);
		if (bottom - top >= kCapacity)
		{
			return false;
		}

		Slot& slot = slots_[bottom & (kCapacity - 1)];
		// A thief has claimed the old generation but has not moved it out yet. Falling back to the
		// shared queue is better than stopping the only thread allowed to push into this deque
		if (slot.sequence.load(std::memory_order_acquire) != bottom)
		{
			return false;
		}

		slot.job = std::move(job);
		slot.sequence.store(bottom + 1, std::memory_order_release);

		// Publishing bottom is the point at which thieves may see the new job
		bottom_.store(bottom + 1, std::memory_order_release);
		return true;
	}

	bool JobQueue::TryPop(Job& job)
	{
		const std::size_t oldBottom = bottom_.load(std::memory_order_relaxed);
		if (top_.load(std::memory_order_acquire) >= oldBottom)
		{
			return false;
		}

		const std::size_t bottom = oldBottom - 1;
		bottom_.store(bottom, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_seq_cst);

		std::size_t top = top_.load(std::memory_order_relaxed);
		if (top > bottom)
		{
			bottom_.store(oldBottom, std::memory_order_relaxed);
			return false;
		}

		if (top == bottom)
		{
			// The owner and thieves contend only for the final job
			if (!top_.compare_exchange_strong(
				top,
				top + 1,
				std::memory_order_seq_cst,
				std::memory_order_relaxed))
			{
				bottom_.store(oldBottom, std::memory_order_relaxed);
				return false;
			}
			bottom_.store(oldBottom, std::memory_order_relaxed);
		}

		Take(bottom, job);
		return true;
	}

	bool JobQueue::TrySteal(Job& job)
	{
		std::size_t top = top_.load(std::memory_order_acquire);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		const std::size_t bottom = bottom_.load(std::memory_order_acquire);
		if (top >= bottom)
		{
			return false;
		}

		const std::size_t claimedTop = top;
		if (!top_.compare_exchange_strong(
			top,
			top + 1,
			std::memory_order_seq_cst,
			std::memory_order_relaxed))
		{
			return false;
		}

		Take(claimedTop, job);
		return true;
	}

	void JobQueue::Take(const std::size_t index, Job& job)
	{
		Slot& slot = slots_[index & (kCapacity - 1)];
		std::uint32_t attempts = 0;
		while (slot.sequence.load(std::memory_order_acquire) != index + 1)
		{
			Pause(attempts);
		}

		job = std::move(slot.job);
		slot.sequence.store(index + kCapacity, std::memory_order_release);
	}

	void SharedJobQueue::Push(Job&& job)
	{
		std::lock_guard<std::mutex> lock(mutex_);

		if (count_ == buffer_.size())
		{
			Grow();
		}

		// The size is a power of two, so "& (size - 1)" wraps the position around the end of the buffer
		const std::size_t back = (head_ + count_) & (buffer_.size() - 1);
		buffer_[back] = std::move(job);
		++count_;
	}

	bool SharedJobQueue::TrySteal(Job& job)
	{
		std::lock_guard<std::mutex> lock(mutex_);

		if (count_ == 0)
		{
			return false;
		}

		job = std::move(buffer_[head_]);
		head_ = (head_ + 1) & (buffer_.size() - 1);
		--count_;
		return true;
	}

	void SharedJobQueue::Grow()
	{
		const std::size_t newCapacity = buffer_.empty() ? kInitialSharedCapacity : buffer_.size() * 2;

		// Move the jobs to the new buffer in their order, the oldest one goes to position 0
		std::vector<Job> newBuffer(newCapacity);
		for (std::size_t i = 0; i < count_; ++i)
		{
			newBuffer[i] = std::move(buffer_[(head_ + i) & (buffer_.size() - 1)]);
		}

		buffer_ = std::move(newBuffer);
		head_ = 0;
	}
}
