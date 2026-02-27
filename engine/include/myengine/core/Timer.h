// Timer.h

#pragma once

namespace myengine::core
{
	class Timer
	{
	public:
		Timer();

		void Reset();
		void Start();
		void Stop();
		void Tick();

		float DeltaTime() const;
		float TotalTime() const;

	private:
		double secondsPerCount_;
		double deltaTime_;

		long long baseTime_;
		long long pausedTime_;
		long long stopTime_;
		long long prevTime_;
		long long currTime_;

		bool stopped_;
	};
}