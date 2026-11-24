#include "world_transform.h"

#include <cmath>

namespace world_transform
{
	Matrix44 makeInstanceTransform(const Vector3& position, float rotation_y_radians, const Vector3& scale)
	{
		Matrix44 transform;
		transform.translate(position);
		transform.rotate(rotation_y_radians, Vector3(0.0f, 1.0f, 0.0f));
		transform.scale(scale);
		return transform;
	}

	Matrix44 makeAssetTransform(const Vector3& asset_scale, const Vector3& pivot_offset)
	{
		Matrix44 transform;
		transform.translate(pivot_offset);
		transform.scale(asset_scale);
		return transform;
	}

	Matrix44 composeAssetThenInstance(const Matrix44& asset_transform, const Matrix44& instance_transform)
	{
		return asset_transform * instance_transform;
	}

	bool forwardAxisYaw(const std::string& forward_axis, float& out_yaw_radians)
	{
		if (forward_axis == "+Z") { out_yaw_radians = 0.0f; return true; }
		if (forward_axis == "+X") { out_yaw_radians = 0.5f * PI; return true; }
		if (forward_axis == "-X") { out_yaw_radians = -0.5f * PI; return true; }
		if (forward_axis == "-Z") { out_yaw_radians = PI; return true; }
		return false;
	}

	float yawFromDirection(float dx, float dz)
	{
		return std::atan2(-dx, dz);
	}

	Vector3 directionFromYaw(float yaw_radians)
	{
		return Vector3(-std::sin(yaw_radians), 0.0f, std::cos(yaw_radians));
	}
}
