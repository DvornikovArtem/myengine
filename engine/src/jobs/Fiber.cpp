// Fiber.cpp

#include "Fiber.h"

#include <cassert>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace myengine::jobs
{
	namespace
	{
		constexpr std::uint32_t kMaxFiberCount = 1024;

		// Tracy identifies a fiber by the pointer to its name and may read the string at any time later,
		// so the names live in static memory and stay the same when the pool is created again
		const char* GetFiberName(std::uint32_t index)
		{
			static char names[kMaxFiberCount][16];
			static std::once_flag once;
			std::call_once(once, [] {
				for (std::uint32_t i = 0; i < kMaxFiberCount; ++i)
				{
					std::snprintf(names[i], sizeof(names[i]), "Fiber %u", i);
				}
			});
			return names[index];
		}

		// Windows calls this function on the new stack the first time the fiber is switched to
		void __stdcall FiberProc(void* parameter)
		{
			Fiber& fiber = *static_cast<Fiber*>(parameter);
			fiber.entry(fiber);

			// Returning from a fiber function ends the whole thread
			assert(false && "Fiber entry must never return");
		}
	}

	void* ConvertThreadToSchedulerFiber()
	{
		// FLOAT_SWITCH: the floating-point state is saved and restored together with the stack
		void* handle = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
		assert(handle != nullptr && "ConvertThreadToFiberEx failed");
		return handle;
	}

	void ConvertSchedulerFiberToThread()
	{
		ConvertFiberToThread();
	}

	void SwitchToFiberHandle(void* handle)
	{
		assert(handle != nullptr);
		SwitchToFiber(handle);
	}

	void FiberPool::Create(std::uint32_t fiberCount, std::size_t stackSize, FiberEntry entry)
	{
		assert(fibers_ == nullptr && "FiberPool is already created");
		assert(fiberCount <= kMaxFiberCount && "Not enough fiber names");

		fibers_ = std::make_unique<Fiber[]>(fiberCount);
		fiberCount_ = fiberCount;

		freeFibers_.clear();
		freeFibers_.reserve(fiberCount);

		// Fibers go to the free list in the reverse order, so that TryAcquire starts with fiber 0
		for (std::uint32_t i = fiberCount; i-- > 0;)
		{
			Fiber& fiber = fibers_[i];
			fiber.entry = entry;
			fiber.index = i;
			fiber.name = GetFiberName(i);

			// Commit size 0: the stack memory is only reserved and gets committed page by page when it is used
			fiber.handle = CreateFiberEx(0, stackSize, FIBER_FLAG_FLOAT_SWITCH, FiberProc, &fiber);
			assert(fiber.handle != nullptr && "CreateFiberEx failed");

			freeFibers_.push_back(&fiber);
		}
	}

	void FiberPool::Destroy()
	{
		assert(freeFibers_.size() == fiberCount_ && "Destroying the pool while some fibers are in use");

		for (std::uint32_t i = 0; i < fiberCount_; ++i)
		{
			DeleteFiber(fibers_[i].handle);
		}

		freeFibers_.clear();
		fibers_.reset();
		fiberCount_ = 0;
	}

	Fiber* FiberPool::TryAcquire()
	{
		std::lock_guard<std::mutex> lock(mutex_);

		if (freeFibers_.empty())
		{
			return nullptr;
		}

		Fiber* fiber = freeFibers_.back();
		freeFibers_.pop_back();
		return fiber;
	}

	void FiberPool::Release(Fiber* fiber)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		freeFibers_.push_back(fiber);
	}

	std::uint32_t FiberPool::GetFiberCount() const
	{
		return fiberCount_;
	}
}
