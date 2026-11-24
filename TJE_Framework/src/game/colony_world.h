#pragma once

#include "asset_catalog.h"
#include "colony_simulation.h"
#include "game_definitions.h"
#include "picking.h"
#include "scene_parser.h"
#include "surface_height_map.h"
#include "framework/entities/entity.h"
#include "graphics/material.h"

#include <cstdint>
#include <limits>
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

// View layer of the colony (plan C1). Owns every render view: the static scene groups
// from scene.json, dynamic views (colonists, later buildings and blueprints) addressed
// by stable integer handles, the visual floor height and the overlay layers. It reads
// the simulation through syncVisuals() and never modifies it.
class ColonyWorld
{
public:
	ColonyWorld(const AssetCatalog& assets, const GameDefinitions& definitions);
	~ColonyWorld();
	ColonyWorld(const ColonyWorld&) = delete;
	ColonyWorld& operator=(const ColonyWorld&) = delete;

	bool buildStaticViews(const SceneLoadResult& scene, DataLoadError& error);

	// ---- Simulation views -------------------------------------------------------
	// Once per frame: creates/removes colonist views, moves them (interpolated by alpha
	// between the last two ticks), rebuilds the navigation overlay when the grid revision
	// changes and refreshes the route, selection and placement-ghost overlays.
	// Throws std::runtime_error if a colonist's role visual cannot be created.
	void syncVisuals(const ColonySimulation& simulation, float interpolation_alpha);
	// Drops every simulation-driven view and overlay (restart).
	void clearSimulationViews();
	std::size_t getColonistViewCount() const { return colonist_views.size(); }

	// ---- Picking (P04) ------------------------------------------------------------
	// Nearest colonist whose transformed picking bounds the ray hits; equal distances
	// resolve to the lowest ColonistId. Uses the poses from the last syncVisuals().
	bool pickColonist(const picking::Ray& ray, ColonistId& out_id, float& out_distance) const;
	// Point where the ray meets the visual ground (terrain or the 0.1 m interior floor).
	bool pickGround(const picking::Ray& ray, Vector3& out_point) const;

	// ---- UI-driven markers (always visible, independent of the debug overlay) -----
	void setSelectedColonist(ColonistId id) { selected_colonist = id; }
	void setPlacementGhost(bool visible, Cell cell, bool valid, int quarter_turns);

	// ---- Low-level dynamic views -------------------------------------------------
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

	// ---- Overlay layers -------------------------------------------------------------
	// A named layer holds world-space triangles or lines drawn after the scene with
	// alpha blending; material_id selects the colour from materials.json. Debug-only
	// layers are hidden unless the debug overlay (F3) is visible.
	enum class OverlayPrimitive { Triangles, Lines };
	void setOverlayLayer(const std::string& name, const std::string& material_id, OverlayPrimitive primitive,
		const std::vector<Vector3>& vertices, bool debug_only);
	void clearOverlayLayers();
	void setDebugOverlayVisible(bool visible) { debug_overlay_visible = visible; }
	bool isDebugOverlayVisible() const { return debug_overlay_visible; }

	void render(Camera* camera);
	std::size_t getStaticViewCount() const { return static_views.size(); }
	std::size_t getInstanceCount() const { return instance_count; }
	std::size_t getInstanceCountForAsset(const std::string& asset_id) const;
	std::size_t getInstancedViewCount() const;

private:
	struct DynamicGroup
	{
		std::unique_ptr<EntityMesh> view;
		const AssetDefinition* asset = nullptr;
		float forward_yaw = 0.0f;
		std::vector<DynamicViewId> slot_owners; // slot index -> view id
	};
	struct DynamicRecord
	{
		DynamicGroup* group = nullptr;
		std::size_t slot = 0;
		Vector3 visual_position;  // last pose, for picking and markers
		float visual_yaw = 0.0f;  // includes the asset's forward-axis yaw
	};
	struct ColonistView
	{
		DynamicViewId view = invalid_dynamic_view;
		Job job = Job::Engineer;
	};
	struct OverlayLayer
	{
		std::unique_ptr<Mesh> mesh;
		Vector4 color;
		OverlayPrimitive primitive = OverlayPrimitive::Triangles;
		bool debug_only = true;
	};
	struct PlacementGhost
	{
		bool visible = false;
		Cell cell;
		bool valid = false;
		int quarter_turns = 0;
	};

	void syncColonists(const ColonySimulation& simulation, float alpha);
	void rebuildNavigationOverlay(const NavigationGrid& grid);
	void updateRouteOverlay(const ColonySimulation& simulation, float alpha);
	void updateSelectionOverlay();
	void updateGhostOverlay(const NavigationGrid& grid);
	void addCellQuad(std::vector<Vector3>& out, const NavigationGrid& grid, Cell cell, float inset, float lift) const;
	void renderOverlay(Camera* camera);
	std::unique_ptr<EntityMesh> makeView(const AssetDefinition& asset, const SemanticMaterial& material) const;

	const AssetCatalog* assets;
	const GameDefinitions* definitions;
	std::vector<std::unique_ptr<EntityMesh>> static_views;
	std::size_t instance_count = 0;
	SurfaceHeightMap surface_heights;

	std::map<std::pair<std::string, std::string>, DynamicGroup> dynamic_groups;
	std::unordered_map<DynamicViewId, DynamicRecord> dynamic_records;
	DynamicViewId next_dynamic_id = 1;

	std::map<ColonistId, ColonistView> colonist_views;
	std::uint64_t overlay_revision = std::numeric_limits<std::uint64_t>::max();
	ColonistId selected_colonist = INVALID_COLONIST;
	PlacementGhost ghost;

	std::map<std::string, OverlayLayer> overlay_layers;
	bool debug_overlay_visible = false;
};
