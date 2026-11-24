// P04 headless checks: strategy camera math and ray picking.

#include "test_harness.h"

#include "game/picking.h"
#include "game/strategy_camera.h"

#include <cmath>
#include <limits>

namespace
{
	float horizontalDistance(const Vector3& a, const Vector3& b)
	{
		return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
	}
}

TEST_CASE("strategy camera: default view looks down at about 50 degrees from +Z")
{
	StrategyCamera camera;
	camera.setFocus(0.0f, 0.0f);
	camera.setDistance(40.0f);
	const Vector3 eye = camera.getEye();
	CHECK(eye.z > 0.0f);
	CHECK_NEAR(eye.x, 0.0, 1e-4);
	const float pitch_degrees = std::atan2(eye.y, horizontalDistance(eye, camera.getFocus())) * RAD2DEG;
	CHECK_NEAR(pitch_degrees, 50.0, 0.01);
	CHECK_NEAR((eye - camera.getFocus()).length(), 40.0, 1e-3);
}

TEST_CASE("strategy camera: WASD pans relative to the view and faster when zoomed out")
{
	StrategyCamera camera;
	camera.setDistance(20.0f);
	CameraInput forward;
	forward.pan_forward = 1.0f;
	camera.update(forward, 0.5f);
	CHECK(camera.getFocus().z < 0.0f);               // yaw 0 looks toward -Z
	CHECK_NEAR(camera.getFocus().x, 0.0, 1e-4);
	const float near_step = -camera.getFocus().z;

	camera.setFocus(0.0f, 0.0f);
	camera.setDistance(40.0f);
	camera.update(forward, 0.5f);
	CHECK_NEAR(-camera.getFocus().z, 2.0 * near_step, 1e-3);

	camera.setFocus(0.0f, 0.0f);
	camera.setYaw(0.5f * PI);                          // camera on +X, looking toward -X
	camera.update(forward, 0.5f);
	CHECK(camera.getFocus().x < 0.0f);
	CameraInput right;
	right.pan_right = 1.0f;
	camera.setFocus(0.0f, 0.0f);
	camera.setYaw(0.0f);
	camera.update(right, 0.5f);
	CHECK(camera.getFocus().x > 0.0f);                 // D moves to screen right (+X at yaw 0)
	// Diagonal input is normalised: not faster than a single axis.
	camera.setFocus(0.0f, 0.0f);
	CameraInput diagonal;
	diagonal.pan_forward = 1.0f;
	diagonal.pan_right = 1.0f;
	camera.update(diagonal, 0.5f);
	CHECK_NEAR(horizontalDistance(camera.getFocus(), Vector3(0.0f, 0.0f, 0.0f)), 2.0 * near_step, 1e-3);
}

TEST_CASE("strategy camera: rotation, zoom and pitch stay within limits")
{
	StrategyCamera camera;
	CameraInput rotate;
	rotate.rotate = 1.0f;
	camera.update(rotate, 1.0f);
	CHECK_NEAR(camera.getYaw(), 0.5 * PI, 1e-4);       // 90 degrees per second

	camera.setDistance(40.0f);
	CameraInput zoom_in;
	zoom_in.zoom_notches = 1.0f;
	camera.update(zoom_in, 0.0f);
	CHECK_NEAR(camera.getDistance(), 40.0 * camera.getSettings().zoom_step, 1e-3);
	zoom_in.zoom_notches = 100.0f;
	camera.update(zoom_in, 0.0f);
	CHECK_NEAR(camera.getDistance(), camera.getSettings().min_distance, 1e-4);
	CameraInput zoom_out;
	zoom_out.zoom_notches = -100.0f;
	camera.update(zoom_out, 0.0f);
	CHECK_NEAR(camera.getDistance(), camera.getSettings().max_distance, 1e-3);

	CameraInput drag;
	drag.drag_pixels.set(0.0f, 10000.0f);
	camera.update(drag, 0.0f);
	CHECK_NEAR(camera.getPitch() * RAD2DEG, camera.getSettings().max_pitch_degrees, 1e-3);
	drag.drag_pixels.set(0.0f, -10000.0f);
	camera.update(drag, 0.0f);
	CHECK_NEAR(camera.getPitch() * RAD2DEG, camera.getSettings().min_pitch_degrees, 1e-3);

	// Garbage input never produces a broken state.
	CameraInput broken;
	broken.zoom_notches = std::numeric_limits<float>::quiet_NaN();
	camera.update(broken, std::numeric_limits<float>::quiet_NaN());
	CHECK(std::isfinite(camera.getDistance()));
	CHECK(std::isfinite(camera.getFocus().x));
}

TEST_CASE("strategy camera: focus is clamped to the map bounds")
{
	StrategyCamera camera;
	camera.setBounds(-32.0f, -32.0f, 32.0f, 32.0f);
	camera.setFocus(100.0f, -100.0f);
	CHECK_NEAR(camera.getFocus().x, 32.0, 1e-4);
	CHECK_NEAR(camera.getFocus().z, -32.0, 1e-4);
	CameraInput forward;
	forward.pan_forward = 1.0f;
	camera.update(forward, 100.0f);
	CHECK(camera.getFocus().z >= -32.0f);
}

