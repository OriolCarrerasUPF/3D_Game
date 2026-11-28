#include "colony_world.h"

#include "world_transform.h"

#include "framework/camera.h"
#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	Matrix44 makeSceneTransform(const StaticSceneInstance& instance)
	{
		return world_transform::makeInstanceTransform(instance.position,
			instance.rotation_y_degrees * DEG2RAD, instance.scale);
	}

	Matrix44 makeAssetTransform(const AssetDefinition& asset)
	{
		return world_transform::makeAssetTransform(asset.default_scale, asset.pivot_offset);
	}

	// World XZ rectangle and top height of an instance's picking bounds.
	void registerWalkableSurface(SurfaceHeightMap& heights, const AssetDefinition& asset, const Matrix44& instance_transform)
	{
		const Vector3& lo = asset.picking_bounds_min;
		const Vector3& hi = asset.picking_bounds_max;
		float min_x = std::numeric_limits<float>::max(), min_z = std::numeric_limits<float>::max();
		float max_x = std::numeric_limits<float>::lowest(), max_z = std::numeric_limits<float>::lowest();
		float top_y = std::numeric_limits<float>::lowest();
		for (int corner = 0; corner < 8; ++corner)
		{
			const Vector3 local((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z);
			const Vector3 world = instance_transform * local;
			min_x = std::min(min_x, world.x); max_x = std::max(max_x, world.x);
			min_z = std::min(min_z, world.z); max_z = std::max(max_z, world.z);
			top_y = std::max(top_y, world.y);
		}
		heights.addSurface(min_x, min_z, max_x, max_z, top_y);
	}

	// Overlay heights above the visual ground, so markers never z-fight the floor.
	constexpr float cell_marker_lift = 0.03f;
	constexpr float route_lift = 0.15f;
	constexpr float selection_lift = 0.06f;
	constexpr float min_building_pick_height = 0.4f;

	// Rotates an XZ offset by yaw with the world_transform convention (+90 deg maps +X to +Z).
	Vector3 rotateXZ(const Vector3& offset, float yaw)
	{
		const float c = std::cos(yaw);
		const float s = std::sin(yaw);
		return Vector3(offset.x * c - offset.z * s, offset.y, offset.x * s + offset.z * c);
	}
}

// ---------------------------------------------------------------------------- EntityMesh

EntityMesh::EntityMesh(Mesh* mesh, const Material& material, Shader* instanced_shader,
	const Matrix44& asset_transform, const std::string& asset_id)
	: mesh(mesh)
	, material(material)
	, instanced_shader(instanced_shader)
	, asset_transform(asset_transform)
	, asset_id(asset_id)
{
}

void EntityMesh::refreshEntityModel()
{
	// A single instance is drawn through Entity::model so parenting keeps working.
	if (instance_models.size() == 1)
		model = instance_models.front();
	else
		model.setIdentity();
}

void EntityMesh::setInstances(const std::vector<Matrix44>& instance_transforms)
{
	instance_models.clear();
	instance_models.reserve(instance_transforms.size());
	for (const Matrix44& instance_transform : instance_transforms)
		instance_models.push_back(world_transform::composeAssetThenInstance(asset_transform, instance_transform));
	refreshEntityModel();
}

std::size_t EntityMesh::addInstance(const Matrix44& instance_transform)
{
	instance_models.push_back(world_transform::composeAssetThenInstance(asset_transform, instance_transform));
	refreshEntityModel();
	return instance_models.size() - 1;
}

void EntityMesh::setInstance(std::size_t index, const Matrix44& instance_transform)
{
	if (index >= instance_models.size())
		return;
	instance_models[index] = world_transform::composeAssetThenInstance(asset_transform, instance_transform);
	if (instance_models.size() == 1)
		model = instance_models.front();
}

void EntityMesh::removeInstance(std::size_t index)
{
	if (index >= instance_models.size())
		return;
	instance_models[index] = instance_models.back();
	instance_models.pop_back();
	refreshEntityModel();
}

void EntityMesh::clearInstances()
{
	instance_models.clear();
	refreshEntityModel();
}

