#pragma once

#include "asset_catalog.h"
#include "scene_parser.h"
#include "surface_height_map.h"
#include "framework/entities/entity.h"
#include "graphics/material.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Camera;
class Mesh;
class Shader;

// Renderable entity for one asset/material pair. It draws any number of instances:
// one instance uses the standard shader (u_model uniform), several use hardware
// instancing (u_model attribute) in a single call per material run. Instances can be
// added, moved and removed at any time, so the same class serves the static scene
// groups and per-frame dynamic views (colonists, later buildings/blueprints).
class EntityMesh : public Entity
{
public:
	EntityMesh(Mesh* mesh, const Material& material, Shader* instanced_shader,
		const Matrix44& asset_transform, const std::string& asset_id);

	// Instance matrices are placement-only; the asset normalization is applied here.
	void setInstances(const std::vector<Matrix44>& instance_transforms);
	std::size_t addInstance(const Matrix44& instance_transform);
	void setInstance(std::size_t index, const Matrix44& instance_transform);
	// Swap-and-pop removal: the last instance moves into `index`.
	void removeInstance(std::size_t index);
	void clearInstances();

	void render(Camera* camera) override;

	bool isInstanced() const { return instance_models.size() > 1; }
	std::size_t getInstanceCount() const { return instance_models.size(); }
	const Matrix44& getInstanceModel(std::size_t index) const { return instance_models.at(index); }
	const std::string& getAssetId() const { return asset_id; }

private:
	void refreshEntityModel();

	Mesh* mesh = nullptr; // borrowed from the framework Mesh cache via AssetCatalog
	Material material;
	Shader* instanced_shader = nullptr; // borrowed from the framework Shader cache
	Matrix44 asset_transform;
	std::string asset_id;
	std::vector<Matrix44> instance_models; // final model matrices (asset then instance)
};

using DynamicViewId = std::uint32_t;
constexpr DynamicViewId invalid_dynamic_view = 0;

// Owns every render view of the colony. Static views come from scene.json; dynamic
// views are created/moved/destroyed by the simulation adapter (P03+) through stable
// integer handles, never through pointers into the instance arrays.
class ColonyWorld
{
public:
	explicit ColonyWorld(const AssetCatalog& assets);
	~ColonyWorld();
	ColonyWorld(const ColonyWorld&) = delete;
	ColonyWorld& operator=(const ColonyWorld&) = delete;

	bool buildStaticViews(const SceneLoadResult& scene, DataLoadError& error);

	// The asset must already be loaded through AssetCatalog::loadReferencedAssets().
	// Views sharing asset/material are drawn as one instanced group.
	DynamicViewId createDynamicView(const std::string& asset_id, const std::string& material_id, DataLoadError& error);
	// Gameplay position (y is the planar gameplay height, normally 0) and yaw in radians
	// using the world_transform convention. The view is drawn on the visual surface height.
	bool setDynamicViewPose(DynamicViewId id, const Vector3& gameplay_position, float yaw_radians);
	bool destroyDynamicView(DynamicViewId id);
	void clearDynamicViews();
	bool hasDynamicView(DynamicViewId id) const { return dynamic_records.count(id) != 0; }
	std::size_t getDynamicViewCount() const { return dynamic_records.size(); }

	// Visual-only height of walkable static surfaces (terrain = 0, interior floor = 0.1).
	float getVisualGroundHeight(float x, float z) const { return surface_heights.heightAt(x, z); }

	void render(Camera* camera);
	std::size_t getStaticViewCount() const { return static_views.size(); }
	std::size_t getInstanceCount() const { return instance_count; }
	std::size_t getInstanceCountForAsset(const std::string& asset_id) const;
	std::size_t getInstancedViewCount() const;

private:
	struct DynamicGroup
	{
		std::unique_ptr<EntityMesh> view;
		float forward_yaw = 0.0f;
		std::vector<DynamicViewId> slot_owners; // slot index -> view id
	};
	struct DynamicRecord
	{
		DynamicGroup* group = nullptr;
		std::size_t slot = 0;
	};

	std::unique_ptr<EntityMesh> makeView(const AssetDefinition& asset, const SemanticMaterial& material) const;

	const AssetCatalog* assets;
	std::vector<std::unique_ptr<EntityMesh>> static_views;
	std::size_t instance_count = 0;
	SurfaceHeightMap surface_heights;

	std::map<std::pair<std::string, std::string>, DynamicGroup> dynamic_groups;
	std::unordered_map<DynamicViewId, DynamicRecord> dynamic_records;
	DynamicViewId next_dynamic_id = 1;
};