TEST_CASE("strategy camera: frame() fits the base and both deposits")
{
	// Shipped layout: base x/z in [-8, 8], deposits around (-23.5, -19.5) and (23.5, -19.5).
	const float min_x = -26.0f, min_z = -22.0f, max_x = 26.0f, max_z = 10.0f;
	for (const float aspect : { 4.0f / 3.0f, 16.0f / 9.0f })
	{
		StrategyCamera camera;
		camera.frame(min_x, min_z, max_x, max_z, aspect);
		const float margin = camera.getSettings().frame_margin;
		CHECK(camera.getDistance() > camera.getSettings().min_distance);
		CHECK(camera.getDistance() < camera.getSettings().max_distance);
		for (const Vector3& corner : { Vector3(min_x, 0, min_z), Vector3(max_x, 0, min_z), Vector3(min_x, 0, max_z), Vector3(max_x, 0, max_z) })
			CHECK(camera.isGroundPointVisible(corner, aspect, margin));
		// Tight fit: slightly closer and a corner leaves the view.
		const float fitted = camera.getDistance();
		camera.setDistance(fitted * 0.9f);
		bool all_visible = true;
		for (const Vector3& corner : { Vector3(min_x, 0, min_z), Vector3(max_x, 0, min_z), Vector3(min_x, 0, max_z), Vector3(max_x, 0, max_z) })
			all_visible = all_visible && camera.isGroundPointVisible(corner, aspect, margin);
		CHECK(!all_visible);
	}
}

TEST_CASE("strategy camera: edge scrolling axes")
{
	float forward = 0.0f, right = 0.0f;
	StrategyCamera::edgeScrollAxes(Vector2(2.0f, 300.0f), 800.0f, 600.0f, 8.0f, forward, right);
	CHECK_NEAR(right, -1.0, 1e-6);
	CHECK_NEAR(forward, 0.0, 1e-6);
	StrategyCamera::edgeScrollAxes(Vector2(400.0f, 1.0f), 800.0f, 600.0f, 8.0f, forward, right);
	CHECK_NEAR(forward, 1.0, 1e-6);
	StrategyCamera::edgeScrollAxes(Vector2(799.0f, 599.0f), 800.0f, 600.0f, 8.0f, forward, right);
	CHECK_NEAR(forward, -1.0, 1e-6);
	CHECK_NEAR(right, 1.0, 1e-6);
	StrategyCamera::edgeScrollAxes(Vector2(400.0f, 300.0f), 800.0f, 600.0f, 8.0f, forward, right);
	CHECK_NEAR(forward, 0.0, 1e-6);
	CHECK_NEAR(right, 0.0, 1e-6);
	StrategyCamera::edgeScrollAxes(Vector2(-5.0f, 300.0f), 800.0f, 600.0f, 8.0f, forward, right);
	CHECK_NEAR(right, 0.0, 1e-6); // pointer outside the window
}

TEST_CASE("picking: ray against axis-aligned boxes")
{
	const picking::Ray down{ Vector3(0.0f, 10.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f) };
	float t = -1.0f;
	CHECK(picking::intersectAabb(down, Vector3(-1, 0, -1), Vector3(1, 2, 1), t));
	CHECK_NEAR(t, 8.0, 1e-5);
	CHECK(!picking::intersectAabb(picking::Ray{ Vector3(3.0f, 10.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f) },
		Vector3(-1, 0, -1), Vector3(1, 2, 1), t));
	// Box behind the origin.
	CHECK(!picking::intersectAabb(picking::Ray{ Vector3(0.0f, -5.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f) },
		Vector3(-1, 0, -1), Vector3(1, 2, 1), t));
	// Origin inside the box hits at t = 0.
	CHECK(picking::intersectAabb(picking::Ray{ Vector3(0.0f, 1.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f) },
		Vector3(-1, 0, -1), Vector3(1, 2, 1), t));
	CHECK_NEAR(t, 0.0, 1e-6);
	// Parallel to a slab and outside it.
	CHECK(!picking::intersectAabb(picking::Ray{ Vector3(0.0f, 5.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f) },
		Vector3(-1, 0, -1), Vector3(1, 2, 1), t));
}

TEST_CASE("picking: ground plane and rotated boxes")
{
	Vector3 diagonal(1.0f, -1.0f, 0.0f);
	diagonal.normalize();
	Vector3 point;
	float t = 0.0f;
	CHECK(picking::intersectHorizontalPlane(picking::Ray{ Vector3(0.0f, 10.0f, 0.0f), diagonal }, 0.0f, point, t));
	CHECK_NEAR(point.x, 10.0, 1e-4);
	CHECK_NEAR(point.y, 0.0, 1e-6);
	CHECK(picking::intersectHorizontalPlane(picking::Ray{ Vector3(0.0f, 10.0f, 0.0f), diagonal }, 0.1f, point, t));
	CHECK_NEAR(point.x, 9.9, 1e-4);
	CHECK(!picking::intersectHorizontalPlane(picking::Ray{ Vector3(0.0f, 10.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f) }, 0.0f, point, t));

	// A thin box 2 m long on local X. At yaw 90 degrees it lies along world Z
	// (world_transform convention: +X turns into +Z).
	const Vector3 box_min(-1.0f, 0.0f, -0.1f), box_max(1.0f, 2.0f, 0.1f);
	const Vector3 position(5.0f, 0.0f, 5.0f);
	const picking::Ray along_z{ Vector3(5.0f, 10.0f, 5.8f), Vector3(0.0f, -1.0f, 0.0f) };
	const picking::Ray along_x{ Vector3(5.8f, 10.0f, 5.0f), Vector3(0.0f, -1.0f, 0.0f) };
	CHECK(picking::intersectPlacedBox(along_z, box_min, box_max, position, 0.5f * PI, t));
	CHECK_NEAR(t, 8.0, 1e-4);
	CHECK(!picking::intersectPlacedBox(along_x, box_min, box_max, position, 0.5f * PI, t));
	CHECK(picking::intersectPlacedBox(along_x, box_min, box_max, position, 0.0f, t));
	CHECK(!picking::intersectPlacedBox(along_z, box_min, box_max, position, 0.0f, t));
}