void EntityMesh::render(Camera* camera)
{
	if (mesh && camera && !instance_models.empty())
	{
		const bool instanced = isInstanced();
		Shader* active_shader = instanced ? instanced_shader : material.shader;
		if (active_shader && active_shader->compiled)
		{
			active_shader->enable();
			active_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
			active_shader->setUniform("u_color", material.color);

			if (instanced)
			{
				mesh->renderInstanced(GL_TRIANGLES, instance_models.data(), static_cast<int>(instance_models.size()));
			}
			else
			{
				active_shader->setUniform("u_model", getGlobalMatrix());
				mesh->render(GL_TRIANGLES);
			}
			active_shader->disable();
		}
	}
	Entity::render(camera);
}

// ---------------------------------------------------------------------------- ColonyWorld

ColonyWorld::ColonyWorld(const AssetCatalog& assets, const GameDefinitions& definitions)
	: assets(&assets)
	, definitions(&definitions)
{
}

ColonyWorld::~ColonyWorld() = default;

std::unique_ptr<EntityMesh> ColonyWorld::makeView(const AssetDefinition& asset, const SemanticMaterial& material) const
{
	Material render_material;
	render_material.shader = assets->getStandardShader();
	render_material.color = material.tint;
	auto view = std::make_unique<EntityMesh>(asset.mesh, render_material, assets->getInstancedShader(),
		makeAssetTransform(asset), asset.id);
	view->name = asset.id;
	return view;
}

bool ColonyWorld::buildStaticViews(const SceneLoadResult& scene, DataLoadError& error)
{
	if (!scene)
	{
		error = *scene.error;
		return false;
	}

	struct InstanceGroup
	{
		const AssetDefinition* asset = nullptr;
		const SemanticMaterial* material = nullptr;
		std::vector<Matrix44> transforms;
	};
	std::map<std::pair<std::string, std::string>, InstanceGroup> groups;
	SurfaceHeightMap pending_heights;

	for (const StaticSceneInstance& instance : scene.instances)
	{
		const AssetDefinition* asset = assets->findAsset(instance.asset_id);
		const SemanticMaterial* material = assets->findMaterial(instance.material_id);
		if (!asset || !material)
		{
			error = DataLoadError{ "data/arkfall/scene.json", "/instances/" + instance.id,
				"scene instance references an undefined asset or material" };
			return false;
		}

		const Matrix44 transform = makeSceneTransform(instance);
		if (asset->walkable_surface)
			registerWalkableSurface(pending_heights, *asset, transform);

		if (!asset->mesh)
		{
			if (asset->load_failure_is_fatal)
			{
				error = DataLoadError{ asset->model_path, "/instances/" + instance.id,
					"required scene asset was not loaded" };
				return false;
			}
			continue;
		}

		InstanceGroup& group = groups[{ instance.asset_id, instance.material_id }];
		group.asset = asset;
		group.material = material;
		group.transforms.push_back(transform);
	}

	std::vector<std::unique_ptr<EntityMesh>> pending_views;
	std::size_t pending_instance_count = 0;
	for (const auto& [key, group] : groups)
	{
		(void)key;
		if (!group.asset || !group.material || !group.asset->mesh)
			continue;
		std::unique_ptr<EntityMesh> view = makeView(*group.asset, *group.material);
		view->setInstances(group.transforms);
		pending_instance_count += group.transforms.size();
		pending_views.push_back(std::move(view));
	}

	static_views.swap(pending_views);
	instance_count = pending_instance_count;
	surface_heights = pending_heights;
	static_surface_heights = pending_heights;
	return true;
}

// ---------------------------------------------------------------------------- simulation views

void ColonyWorld::syncVisuals(const ColonySimulation& simulation, float alpha)
{
	alpha = std::clamp(alpha, 0.0f, 1.0f);
	syncBuildings(simulation);
	if (corridor_revision != simulation.getGrid().getRevision())
		syncCorridorPieces(simulation);
	syncColonists(simulation, alpha);
	const NavigationGrid& grid = simulation.getGrid();
	if (overlay_revision != grid.getRevision())
		rebuildNavigationOverlay(grid);
	updateRouteOverlay(simulation, alpha);
	updateSelectionOverlay();
	updateBuildingSelectionOverlay(simulation);
	updateGhostOverlay(grid);
}

