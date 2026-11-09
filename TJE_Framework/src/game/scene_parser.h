#pragma once

#include "asset_catalog.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct StaticSceneInstance
{
	std::string id;
	std::string asset_id;
	std::string material_id;
	Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
	float rotation_y_degrees = 0.0f;
	Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
	bool blocks_navigation = false;
};

struct SceneLoadResult
{
	std::vector<StaticSceneInstance> instances;
	std::optional<DataLoadError> error;
	explicit operator bool() const { return !error.has_value(); }
};

class SceneParser
{
public:
	SceneLoadResult parse(const std::filesystem::path& path, const AssetCatalog& assets) const;
};
