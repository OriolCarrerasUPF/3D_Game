#pragma once

// Visual-only ground height lookup (headless).
//
// Gameplay stays planar at y = 0. Walkable visual surfaces (terrain tiles, the
// 0.1 m interior floor modules, future platforms) register their world XZ extent
// and top height here, so dynamic views such as colonists are drawn standing on
// the floor instead of sinking into it. Heights are sampled per square bucket of
// bucket_size metres (1 m, matching the navigation grid), so a colonist steps up
// exactly when it crosses a bucket edge (e.g. the Exit Door edge).

#include <cstdint>
#include <unordered_map>

class SurfaceHeightMap
{
public:
	explicit SurfaceHeightMap(float bucket_size = 1.0f, float default_height = 0.0f);

	void clear();

	// Registers a surface covering [min_x, max_x] x [min_z, max_z] with its top at top_y.
	// Every bucket whose centre lies inside the rectangle takes the highest top seen.
	void addSurface(float min_x, float min_z, float max_x, float max_z, float top_y);

	// Highest registered surface at the bucket containing (x, z), or default_height.
	float heightAt(float x, float z) const;

	std::size_t getBucketCount() const { return heights.size(); }

private:
	static std::int64_t makeKey(int bucket_x, int bucket_z);
	int toBucket(float coordinate) const;

	float bucket_size;
	float default_height;
	std::unordered_map<std::int64_t, float> heights;
};