void ColonyWorld::syncColonists(const ColonySimulation& simulation, float alpha)
{
	// Remove views of colonists that no longer exist (later: dead colonists, restart).
	for (auto it = colonist_views.begin(); it != colonist_views.end();)
	{
		const ColonistRecord* colonist = simulation.findColonist(it->first);
		// P05B: a colonist inside a dome is hidden (no view, not pickable).
		if (!colonist || colonist->state == ColonistState::Dead || colonist->job != it->second.job ||
			colonist->current_dome != INVALID_BUILDING)
		{
			destroyDynamicView(it->second.view);
			it = colonist_views.erase(it);
		}
		else
			++it;
	}

	const NavigationGrid& grid = simulation.getGrid();
	const float cell = grid.getCellSize();
	for (const ColonistRecord& colonist : simulation.getColonists())
	{
		if (colonist.state == ColonistState::Dead || colonist.current_dome != INVALID_BUILDING)
			continue;
		auto it = colonist_views.find(colonist.id);
		if (it == colonist_views.end())
		{
			// New colonist (or new role tint after a job change): create its view.
			const ColonistRoleDefinition* role = definitions->colonists.findRole(colonist.job);
			if (!role)
				throw std::runtime_error("colonist " + std::to_string(colonist.id) + " has no role visual");
			DataLoadError error;
			const DynamicViewId view = createDynamicView(role->asset_id, role->material_id, error);
			if (view == invalid_dynamic_view)
				throw std::runtime_error(error.describe());
			it = colonist_views.emplace(colonist.id, ColonistView{ view, colonist.job }).first;
		}
		Vector3 position(lerp(colonist.previous_position.x, colonist.position.x, alpha), 0.0f,
			lerp(colonist.previous_position.z, colonist.position.z, alpha));
		// Passage axis (C5.2): blend the offsets of the cell the colonist left and the one it
		// walks to, so it moves along the middle of corridors and through the door frame.
		const Cell from = colonist.last_cell;
		const Cell to = colonist.route.finished() ? from : colonist.route.cells[colonist.route.next_index];
		const auto offset_from = base_layout::offsetAt(passage_offsets, from);
		const auto offset_to = base_layout::offsetAt(passage_offsets, to);
		float t = 0.0f;
		if (!(to == from))
		{
			const Vector3 start = grid.cellCenter(from);
			t = std::clamp(std::sqrt((position.x - start.x) * (position.x - start.x) +
				(position.z - start.z) * (position.z - start.z)) / cell, 0.0f, 1.0f);
		}
		position.x += lerp(offset_from.first, offset_to.first, t) * cell;
		position.z += lerp(offset_from.second, offset_to.second, t) * cell;
		const float yaw = lerpAngleRad(colonist.previous_yaw, colonist.yaw, alpha);
		setDynamicViewPose(it->second.view, position, yaw);
	}
}

