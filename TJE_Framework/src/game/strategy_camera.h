#pragma once

// Orbit-style strategy camera (plan P04, GDD 3.2/3.3), headless.
//
// The camera orbits a focus point on the ground. yaw turns around +Y, pitch is the
// angle below the horizon (about 50 degrees by default) and distance is the orbit
// radius. At yaw 0 the camera stands on the +Z side of the focus and looks toward -Z.
// The class only computes positions; Game copies eye/center into the framework Camera
// (separation of controller and camera, T11). Input arrives already decoded in
// CameraInput, so the logic is testable without SDL or OpenGL.

#include "framework/framework.h"

struct StrategyCameraSettings
{
	float min_distance = 8.0f;
	float max_distance = 110.0f;
	float min_pitch_degrees = 25.0f;
	float max_pitch_degrees = 80.0f;
	float default_pitch_degrees = 50.0f;
	float vertical_fov_degrees = 48.0f;
	float pan_speed_per_distance = 1.0f;          // focus speed (m/s) = distance * this
	float key_rotate_degrees_per_second = 90.0f;  // Q/E
	float drag_rotate_degrees_per_pixel = 0.3f;   // middle-drag
	float zoom_step = 0.88f;                      // distance factor per wheel notch
	float edge_scroll_margin_pixels = 8.0f;
	float frame_margin = 0.9f;                    // fraction of the screen used by frame()
};

struct CameraInput
{
	float pan_forward = 0.0f;   // -1..1, + = away from the camera (W)
	float pan_right = 0.0f;     // -1..1, + = to the right on screen (D)
	float rotate = 0.0f;        // -1..1, + = E (yaw increases)
	Vector2 drag_pixels;        // middle-drag motion this frame, +x right, +y down
	float zoom_notches = 0.0f;  // wheel notches, + = zoom in
};

class StrategyCamera
{
public:
	explicit StrategyCamera(const StrategyCameraSettings& settings = StrategyCameraSettings());

	const StrategyCameraSettings& getSettings() const { return settings; }

	// The focus is clamped to this ground rectangle (the map).
	void setBounds(float min_x, float min_z, float max_x, float max_z);
	void setFocus(float x, float z);
	void setYaw(float yaw_radians);
	void setPitch(float pitch_radians);
	void setDistance(float distance);

	// Centres the focus on the rectangle and picks the smallest distance (within the
	// limits) that keeps its four corners inside the view at the current yaw/pitch.
	void frame(float min_x, float min_z, float max_x, float max_z, float aspect);

	void update(const CameraInput& input, float dt);

	Vector3 getFocus() const { return focus; }
	Vector3 getEye() const;
	float getYaw() const { return yaw; }
	float getPitch() const { return pitch; }
	float getDistance() const { return distance; }
	// Unit ground directions for panning: where the camera looks and its right side.
	Vector3 getGroundForward() const;
	Vector3 getGroundRight() const;

	// True if the ground point (y = 0) projects inside the view, with `margin` < 1
	// shrinking the usable screen area. Used by frame() and the tests.
	bool isGroundPointVisible(const Vector3& point, float aspect, float margin) const;

	// Edge scrolling: pan axes from the pointer position (zero away from the edges).
	static void edgeScrollAxes(const Vector2& pointer, float width, float height, float margin,
		float& out_pan_forward, float& out_pan_right);

private:
	void clampState();

	StrategyCameraSettings settings;
	Vector3 focus;
	float yaw = 0.0f;
	float pitch = 0.0f;
	float distance = 40.0f;
	float bounds_min_x = -1.0e6f;
	float bounds_min_z = -1.0e6f;
	float bounds_max_x = 1.0e6f;
	float bounds_max_z = 1.0e6f;
};
