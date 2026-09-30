// JobQueue.cpp

#include "JobQueue.h"

#include <utility>

namespace myengine::jobs
{
	namespace
	{
		constexpr std::size_t kInitialCapacity = 64;
	}

	void JobQueue::Push(Job&& job)
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

	bool JobQueue::TryPop(Job& job)
	{
		std::lock_guard<std::mutex> lock(mutex_);

		if (count_ == 0)
		{
			return false;
		}

		--count_;
		const std::size_t back = (head_ + count_) & (buffer_.size() - 1);
		job = std::move(buffer_[back]);
		return true;
	}

	bool JobQueue::TrySteal(Job& job)
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

	void JobQueue::Grow()
	{
		const std::size_t newCapacity = buffer_.empty() ? kInitialCapacity : buffer_.size() * 2;

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