void ColonyWorld::syncBuildings(const ColonySimulation& simulation)
{
	// Remove views of buildings that no longer exist (restart; demolition is not in the MVP).
	for (auto it = building_views.begin(); it != building_views.end();)
	{
		if (!simulation.findBuilding(it->first))
		{
			for (DynamicViewId view : it->second.views)
				destroyDynamicView(view);
			it = building_views.erase(it);
		}
		else
			++it;
	}

	const NavigationGrid& grid = simulation.getGrid();
	const float cell = grid.getCellSize();
	for (const BuildingRecord& building : simulation.getBuildings())
	{
		if (building_views.count(building.id))
			continue; // buildings never move: posed once
		const BuildingDefinition* def = simulation.findDefinition(building.type);
		if (!def || building.footprint_cells.empty())
			continue;

		// Placement frame: footprint centre and quarter-turn yaw (the Exit Door is a 2 x 3
		// airlock like any other footprint since P05B).
		const float base_yaw = static_cast<float>(building.quarter_turns) * 0.5f * PI;
		int cx0 = building.footprint_cells.front().x, cx1 = cx0, cz0 = building.footprint_cells.front().z, cz1 = cz0;
		for (Cell c : building.footprint_cells)
		{
			cx0 = std::min(cx0, c.x); cx1 = std::max(cx1, c.x);
			cz0 = std::min(cz0, c.z); cz1 = std::max(cz1, c.z);
		}
		const float min_x = grid.getOriginX() + static_cast<float>(cx0) * cell;
		const float max_x = grid.getOriginX() + static_cast<float>(cx1 + 1) * cell;
		const float min_z = grid.getOriginZ() + static_cast<float>(cz0) * cell;
		const float max_z = grid.getOriginZ() + static_cast<float>(cz1 + 1) * cell;
		const Vector3 centre((min_x + max_x) * 0.5f, 0.0f, (min_z + max_z) * 0.5f);

		BuildingView view;
		float top = min_building_pick_height;
		for (const BuildingVisualDefinition& visual : def->visuals)
		{
			DataLoadError error;
			const DynamicViewId id = createDynamicView(visual.asset_id, visual.material_id, error);
			if (id == invalid_dynamic_view)
				throw std::runtime_error(error.describe());
			const AssetDefinition* asset = assets->findAsset(visual.asset_id);
			const Vector3 position = centre + rotateXZ(visual.offset, base_yaw);
			setDynamicViewPose(id, position, base_yaw + visual.yaw_degrees * DEG2RAD, visual.scale, false);
			if (asset)
				top = std::max(top, asset->picking_bounds_max.y * visual.scale + visual.offset.y);
			view.views.push_back(id);
		}
		// Corridors and airlocks have a floor: colonists walking there stand on it.
		if (def->hasWalkableFootprint())
			surface_heights.addSurface(min_x, min_z, max_x, max_z, baseFloorHeight());
		view.box_min = Vector3(min_x, 0.0f, min_z);
		view.box_max = Vector3(max_x, top, max_z);
		building_views.emplace(building.id, std::move(view));
	}
}

float ColonyWorld::baseFloorHeight() const
{
	// Top of the team-authored corridor floor (0.1 m, plan C5.2); the airlock matches it.
	const AssetDefinition* floor = assets->findAsset("corridor_floor");
	return floor ? floor->picking_bounds_max.y : 0.1f; // picking bounds are already scaled
}

void ColonyWorld::syncCorridorPieces(const ColonySimulation& simulation)
{
	const NavigationGrid& grid = simulation.getGrid();
	corridor_revision = grid.getRevision();
	for (DynamicViewId piece : corridor_piece_views)
		destroyDynamicView(piece);
	corridor_piece_views.clear();
	passage_offsets.clear();

	base_layout::CellSet corridor, inside;
	base_layout::EdgeSet open_edges;
	for (int z = 0; z < grid.getHeight(); ++z)
		for (int x = 0; x < grid.getWidth(); ++x)
			if (grid.getZone(Cell{ x, z }) == Zone::Inside)
				inside.insert({ x, z });
	for (const BuildingRecord& building : simulation.getBuildings())
	{
		const BuildingDefinition* def = simulation.findDefinition(building.type);
		if (!def || !building.isComplete())
			continue;
		if (def->placement == PlacementKind::Corridor)
			for (Cell c : building.footprint_cells)
				corridor.insert(base_layout::key(c));
		else if (def->placement == PlacementKind::Airlock)
			base_layout::addAirlockOffsets(building.footprint_cells, building.quarter_turns, passage_offsets);
		else if (def->isDome())
		{
			// Walkable domes are inside cells, but for the corridor view they are a wall
			// (the dome shell) except at their entrances.
			for (Cell c : building.footprint_cells)
				inside.erase(base_layout::key(c));
			// A dome entrance leaves the corridor open towards the dome.
			for (Cell access : building.access_cells)
				for (Cell c : building.footprint_cells)
					if (std::abs(c.x - access.x) + std::abs(c.z - access.z) == 1)
						open_edges.insert({ base_layout::key(access), base_layout::key(c) });
		}
	}

	const std::vector<base_layout::Piece> pieces = base_layout::corridorPieces(corridor, inside, open_edges);
	base_layout::addCorridorOffsets(pieces, passage_offsets);
	const float cell = grid.getCellSize();
	for (const base_layout::Piece& piece : pieces)
	{
		const char* asset_id = piece.kind == base_layout::PieceKind::Wall ? base_layout::wall_asset_id
			: piece.kind == base_layout::PieceKind::Arch ? base_layout::arch_asset_id : base_layout::junction_asset_id;
		DataLoadError error;
		const DynamicViewId id = createDynamicView(asset_id, base_layout::piece_material_id, error);
		if (id == invalid_dynamic_view)
			throw std::runtime_error(error.describe());
		const Vector3 position(grid.getOriginX() + piece.x * cell, 0.0f, grid.getOriginZ() + piece.z * cell);
		setDynamicViewPose(id, position, piece.yaw_degrees * DEG2RAD, 1.0f, false);
		corridor_piece_views.push_back(id);
	}
}

