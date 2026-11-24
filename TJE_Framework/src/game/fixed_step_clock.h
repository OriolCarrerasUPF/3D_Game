#pragma once

// Fixed-timestep accumulator for the colony simulation (plan section C4), headless.
//
// Real frame time is clamped to max_frame_seconds (0.25 s) before it is accumulated,
// and at most max_steps_per_frame (8) fixed steps of step_seconds (1/30 s) run per
// frame. Backlog that still remains after the step budget is dropped so a slow frame
// can never cause a spiral of death. Pausing stops accumulation; camera and UI keep
// using real frame time. Game::time is wall-clock based: simulation code must use
// getSimulationTime() instead.

#include <cmath>

class FixedStepClock
{
public:
	static constexpr double default_step_seconds = 1.0 / 30.0;
	static constexpr double default_max_frame_seconds = 0.25;
	static constexpr int default_max_steps_per_frame = 8;

	explicit FixedStepClock(double step_seconds = default_step_seconds,
		double max_frame_seconds = default_max_frame_seconds,
		int max_steps_per_frame = default_max_steps_per_frame)
		: step_seconds(step_seconds > 0.0 ? step_seconds : default_step_seconds)
		, max_frame_seconds(max_frame_seconds > 0.0 ? max_frame_seconds : default_max_frame_seconds)
		, max_steps_per_frame(max_steps_per_frame > 0 ? max_steps_per_frame : default_max_steps_per_frame)
	{
	}

	// Adds one real frame and returns how many fixed steps must run now.
	int advance(double frame_seconds)
	{
		if (paused)
			return 0;
		if (!std::isfinite(frame_seconds) || frame_seconds < 0.0)
			frame_seconds = 0.0;
		if (frame_seconds > max_frame_seconds)
			frame_seconds = max_frame_seconds;

		accumulator += frame_seconds;
		int steps = 0;
		while (accumulator >= step_seconds && steps < max_steps_per_frame)
		{
			accumulator -= step_seconds;
			++steps;
			++total_steps;
		}
		if (accumulator >= step_seconds)
		{
			dropped_seconds += accumulator - std::fmod(accumulator, step_seconds);
			accumulator = std::fmod(accumulator, step_seconds);
		}
		return steps;
	}

	void setPaused(bool must_pause) { paused = must_pause; }
	bool isPaused() const { return paused; }

	// Restart: clears accumulated time and counters, keeps the configuration.
	void reset()
	{
		accumulator = 0.0;
		total_steps = 0;
		dropped_seconds = 0.0;
		paused = false;
	}

	double getStepSeconds() const { return step_seconds; }
	double getMaxFrameSeconds() const { return max_frame_seconds; }
	int getMaxStepsPerFrame() const { return max_steps_per_frame; }
	double getSimulationTime() const { return static_cast<double>(total_steps) * step_seconds; }
	long long getTotalSteps() const { return total_steps; }
	double getDroppedSeconds() const { return dropped_seconds; }
	// Fraction of a step accumulated but not simulated yet, for render interpolation.
	double getInterpolationAlpha() const { return accumulator / step_seconds; }

private:
	double step_seconds;
	double max_frame_seconds;
	int max_steps_per_frame;
	double accumulator = 0.0;
	long long total_steps = 0;
	double dropped_seconds = 0.0;
	bool paused = false;
};
