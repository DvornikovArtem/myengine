#include <myengine/jobs/JobSystem.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
	constexpr std::uint32_t kWorkerCount = 4;
	constexpr std::uint32_t kJobCount = 100'000;
	constexpr std::uint32_t kRoundCount = 20;

	struct BatchResult
	{
		explicit BatchResult(const std::uint32_t jobCount)
			: seen(std::make_unique<std::atomic<std::uint32_t>[]>(jobCount))
		{
			for (std::uint32_t i = 0; i < jobCount; ++i)
			{
				seen[i].store(0, std::memory_order_relaxed);
			}
		}

		std::unique_ptr<std::atomic<std::uint32_t>[]> seen;
		std::atomic<std::uint32_t> executed{ 0 };
		std::atomic<std::uint32_t> duplicates{ 0 };
		std::atomic<bool> ranOutsideOwner{ false };
	};

	bool RunBatch(const std::uint32_t jobCount)
	{
		BatchResult result(jobCount);
		std::atomic<bool> rootStarted{ false };
		std::atomic<bool> mainReady{ false };
		std::atomic<bool> releaseJobs{ false };

		myengine::jobs::Context rootContext;
		myengine::jobs::Execute(rootContext, [&](myengine::jobs::JobArgs)
		{
			const std::thread::id ownerThread = std::this_thread::get_id();
			rootStarted.store(true, std::memory_order_release);
			while (!mainReady.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}

			myengine::jobs::Context childContext;
			myengine::jobs::Dispatch(childContext, jobCount, 1,
				[&, ownerThread](const myengine::jobs::JobArgs args)
				{
					// Keep consumers occupied until Dispatch has filled the local deque and overflowed it
					while (!releaseJobs.load(std::memory_order_acquire))
					{
						std::this_thread::yield();
					}

					if (std::this_thread::get_id() != ownerThread)
					{
						result.ranOutsideOwner.store(true, std::memory_order_relaxed);
					}

					if (result.seen[args.jobIndex].fetch_add(1, std::memory_order_relaxed) != 0)
					{
						result.duplicates.fetch_add(1, std::memory_order_relaxed);
					}
					result.executed.fetch_add(1, std::memory_order_relaxed);
				});

			releaseJobs.store(true, std::memory_order_release);
			myengine::jobs::Wait(childContext);
		});

		// Do not let the main thread take the root job: its child jobs must be submitted from a worker
		while (!rootStarted.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		mainReady.store(true, std::memory_order_release);
		myengine::jobs::Wait(rootContext);

		std::uint32_t missing = 0;
		for (std::uint32_t i = 0; i < jobCount; ++i)
		{
			if (result.seen[i].load(std::memory_order_relaxed) == 0)
			{
				++missing;
			}
		}

		if (result.executed.load(std::memory_order_relaxed) != jobCount ||
			result.duplicates.load(std::memory_order_relaxed) != 0 || missing != 0 ||
			!result.ranOutsideOwner.load(std::memory_order_relaxed))
		{
			std::cerr << "FAILED: executed=" << result.executed.load()
				<< " duplicates=" << result.duplicates.load()
				<< " missing=" << missing
				<< " stolen=" << result.ranOutsideOwner.load() << '\n';
			return false;
		}

		return true;
	}
}

int main()
{
	myengine::jobs::Initialize(kWorkerCount);
	std::cout << "High workers: " << myengine::jobs::GetWorkerCount() << '\n';

	const auto start = std::chrono::steady_clock::now();
	bool passed = true;
	for (std::uint32_t round = 0; round < kRoundCount; ++round)
	{
		if (!RunBatch(kJobCount))
		{
			passed = false;
			break;
		}
	}

	myengine::jobs::Shutdown();

	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - start);
	if (!passed)
	{
		return 1;
	}

	std::cout << "OK: " << static_cast<std::uint64_t>(kJobCount) * kRoundCount
		<< " jobs, successful stealing, no missing or duplicate executions, "
		<< elapsed.count() << " ms\n";
	return 0;
}