void ColonyWorld::updateBuildingSelectionOverlay(const ColonySimulation& simulation)
{
	std::vector<Vector3> outline;
	std::vector<Vector3> access;
	const BuildingRecord* building = simulation.findBuilding(selected_building);
	const auto view = building_views.find(selected_building);
	if (building && view != building_views.end())
	{
		const NavigationGrid& grid = simulation.getGrid();
		const Vector3& lo = view->second.box_min;
		const Vector3& hi = view->second.box_max;
		const float y = getVisualGroundHeight((lo.x + hi.x) * 0.5f, (lo.z + hi.z) * 0.5f) + selection_lift;
		const Vector3 a(lo.x, y, lo.z), b(hi.x, y, lo.z), c(hi.x, y, hi.z), d(lo.x, y, hi.z);
		outline.insert(outline.end(), { a, b, b, c, c, d, d, a });
		for (Cell cell : building->access_cells)
			addCellQuad(access, grid, cell, grid.getCellSize() * 0.2f, cell_marker_lift);
	}
	setOverlayLayer("building_selection", "ui_selection", OverlayPrimitive::Lines, outline, false);
	setOverlayLayer("building_access", "ui_access", OverlayPrimitive::Triangles, access, false);
}

void ColonyWorld::clearSimulationViews()
{
	for (const auto& [id, view] : colonist_views)
	{
		(void)id;
		destroyDynamicView(view.view);
	}
	colonist_views.clear();
	for (const auto& [id, view] : building_views)
	{
		(void)id;
		for (DynamicViewId dynamic : view.views)
			destroyDynamicView(dynamic);
	}
	building_views.clear();
	for (DynamicViewId piece : corridor_piece_views)
		destroyDynamicView(piece);
	corridor_piece_views.clear();
	corridor_revision = std::numeric_limits<std::uint64_t>::max();
	passage_offsets.clear();
	surface_heights = static_surface_heights;
	selected_building = INVALID_BUILDING;
	overlay_revision = std::numeric_limits<std::uint64_t>::max();
	selected_colonist = INVALID_COLONIST;
	ghost = PlacementGhost();
	clearOverlayLayers();
}

void ColonyWorld::addCellQuad(std::vector<Vector3>& out, const NavigationGrid& grid, Cell cell, float inset, float lift) const
{
	const Vector3 c = grid.cellCenter(cell);
	const float half = grid.getCellSize() * 0.5f - inset;
	const float y = getVisualGroundHeight(c.x, c.z) + lift;
	const Vector3 a(c.x - half, y, c.z - half), b(c.x + half, y, c.z - half);
	const Vector3 d(c.x - half, y, c.z + half), e(c.x + half, y, c.z + half);
	out.insert(out.end(), { a, b, e, a, e, d });
}

void ColonyWorld::rebuildNavigationOverlay(const NavigationGrid& grid)
{
	overlay_revision = grid.getRevision();
	const float inset = grid.getCellSize() * 0.05f;

	std::vector<Vector3> blocked;
	for (int z = 0; z < grid.getHeight(); ++z)
		for (int x = 0; x < grid.getWidth(); ++x)
			if (grid.isBlocked(Cell{ x, z }))
				addCellQuad(blocked, grid, Cell{ x, z }, inset, cell_marker_lift);
	setOverlayLayer("debug_blocked", "debug_blocked", OverlayPrimitive::Triangles, blocked, true);

	std::vector<Vector3> open_doors, closed_doors;
	for (std::size_t door = 0; door < grid.getDoorCount(); ++door)
		for (const DoorPortal& portal : grid.getDoorPortals(door))
		{
			std::vector<Vector3>& out = grid.isDoorOpen(door) ? open_doors : closed_doors;
			addCellQuad(out, grid, portal.outside, inset, cell_marker_lift);
			addCellQuad(out, grid, portal.inside, inset, cell_marker_lift);
		}
	setOverlayLayer("debug_door_open", "debug_door_open", OverlayPrimitive::Triangles, open_doors, true);
	setOverlayLayer("debug_door_closed", "debug_door_closed", OverlayPrimitive::Triangles, closed_doors, true);
}

