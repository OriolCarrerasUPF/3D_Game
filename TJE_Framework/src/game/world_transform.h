#pragma once

// One place for every model-matrix convention used by Arkfall views (headless).
//
// TJE's Matrix44 uses row-vector order: (A * B) applies A first, then B, and
// Matrix44::translate/rotate/scale PRE-multiply, so the call order
// translate -> rotate -> scale produces "scale, then rotate, then translate".
// Entity::getGlobalMatrix() follows the same rule (model * parent).
//
// Yaw convention (radians, about +Y): a model whose forward is +Z, rotated by yaw,
// faces (-sin(yaw), 0, cos(yaw)). yawFromDirection() is the inverse of that.

#include "framework/framework.h"

#include <string>

namespace world_transform
{
	// Instance placement: scale, then rotate about +Y, then translate.
	Matrix44 makeInstanceTransform(const Vector3& position, float rotation_y_radians, const Vector3& scale);

	// Asset normalization: scale the source mesh, then offset it by the pivot (metres).
	Matrix44 makeAssetTransform(const Vector3& asset_scale, const Vector3& pivot_offset);

	// Final model matrix: asset normalization first, then the instance placement.
	Matrix44 composeAssetThenInstance(const Matrix44& asset_transform, const Matrix44& instance_transform);

	// Extra yaw that turns an asset's authored forward axis (+X, -X, +Z, -Z) into +Z.
	// Returns false for an unknown axis string.
	bool forwardAxisYaw(const std::string& forward_axis, float& out_yaw_radians);

	// Yaw that makes a +Z-forward model face the XZ direction (dx, dz).
	float yawFromDirection(float dx, float dz);

	// Direction a +Z-forward model faces after rotating by yaw.
	Vector3 directionFromYaw(float yaw_radians);
}
