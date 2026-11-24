#include "picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace picking
{
	bool intersectAabb(const Ray& ray, const Vector3& box_min, const Vector3& box_max, float& out_t)
	{
		float t_near = -std::numeric_limits<float>::infinity();
		float t_far = std::numeric_limits<float>::infinity();
		const float origin[3] = { ray.origin.x, ray.origin.y, ray.origin.z };
		const float direction[3] = { ray.direction.x, ray.direction.y, ray.direction.z };
		const float low[3] = { box_min.x, box_min.y, box_min.z };
		const float high[3] = { box_max.x, box_max.y, box_max.z };
		for (int axis = 0; axis < 3; ++axis)
		{
			if (std::fabs(direction[axis]) < 1e-8f)
			{
				// Parallel to this slab: must already be between its planes.
				if (origin[axis] < low[axis] || origin[axis] > high[axis])
					return false;
				continue;
			}
			float t0 = (low[axis] - origin[axis]) / direction[axis];
			float t1 = (high[axis] - origin[axis]) / direction[axis];
			if (t0 > t1)
				std::swap(t0, t1);
			t_near = std::max(t_near, t0);
			t_far = std::min(t_far, t1);
			if (t_near > t_far)
				return false;
		}
		if (t_far < 0.0f)
			return false; // box entirely behind the origin
		out_t = std::max(t_near, 0.0f);
		return true;
	}

	bool intersectHorizontalPlane(const Ray& ray, float plane_y, Vector3& out_point, float& out_t)
	{
		if (std::fabs(ray.direction.y) < 1e-8f)
			return false;
		const float t = (plane_y - ray.origin.y) / ray.direction.y;
		if (t < 0.0f)
			return false;
		out_t = t;
		out_point = ray.origin + ray.direction * t;
		out_point.y = plane_y;
		return true;
	}

	bool intersectPlacedBox(const Ray& ray, const Vector3& local_min, const Vector3& local_max,
		const Vector3& position, float yaw_radians, float& out_t)
	{
		// Move the ray into the box's local frame: undo the translation, then rotate by
		// -yaw (world_transform rotation: (x, z) -> (x c - z s, x s + z c)).
		const float c = std::cos(-yaw_radians);
		const float s = std::sin(-yaw_radians);
		auto toLocal = [c, s](const Vector3& v) {
			return Vector3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
		};
		Ray local;
		local.origin = toLocal(ray.origin - position);
		local.direction = toLocal(ray.direction);
		return intersectAabb(local, local_min, local_max, out_t);
	}
}
