#include "surface_height_map.h"

#include <cmath>

SurfaceHeightMap::SurfaceHeightMap(float bucket_size, float default_height)
	: bucket_size(bucket_size > 0.0f ? bucket_size : 1.0f)
	, default_height(default_height)
{
}

void SurfaceHeightMap::clear()
{
	heights.clear();
}

std::int64_t SurfaceHeightMap::makeKey(int bucket_x, int bucket_z)
{
	return (static_cast<std::int64_t>(bucket_x) << 32) ^ static_cast<std::uint32_t>(bucket_z);
}

int SurfaceHeightMap::toBucket(float coordinate) const
{
	return static_cast<int>(std::floor(coordinate / bucket_size));
}

void SurfaceHeightMap::addSurface(float min_x, float min_z, float max_x, float max_z, float top_y)
{
	if (!(min_x <= max_x) || !(min_z <= max_z) || !std::isfinite(top_y))
		return;

	// A bucket belongs to the surface when its centre lies strictly inside the
	// rectangle; centres exactly on an edge are ignored so a surface that is not
	// aligned with the buckets never claims a neighbour it barely touches.
	const float epsilon = 0.0001f;
	const int first_x = static_cast<int>(std::ceil((min_x + epsilon) / bucket_size - 0.5f));
	const int last_x = static_cast<int>(std::floor((max_x - epsilon) / bucket_size - 0.5f));
	const int first_z = static_cast<int>(std::ceil((min_z + epsilon) / bucket_size - 0.5f));
	const int last_z = static_cast<int>(std::floor((max_z - epsilon) / bucket_size - 0.5f));
	for (int bx = first_x; bx <= last_x; ++bx)
	{
		for (int bz = first_z; bz <= last_z; ++bz)
		{
			const std::int64_t key = makeKey(bx, bz);
			auto it = heights.find(key);
			if (it == heights.end())
				heights.emplace(key, top_y);
			else if (top_y > it->second)
				it->second = top_y;
		}
	}
}

float SurfaceHeightMap::heightAt(float x, float z) const
{
	const auto it = heights.find(makeKey(toBucket(x), toBucket(z)));
	return it == heights.end() ? default_height : it->second;
}
