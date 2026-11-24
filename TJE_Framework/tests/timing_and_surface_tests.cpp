#include "test_harness.h"

#include "game/fixed_step_clock.h"
#include "game/surface_height_map.h"

#include <limits>

TEST_CASE("fixed_step_clock: defaults follow plan C4")
{
	FixedStepClock clock;
	CHECK_NEAR(clock.getStepSeconds(), 1.0 / 30.0, 1e-12);
	CHECK_NEAR(clock.getMaxFrameSeconds(), 0.25, 1e-12);
	CHECK_EQ(clock.getMaxStepsPerFrame(), 8);
}

TEST_CASE("fixed_step_clock: accumulates partial frames")
{
	FixedStepClock clock(0.1, 0.25, 8);
	CHECK_EQ(clock.advance(0.05), 0);
	CHECK_EQ(clock.advance(0.06), 1);
	CHECK_NEAR(clock.getInterpolationAlpha(), 0.1, 1e-9);
	CHECK_EQ(clock.advance(0.095), 1);
	CHECK_NEAR(clock.getSimulationTime(), 0.2, 1e-9);
}

TEST_CASE("fixed_step_clock: frame time is clamped to 0.25 s")
{
	FixedStepClock clock;
	// A 5 s hitch only simulates 0.25 s: 7 steps of 1/30 s, remainder kept.
	CHECK_EQ(clock.advance(5.0), 7);
	CHECK_NEAR(clock.getSimulationTime(), 7.0 / 30.0, 1e-9);
}

TEST_CASE("fixed_step_clock: never more than the step budget per frame")
{
	FixedStepClock clock(0.01, 0.25, 8);
	CHECK_EQ(clock.advance(0.25), 8);       // 25 steps due, 8 allowed
	CHECK(clock.getDroppedSeconds() > 0.1); // backlog dropped, no spiral of death
	CHECK(clock.getInterpolationAlpha() < 1.0);
	CHECK_EQ(clock.advance(0.0), 0);
}

TEST_CASE("fixed_step_clock: pause, invalid input and reset")
{
	FixedStepClock clock;
	clock.setPaused(true);
	CHECK_EQ(clock.advance(0.2), 0);
	CHECK_NEAR(clock.getSimulationTime(), 0.0, 1e-12);
	clock.setPaused(false);
	CHECK_EQ(clock.advance(-1.0), 0);
	CHECK_EQ(clock.advance(std::numeric_limits<double>::quiet_NaN()), 0);
	CHECK_EQ(clock.advance(0.11), 3);
	clock.reset();
	CHECK_EQ(clock.getTotalSteps(), 0);
	CHECK_NEAR(clock.getInterpolationAlpha(), 0.0, 1e-12);
}

TEST_CASE("surface_height_map: tiles claim only their own bucket")
{
	SurfaceHeightMap heights;
	// Terrain tile of cell (31,31): world [-1, 0] x [-1, 0] with the -32 grid origin.
	heights.addSurface(-1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
	CHECK_EQ(heights.getBucketCount(), static_cast<std::size_t>(1));
	CHECK_NEAR(heights.heightAt(-0.5f, -0.5f), 0.0, 1e-6);
	// A 2x2 floor module on cell edges [-8, -6] x [-1, 1] covers four buckets.
	heights.addSurface(-8.0f, -1.0f, -6.0f, 1.0f, 0.1f);
	CHECK_EQ(heights.getBucketCount(), static_cast<std::size_t>(5));
	CHECK_NEAR(heights.heightAt(-7.9f, -0.5f), 0.1, 1e-6);
	CHECK_NEAR(heights.heightAt(-6.1f, 0.9f), 0.1, 1e-6);
	CHECK_NEAR(heights.heightAt(-8.1f, -0.5f), 0.0, 1e-6); // outside the base: default height
	CHECK_NEAR(heights.heightAt(-5.9f, 0.0f), 0.0, 1e-6);
	// A surface whose edges fall on bucket centres claims none of those buckets.
	SurfaceHeightMap unaligned;
	unaligned.addSurface(-0.5f, -0.5f, 0.5f, 0.5f, 1.0f);
	CHECK_EQ(unaligned.getBucketCount(), static_cast<std::size_t>(0));
}

TEST_CASE("surface_height_map: highest surface wins and clear resets")
{
	SurfaceHeightMap heights;
	heights.addSurface(0.0f, 0.0f, 1.0f, 1.0f, 0.0f);
	heights.addSurface(0.0f, 0.0f, 1.0f, 1.0f, 0.1f);
	heights.addSurface(0.0f, 0.0f, 1.0f, 1.0f, 0.05f);
	CHECK_NEAR(heights.heightAt(0.5f, 0.5f), 0.1, 1e-6);
	heights.clear();
	CHECK_NEAR(heights.heightAt(0.5f, 0.5f), 0.0, 1e-6);
	heights.addSurface(1.0f, 1.0f, 0.0f, 0.0f, 1.0f); // inverted rectangle is ignored
	CHECK_EQ(heights.getBucketCount(), static_cast<std::size_t>(0));
}
