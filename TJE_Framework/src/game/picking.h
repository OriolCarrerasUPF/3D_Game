#pragma once

// Ray picking helpers (plan P04, T12), headless.
//
// Selection is visual picking only: grid occupancy stays the authority for movement
// and construction. Rays come from Camera::getRayDirection (world space, normalized).
// Placed boxes use the world_transform yaw convention and uniform scale 1, which is
// how dynamic views (colonists) are placed; their box is the asset's picking bounds
// already scaled by the catalog.

#include "framework/framework.h"

namespace picking
{
	struct Ray
	{
		Vector3 origin;
		Vector3 direction; // normalized
	};

	// Slab test. Returns the nearest hit distance t >= 0 along the ray; a ray that starts
	// inside the box hits at t = 0. False for misses and boxes behind the origin.
	bool intersectAabb(const Ray& ray, const Vector3& box_min, const Vector3& box_max, float& out_t);

	// Intersection with the horizontal plane y = plane_y, in front of the origin.
	bool intersectHorizontalPlane(const Ray& ray, float plane_y, Vector3& out_point, float& out_t);

	// Box given in local space, placed at `position` and rotated by `yaw_radians` about +Y.
	bool intersectPlacedBox(const Ray& ray, const Vector3& local_min, const Vector3& local_max,
		const Vector3& position, float yaw_radians, float& out_t);
}