void ColonyWorld::updateRouteOverlay(const ColonySimulation& simulation, float alpha)
{
	const NavigationGrid& grid = simulation.getGrid();
	std::vector<Vector3> lines;
	for (const ColonistRecord& colonist : simulation.getColonists())
	{
		if (colonist.route.finished() || colonist.state == ColonistState::Dead)
			continue;
		Vector3 from(lerp(colonist.previous_position.x, colonist.position.x, alpha), 0.0f,
			lerp(colonist.previous_position.z, colonist.position.z, alpha));
		from.y = getVisualGroundHeight(from.x, from.z) + route_lift;
		for (std::size_t i = colonist.route.next_index; i < colonist.route.cells.size(); ++i)
		{
			Vector3 to = grid.cellCenter(colonist.route.cells[i]);
			to.y = getVisualGroundHeight(to.x, to.z) + route_lift;
			lines.push_back(from);
			lines.push_back(to);
			from = to;
		}
	}
	setOverlayLayer("debug_route", "debug_path", OverlayPrimitive::Lines, lines, true);
}

void ColonyWorld::updateSelectionOverlay()
{
	std::vector<Vector3> ring;
	const auto it = colonist_views.find(selected_colonist);
	if (it != colonist_views.end())
	{
		const auto record = dynamic_records.find(it->second.view);
		if (record != dynamic_records.end())
		{
			const Vector3 center = record->second.visual_position;
			const int segments = 24;
			for (const float radius : { 0.62f, 0.7f })
				for (int i = 0; i < segments; ++i)
				{
					const float a0 = 2.0f * PI * static_cast<float>(i) / segments;
					const float a1 = 2.0f * PI * static_cast<float>(i + 1) / segments;
					ring.push_back(Vector3(center.x + radius * std::cos(a0), center.y + selection_lift, center.z + radius * std::sin(a0)));
					ring.push_back(Vector3(center.x + radius * std::cos(a1), center.y + selection_lift, center.z + radius * std::sin(a1)));
				}
		}
	}
	setOverlayLayer("selection", "ui_selection", OverlayPrimitive::Lines, ring, false);
}

void ColonyWorld::setPlacementGhost(bool visible, Cell cell, bool valid, int quarter_turns)
{
	ghost.visible = visible;
	ghost.cell = cell;
	ghost.valid = valid;
	ghost.quarter_turns = ((quarter_turns % 4) + 4) % 4;
}

void ColonyWorld::updateGhostOverlay(const NavigationGrid& grid)
{
	std::vector<Vector3> quad;
	std::vector<Vector3> arrow;
	if (ghost.visible && grid.inBounds(ghost.cell))
	{
		addCellQuad(quad, grid, ghost.cell, grid.getCellSize() * 0.04f, cell_marker_lift + 0.01f);
		// Facing arrow for the current quarter turn (world_transform yaw convention).
		const Vector3 c = grid.cellCenter(ghost.cell);
		const float y = getVisualGroundHeight(c.x, c.z) + route_lift;
		const Vector3 facing = world_transform::directionFromYaw(ghost.quarter_turns * 0.5f * PI);
		const Vector3 side(facing.z, 0.0f, -facing.x);
		const Vector3 tip(c.x + facing.x * 0.45f, y, c.z + facing.z * 0.45f);
		const Vector3 base(c.x - facing.x * 0.3f, y, c.z - facing.z * 0.3f);
		arrow.insert(arrow.end(), { base, tip,
			tip, Vector3(tip.x - facing.x * 0.2f + side.x * 0.15f, y, tip.z - facing.z * 0.2f + side.z * 0.15f),
			tip, Vector3(tip.x - facing.x * 0.2f - side.x * 0.15f, y, tip.z - facing.z * 0.2f - side.z * 0.15f) });
	}
	setOverlayLayer("ghost_valid", "ui_ghost_valid", OverlayPrimitive::Triangles, ghost.valid ? quad : std::vector<Vector3>(), false);
	setOverlayLayer("ghost_invalid", "ui_ghost_invalid", OverlayPrimitive::Triangles, ghost.valid ? std::vector<Vector3>() : quad, false);
	setOverlayLayer("ghost_arrow", "ui_selection", OverlayPrimitive::Lines, arrow, false);
}

