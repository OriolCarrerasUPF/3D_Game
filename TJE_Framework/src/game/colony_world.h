#pragma once

#include "asset_catalog.h"
#include "scene_parser.h"
#include "framework/entities/entity.h"

#include <string>
#include <vector>

class Camera;
class Mesh;
class Shader;

class EntityMesh : public Entity
{
public:
	EntityMesh(Mesh* mesh, const Material& material, Shader* instanced_shader,
		const Matrix44& asset_transform, const std::string& asset_id);
	void setInstances(const std::vector<Matrix44>& models, const std::vector<bool>& blocks_navigation);
	void render(Camera* camera) override;

	bool isInstanced() const { return instance_models.size() > 1; }
	std::size_t getInstanceCount() const { return instance_models.size(); }
	const std::vector<bool>& getNavigationBlockers() const { return instance_blocks_navigation; }
	const std::string& getAssetId() const { return asset_id; }

private:
	Mesh* mesh = nullptr; // borrowed from AssetCatalog
	Material material;
	Shader* instanced_shader = nullptr;
	Matrix44 asset_transform;
	std::string asset_id;
	std::vector<Matrix44> instance_models;
	std::vector<bool> instance_blocks_navigation;
};

class ColonyWorld
{
public:
	explicit ColonyWorld(const AssetCatalog& assets);
	~ColonyWorld();
	ColonyWorld(const ColonyWorld&) = delete;
	ColonyWorld& operator=(const ColonyWorld&) = delete;

	bool buildStaticViews(const SceneLoadResult& scene, DataLoadError& error);
	void render(Camera* camera);
	std::size_t getStaticViewCount() const { return static_views.size(); }
	std::size_t getInstanceCount() const { return instance_count; }
	std::size_t getInstanceCountForAsset(const std::string& asset_id) const;
	std::size_t getInstancedViewCount() const;

private:
	const AssetCatalog* assets;
	std::vector<EntityMesh*> static_views;
	std::size_t instance_count = 0;
};
