// Fiber.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "JobQueue.h"

namespace myengine::jobs
{
	struct Fiber;

	// The function every pooled fiber runs. It must never return: it switches away instead
	using FiberEntry = void (*)(Fiber& fiber);

#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier: Job is aligned to a cache line

	// A separate stack that a thread can switch to. Code running on it can be paused in the middle and continued later
	struct Fiber
	{
		void* handle = nullptr; // Win32 fiber
		FiberEntry entry = nullptr;
		std::uint32_t index = 0;
		const char* name = ""; // "Fiber 12". Tracy keeps the pointer, so the string is never freed

		Job job; // set by the scheduler before it switches to the fiber

		// Set by the fiber right before it switches back to the scheduler:
		// nullptr - the fiber has nothing to do and can be released, otherwise - pause it until this context is done
		Context* waitContext = nullptr;
		Fiber* nextWaiting = nullptr; // next fiber in the list of a context or in a list of fibers to resume
	};

#pragma warning(pop)

	// Turns the calling thread into a fiber so that it can switch to other fibers. Returns the handle of that fiber
	void* ConvertThreadToSchedulerFiber();
	void ConvertSchedulerFiberToThread();

	// Pauses the current fiber and continues the target one from the place where it stopped
	void SwitchToFiberHandle(void* handle);

	// Fibers are created once and reused: creating a stack for every job would be too slow
	class FiberPool
	{
	public:
		void Create(std::uint32_t fiberCount, std::size_t stackSize, FiberEntry entry);

		// Every fiber must be released by this moment
		void Destroy();

		// Returns nullptr when every fiber is busy
		Fiber* TryAcquire();
		void Release(Fiber* fiber);

		std::uint32_t GetFiberCount() const;

	private:
		std::unique_ptr<Fiber[]> fibers_;
		std::uint32_t fiberCount_ = 0;

		std::mutex mutex_;
		std::vector<Fiber*> freeFibers_; // the last released fiber is taken first: its stack is still in the CPU cache
	};
}
