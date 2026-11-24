#pragma once

#include "data_load_error.h"
#include "framework/framework.h"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class Mesh;
class Shader;

struct AssetDefinition
{
	std::string id;
	std::filesystem::path model_path;
	std::filesystem::path material_path;
	std::string material_id;
	Vector3 default_scale = Vector3(1.0f, 1.0f, 1.0f);
	Vector3 pivot_offset = Vector3(0.0f, 0.0f, 0.0f);
	std::string forward_axis;
	Vector3 source_bounds_min = Vector3(0.0f, 0.0f, 0.0f);
	Vector3 source_bounds_max = Vector3(0.0f, 0.0f, 0.0f);
	// Bounds transformed once by default_scale and pivot_offset during catalog load.
	Vector3 picking_bounds_min = Vector3(0.0f, 0.0f, 0.0f);
	Vector3 picking_bounds_max = Vector3(0.0f, 0.0f, 0.0f);
	// Visual-only: instances of this asset define the height colonists are drawn at
	// (terrain, interior floor modules). Never used as gameplay collision.
	bool walkable_surface = false;
	bool load_failure_is_fatal = true;
	Mesh* mesh = nullptr; // borrowed from the framework's Mesh cache
};

struct SemanticMaterial
{
	std::string id;
	Vector4 tint = Vector4(1.0f);
};

struct AssetCatalogLoadResult
{
	std::optional<DataLoadError> error;
	explicit operator bool() const { return !error.has_value(); }
};

// load() is headless (JSON + filesystem only, asset_catalog.cpp).
// loadReferencedAssets() needs an OpenGL context (asset_catalog_resources.cpp); it is
// additive and may be called again later for dynamic-view assets.
class AssetCatalog
{
public:
	AssetCatalogLoadResult load(const std::filesystem::path& assets_file,
		const std::filesystem::path& materials_file);
	AssetCatalogLoadResult loadReferencedAssets(const std::set<std::string>& asset_ids);

	const AssetDefinition* findAsset(const std::string& id) const;
	const SemanticMaterial* findMaterial(const std::string& id) const;
	const std::map<std::string, AssetDefinition>& getAssets() const { return assets; }
	Shader* getStandardShader() const { return standard_shader; }
	Shader* getInstancedShader() const { return instanced_shader; }

private:
	std::filesystem::path assets_path;
	std::filesystem::path data_root;
	std::filesystem::path arkfall_root;
	std::filesystem::path standard_vertex_path;
	std::filesystem::path instanced_vertex_path;
	std::filesystem::path fragment_path;
	std::map<std::string, AssetDefinition> assets;
	std::map<std::string, SemanticMaterial> materials;
	Shader* standard_shader = nullptr; // borrowed from the framework's Shader cache
	Shader* instanced_shader = nullptr; // borrowed from the framework's Shader cache
};