// ---------------------------------------------------------------------------- picking

bool ColonyWorld::pickColonist(const picking::Ray& ray, ColonistId& out_id, float& out_distance) const
{
	bool found = false;
	float best = std::numeric_limits<float>::max();
	ColonistId best_id = INVALID_COLONIST;
	for (const auto& [id, view] : colonist_views) // std::map: ascending ids, so ties keep the lowest
	{
		const auto record = dynamic_records.find(view.view);
		if (record == dynamic_records.end() || !record->second.group || !record->second.group->asset)
			continue;
		const AssetDefinition& asset = *record->second.group->asset;
		float t = 0.0f;
		if (picking::intersectPlacedBox(ray, asset.picking_bounds_min, asset.picking_bounds_max,
				record->second.visual_position, record->second.visual_yaw, t) && t < best)
		{
			best = t;
			best_id = id;
			found = true;
		}
	}
	if (found)
	{
		out_id = best_id;
		out_distance = best;
	}
	return found;
}

bool ColonyWorld::pickBuilding(const picking::Ray& ray, BuildingId& out_id, float& out_distance) const
{
	bool found = false;
	float best = std::numeric_limits<float>::max();
	BuildingId best_id = INVALID_BUILDING;
	for (const auto& [id, view] : building_views) // ascending ids: ties keep the lowest
	{
		float t = 0.0f;
		if (picking::intersectAabb(ray, view.box_min, view.box_max, t) && t < best)
		{
			best = t;
			best_id = id;
			found = true;
		}
	}
	if (found)
	{
		out_id = best_id;
		out_distance = best;
	}
	return found;
}

bool ColonyWorld::pickGround(const picking::Ray& ray, Vector3& out_point) const
{
	// Intersect y = 0, then refine once against the visual height found there (the
	// interior floor is 0.1 m above the terrain).
	Vector3 point;
	float t = 0.0f;
	if (!picking::intersectHorizontalPlane(ray, 0.0f, point, t))
		return false;
	const float height = getVisualGroundHeight(point.x, point.z);
	if (height != 0.0f)
	{
		Vector3 refined;
		if (picking::intersectHorizontalPlane(ray, height, refined, t) &&
			getVisualGroundHeight(refined.x, refined.z) == height)
			point = refined;
	}
	out_point = point;
	return true;
}

// ---------------------------------------------------------------------------- dynamic views

DynamicViewId ColonyWorld::createDynamicView(const std::string& asset_id, const std::string& material_id, DataLoadError& error)
{
	const AssetDefinition* asset = assets->findAsset(asset_id);
	const SemanticMaterial* material = assets->findMaterial(material_id);
	if (!asset || !material)
	{
		error = DataLoadError{ "", "", "dynamic view references unknown asset '" + asset_id + "' or material '" + material_id + "'" };
		return invalid_dynamic_view;
	}
	if (!asset->mesh)
	{
		error = DataLoadError{ asset->model_path, "", "dynamic view asset '" + asset_id + "' has not been loaded" };
		return invalid_dynamic_view;
	}

	DynamicGroup& group = dynamic_groups[{ asset_id, material_id }];
	if (!group.view)
	{
		group.view = makeView(*asset, *material);
		group.asset = asset;
		if (!world_transform::forwardAxisYaw(asset->forward_axis, group.forward_yaw))
			group.forward_yaw = 0.0f;
	}

	const DynamicViewId id = next_dynamic_id++;
	const std::size_t slot = group.view->addInstance(Matrix44());
	group.slot_owners.push_back(id);
	DynamicRecord record;
	record.group = &group;
	record.slot = slot;
	dynamic_records[id] = record;
	return id;
}

