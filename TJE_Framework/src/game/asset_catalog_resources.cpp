// GPU half of AssetCatalog: compiles the Arkfall shaders and loads the OBJ/MTL
// meshes of the requested assets through the framework caches. Requires a live
// OpenGL context; the JSON validation in asset_catalog.cpp does not.

#include "asset_catalog.h"

#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <cmath>

AssetCatalogLoadResult AssetCatalog::loadReferencedAssets(const std::set<std::string>& asset_ids)
{
	AssetCatalogLoadResult result;
	for (const std::string& id : asset_ids)
	{
		if (!assets.count(id))
		{
			result.error = DataLoadError{ assets_path, "/assets", "unknown asset id '" + id + "'" };
			return result;
		}
	}

	standard_shader = Shader::Get(standard_vertex_path.string().c_str(), fragment_path.string().c_str());
	if (!standard_shader || !standard_shader->compiled)
	{
		result.error = DataLoadError{ fragment_path, "", "could not compile the standard Arkfall shader" };
		return result;
	}
	instanced_shader = Shader::Get(instanced_vertex_path.string().c_str(), fragment_path.string().c_str());
	if (!instanced_shader || !instanced_shader->compiled)
	{
		result.error = DataLoadError{ fragment_path, "", "could not compile the instanced Arkfall shader" };
		return result;
	}

	// Validate the selected source OBJ/MTL files, not stale .mbin caches.
	const bool previous_use_binary = Mesh::use_binary;
	Mesh::use_binary = false;
	for (const std::string& id : asset_ids)
	{
		AssetDefinition& asset = assets.at(id);
		if (asset.mesh)
			continue; // already loaded by an earlier call (scene first, dynamic views later)

		asset.mesh = Mesh::Get(asset.model_path.string().c_str());
		if (!asset.mesh)
		{
			if (asset.load_failure_is_fatal)
			{
				Mesh::use_binary = previous_use_binary;
				result.error = DataLoadError{ asset.model_path, "/assets/" + id + "/modelPath", "OBJ/MTL loader rejected this asset" };
				return result;
			}
			continue;
		}

		const Vector3& actual_min = asset.mesh->aabb_min;
		const Vector3& actual_max = asset.mesh->aabb_max;
		const float tolerance = 0.0001f;
		const bool bounds_match =
			std::fabs(actual_min.x - asset.source_bounds_min.x) <= tolerance &&
			std::fabs(actual_min.y - asset.source_bounds_min.y) <= tolerance &&
			std::fabs(actual_min.z - asset.source_bounds_min.z) <= tolerance &&
			std::fabs(actual_max.x - asset.source_bounds_max.x) <= tolerance &&
			std::fabs(actual_max.y - asset.source_bounds_max.y) <= tolerance &&
			std::fabs(actual_max.z - asset.source_bounds_max.z) <= tolerance;
		if (!bounds_match)
		{
			asset.mesh = nullptr;
			if (asset.load_failure_is_fatal)
			{
				Mesh::use_binary = previous_use_binary;
				result.error = DataLoadError{ asset.model_path, "/assets/" + id + "/pickingBounds",
					"catalog bounds do not match the loaded OBJ bounds" };
				return result;
			}
		}
	}
	Mesh::use_binary = previous_use_binary;
	return result;
}
