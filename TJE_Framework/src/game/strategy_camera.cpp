#include "strategy_camera.h"

#include <algorithm>
#include <cmath>

StrategyCamera::StrategyCamera(const StrategyCameraSettings& settings)
	: settings(settings)
	, focus(0.0f, 0.0f, 0.0f)
	, pitch(settings.default_pitch_degrees * DEG2RAD)
{
	clampState();
}

void StrategyCamera::setBounds(float min_x, float min_z, float max_x, float max_z)
{
	bounds_min_x = std::min(min_x, max_x);
	bounds_max_x = std::max(min_x, max_x);
	bounds_min_z = std::min(min_z, max_z);
	bounds_max_z = std::max(min_z, max_z);
	clampState();
}

void StrategyCamera::setFocus(float x, float z)
{
	focus.set(x, 0.0f, z);
	clampState();
}

void StrategyCamera::setYaw(float yaw_radians)
{
	yaw = yaw_radians;
	clampState();
}

void StrategyCamera::setPitch(float pitch_radians)
{
	pitch = pitch_radians;
	clampState();
}

void StrategyCamera::setDistance(float new_distance)
{
	distance = new_distance;
	clampState();
}

void StrategyCamera::clampState()
{
	const float two_pi = 2.0f * PI;
	if (std::isfinite(yaw))
		yaw = std::fmod(yaw, two_pi);
	else
		yaw = 0.0f;
	if (!std::isfinite(pitch))
		pitch = settings.default_pitch_degrees * DEG2RAD;
	pitch = std::clamp(pitch, settings.min_pitch_degrees * DEG2RAD, settings.max_pitch_degrees * DEG2RAD);
	if (!std::isfinite(distance))
		distance = settings.max_distance;
	distance = std::clamp(distance, settings.min_distance, settings.max_distance);
	focus.x = std::clamp(focus.x, bounds_min_x, bounds_max_x);
	focus.z = std::clamp(focus.z, bounds_min_z, bounds_max_z);
	focus.y = 0.0f;
}

Vector3 StrategyCamera::getEye() const
{
	const float horizontal = distance * std::cos(pitch);
	return Vector3(focus.x + horizontal * std::sin(yaw), distance * std::sin(pitch),
		focus.z + horizontal * std::cos(yaw));
}

Vector3 StrategyCamera::getGroundForward() const
{
	return Vector3(-std::sin(yaw), 0.0f, -std::cos(yaw));
}

Vector3 StrategyCamera::getGroundRight() const
{
	return Vector3(std::cos(yaw), 0.0f, -std::sin(yaw));
}

void StrategyCamera::update(const CameraInput& input, float dt)
{
	if (!(dt >= 0.0f) || !std::isfinite(dt))
		dt = 0.0f;

	// Pan relative to the view, faster when zoomed out.
	float forward = std::clamp(input.pan_forward, -1.0f, 1.0f);
	float right = std::clamp(input.pan_right, -1.0f, 1.0f);
	const float length = std::sqrt(forward * forward + right * right);
	if (length > 1.0f)
	{
		forward /= length;
		right /= length;
	}
	const float speed = distance * settings.pan_speed_per_distance * dt;
	const Vector3 ground_forward = getGroundForward();
	const Vector3 ground_right = getGroundRight();
	focus.x += (ground_forward.x * forward + ground_right.x * right) * speed;
	focus.z += (ground_forward.z * forward + ground_right.z * right) * speed;

	// Rotation from Q/E and middle-drag.
	yaw += std::clamp(input.rotate, -1.0f, 1.0f) * settings.key_rotate_degrees_per_second * DEG2RAD * dt;
	yaw += input.drag_pixels.x * settings.drag_rotate_degrees_per_pixel * DEG2RAD;
	pitch += input.drag_pixels.y * settings.drag_rotate_degrees_per_pixel * DEG2RAD;

	// Zoom: each notch multiplies the distance by zoom_step (< 1 zooms in).
	if (input.zoom_notches != 0.0f && std::isfinite(input.zoom_notches))
		distance *= std::pow(settings.zoom_step, input.zoom_notches);

	clampState();
}

bool StrategyCamera::isGroundPointVisible(const Vector3& point, float aspect, float margin) const
{
	const Vector3 eye = getEye();
	Vector3 view_forward = focus - eye;
	view_forward.normalize();
	Vector3 view_right = view_forward.cross(Vector3(0.0f, 1.0f, 0.0f));
	view_right.normalize();
	const Vector3 view_up = view_right.cross(view_forward);

	const Vector3 offset = point - eye;
	const float depth = offset.dot(view_forward);
	if (depth <= 0.1f)
		return false;
	const float tan_vertical = std::tan(settings.vertical_fov_degrees * 0.5f * DEG2RAD);
	const float tan_horizontal = tan_vertical * aspect;
	return std::fabs(offset.dot(view_right)) <= depth * tan_horizontal * margin &&
		std::fabs(offset.dot(view_up)) <= depth * tan_vertical * margin;
}

void StrategyCamera::frame(float min_x, float min_z, float max_x, float max_z, float aspect)
{
	if (!(aspect > 0.0f))
		aspect = 1.0f;
	setFocus((min_x + max_x) * 0.5f, (min_z + max_z) * 0.5f);
	const Vector3 corners[4] = { Vector3(min_x, 0.0f, min_z), Vector3(max_x, 0.0f, min_z),
		Vector3(min_x, 0.0f, max_z), Vector3(max_x, 0.0f, max_z) };
	auto allVisible = [&]() {
		for (const Vector3& corner : corners)
			if (!isGroundPointVisible(corner, aspect, settings.frame_margin))
				return false;
		return true;
	};

	// Smallest distance that shows every corner (visibility grows with distance).
	float low = settings.min_distance;
	float high = settings.max_distance;
	distance = high;
	if (!allVisible())
		return; // even the farthest allowed view cannot show it all; keep max distance
	for (int iteration = 0; iteration < 40; ++iteration)
	{
		distance = 0.5f * (low + high);
		if (allVisible())
			high = distance;
		else
			low = distance;
	}
	distance = high;
	clampState();
}

void StrategyCamera::edgeScrollAxes(const Vector2& pointer, float width, float height, float margin,
	float& out_pan_forward, float& out_pan_right)
{
	out_pan_forward = 0.0f;
	out_pan_right = 0.0f;
	if (!(width > 0.0f) || !(height > 0.0f) || pointer.x < 0.0f || pointer.y < 0.0f ||
		pointer.x > width || pointer.y > height)
		return; // pointer outside the window
	if (pointer.x <= margin) out_pan_right = -1.0f;
	else if (pointer.x >= width - margin) out_pan_right = 1.0f;
	if (pointer.y <= margin) out_pan_forward = 1.0f;
	else if (pointer.y >= height - margin) out_pan_forward = -1.0f;
}