bool ColonyWorld::setDynamicViewPose(DynamicViewId id, const Vector3& gameplay_position, float yaw_radians, float scale,
	bool on_visual_surface)
{
	const auto it = dynamic_records.find(id);
	if (it == dynamic_records.end())
		return false;
	DynamicRecord& record = it->second;
	DynamicGroup& group = *record.group;
	const float surface = on_visual_surface ? surface_heights.heightAt(gameplay_position.x, gameplay_position.z) : 0.0f;
	record.visual_position.set(gameplay_position.x, gameplay_position.y + surface, gameplay_position.z);
	record.visual_yaw = yaw_radians + group.forward_yaw;
	group.view->setInstance(record.slot, world_transform::makeInstanceTransform(
		record.visual_position, record.visual_yaw, Vector3(scale, scale, scale)));
	return true;
}

bool ColonyWorld::destroyDynamicView(DynamicViewId id)
{
	const auto it = dynamic_records.find(id);
	if (it == dynamic_records.end())
		return false;
	DynamicGroup& group = *it->second.group;
	const std::size_t slot = it->second.slot;
	const std::size_t last = group.slot_owners.size() - 1;

	group.view->removeInstance(slot); // moves the last instance into `slot`
	if (slot != last)
	{
		const DynamicViewId moved = group.slot_owners[last];
		group.slot_owners[slot] = moved;
		dynamic_records[moved].slot = slot;
	}
	group.slot_owners.pop_back();
	dynamic_records.erase(it);
	return true;
}

void ColonyWorld::clearDynamicViews()
{
	dynamic_records.clear();
	dynamic_groups.clear();
	colonist_views.clear();
	building_views.clear();
	next_dynamic_id = 1;
}

// ---------------------------------------------------------------------------- overlay

void ColonyWorld::setOverlayLayer(const std::string& name, const std::string& material_id, OverlayPrimitive primitive,
	const std::vector<Vector3>& vertices, bool debug_only)
{
	OverlayLayer& layer = overlay_layers[name];
	if (!layer.mesh)
		layer.mesh = std::make_unique<Mesh>();
	const SemanticMaterial* material = assets->findMaterial(material_id);
	layer.color = material ? material->tint : Vector4(1.0f, 0.0f, 1.0f, 1.0f);
	layer.primitive = primitive;
	layer.debug_only = debug_only;
	// Client-side vertex array (no VBO): the geometry changes often and is tiny.
	layer.mesh->vertices = vertices;
}

void ColonyWorld::clearOverlayLayers()
{
	overlay_layers.clear();
}

void ColonyWorld::renderOverlay(Camera* camera)
{
	Shader* shader = assets->getDebugShader();
	if (!camera || !shader || !shader->compiled || overlay_layers.empty())
		return;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_FALSE);
	glLineWidth(3.0f);
	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_model", Matrix44());
	for (auto& [name, layer] : overlay_layers)
	{
		(void)name;
		if (!layer.mesh || layer.mesh->vertices.empty() || (layer.debug_only && !debug_overlay_visible))
			continue;
		shader->setUniform("u_color", layer.color);
		layer.mesh->render(layer.primitive == OverlayPrimitive::Lines ? GL_LINES : GL_TRIANGLES);
	}
	shader->disable();
	glLineWidth(1.0f);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
}

void ColonyWorld::render(Camera* camera)
{
	for (const std::unique_ptr<EntityMesh>& view : static_views)
		view->render(camera);
	for (auto& [key, group] : dynamic_groups)
	{
		(void)key;
		if (group.view)
			group.view->render(camera);
	}
	renderOverlay(camera);
}

std::size_t ColonyWorld::getInstanceCountForAsset(const std::string& asset_id) const
{
	std::size_t count = 0;
	for (const std::unique_ptr<EntityMesh>& view : static_views)
	{
		if (view->getAssetId() == asset_id)
			count += view->getInstanceCount();
	}
	return count;
}

std::size_t ColonyWorld::getInstancedViewCount() const
{
	std::size_t count = 0;
	for (const std::unique_ptr<EntityMesh>& view : static_views)
		count += view->isInstanced() ? 1 : 0;
	return count;
}
