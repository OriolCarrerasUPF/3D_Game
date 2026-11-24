#include "game.h"
#include "framework/utils.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "framework/input.h"
#include "colony_ui_render.h"
#include "scene_parser.h"
#include "scenario_loader.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace
{
	constexpr float camera_near_plane = 0.1f;
	constexpr float camera_far_plane = 1000.0f;
	constexpr float click_slop_pixels = 6.0f;   // a press that moves farther is not a click
	constexpr float frame_padding_metres = 2.0f;

	// ---- TEST: show one building model at startup ---------------------------------
	// Put any building id from data/arkfall/assets.json here, or "" to show nothing:
	//   "building_exit_door"        "building_storage"          "building_oxygenator"
	//   "building_greenhouse"       "building_iron_processor"   "building_copper_processor"
	//   "building_aurite_processor" "building_iron_mine"        "building_copper_mine"
	//   "building_water_extractor"  "building_solar_panel"      "building_landing_pad"
	//   "building_haven_beacon"
	const char* const test_building_id = "building_solar_panel";
	constexpr float test_building_x = 16.0f;   // world metres: 8 m east of the base wall
	constexpr float test_building_z = 0.0f;
}

Game* Game::instance = NULL;

Game::Game(int window_width, int window_height, SDL_Window* window)
	: window(window)
	, window_width(window_width)
	, window_height(window_height)
	, frame(0)
	, time(0.0f)
	, elapsed_time(0.0f)
	, fps(0)
	, max_fps(60) // FPS cap: change this number (0 = uncapped)
	, must_exit(false)
	, camera(nullptr)
	, mouse_locked(false)
	, asset_catalog()
	, definitions()
	, colony_world(asset_catalog, definitions)
	, simulation_clock()
	, simulation()
	, strategy_camera()
	, ui()
	, scene_stats_logged(false)
{
	if (!window)
		throw std::runtime_error("Cannot start the game without an SDL window.");
	if (window_width <= 0 || window_height <= 0)
		throw std::runtime_error("Cannot start the game with an empty window size.");

	AssetCatalogLoadResult catalog_result = asset_catalog.load(
		"data/arkfall/assets.json", "data/arkfall/materials.json");
	if (!catalog_result)
		throw std::runtime_error(catalog_result.error->describe());
	for (const std::string& material_id : ColonyUI::requiredMaterialIds())
		if (!asset_catalog.findMaterial(material_id))
			throw std::runtime_error("data/arkfall/materials.json: missing HUD/marker material '" + material_id + "'");

	// Startup order (plan C9.1): validate every JSON file and the cross-file rules
	// before any OpenGL resource is created; then build views, then the simulation.
	SceneParser scene_parser;
	SceneLoadResult scene_result = scene_parser.parse("data/arkfall/scene.json", asset_catalog);
	if (!scene_result)
		throw std::runtime_error(scene_result.error->describe());

	ScenarioLoader scenario_loader;
	ScenarioLoadResult scenario_result = scenario_loader.load("data/arkfall/scenario.json");
	if (!scenario_result)
		throw std::runtime_error(scenario_result.error->describe());
	DataLoadError blocker_error;
	if (!ScenarioLoader::validateSceneBlockers(scenario_result.scenario, scene_result.instances,
		"data/arkfall/scene.json", blocker_error))
		throw std::runtime_error(blocker_error.describe());

	ColonistCatalogLoadResult colonist_result = definitions.colonists.load("data/arkfall/colonists.json");
	if (!colonist_result)
		throw std::runtime_error(colonist_result.error->describe());
	colonist_result = definitions.colonists.validateVisuals(asset_catalog);
	if (!colonist_result)
		throw std::runtime_error(colonist_result.error->describe());

	// Only assets referenced by data are loaded: the static scene plus the role visuals
	// used for dynamic colonist views.
	std::set<std::string> referenced_assets;
	for (const StaticSceneInstance& instance : scene_result.instances)
		referenced_assets.insert(instance.asset_id);
	for (const auto& [job, role] : definitions.colonists.getRoles())
	{
		(void)job;
		referenced_assets.insert(role.asset_id);
	}

	// TEST building (see test_building_id at the top of this file)
	if (*test_building_id)
	{
		if (!asset_catalog.findAsset(test_building_id))
			throw std::runtime_error(std::string("test building: '") + test_building_id +
				"' is not an asset id in data/arkfall/assets.json");
		referenced_assets.insert(test_building_id);
	}

	// Validate all authored JSON and references before creating any OpenGL assets.
	AssetCatalogLoadResult resource_result = asset_catalog.loadReferencedAssets(referenced_assets);
	if (!resource_result)
		throw std::runtime_error(resource_result.error->describe());

	DataLoadError scene_build_error;
	if (!colony_world.buildStaticViews(scene_result, scene_build_error))
		throw std::runtime_error(scene_build_error.describe());

	std::string simulation_error;
	if (!simulation.initialize(scenario_result.scenario, definitions, simulation_error))
		throw std::runtime_error("scenario.json: " + simulation_error);
	colony_world.syncVisuals(simulation, 0.0f);

	// TEST building: visual only, it does not block colonists.
	if (*test_building_id)
	{
		DataLoadError test_error;
		const std::string& test_material = asset_catalog.findAsset(test_building_id)->material_id;
		const DynamicViewId test_view = colony_world.createDynamicView(test_building_id, test_material, test_error);
		if (test_view == invalid_dynamic_view)
			throw std::runtime_error(test_error.describe());
		colony_world.setDynamicViewPose(test_view, Vector3(test_building_x, 0.0f, test_building_z), 0.0f);
	}

	ui_renderer = std::make_unique<ColonyUIRenderer>(asset_catalog);

	// OpenGL flags
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);

	// Create our camera after fallible resource loading so construction failures need no GL cleanup here.
	camera = new Camera();
	updateViewport(window_width, window_height);
	frameInitialView();

	// ARKFALL keeps the cursor visible and free (strategy controls).
	SDL_ShowCursor(SDL_ENABLE);
	SDL_SetRelativeMouseMode(SDL_FALSE);
	instance = this;

	std::cout << " + Controls: WASD/arrows pan, Q/E or middle-drag rotate, wheel zoom, left-click select, "
		"B build mode, R rotate ghost, Space pause, -/+ speed, Esc/right-click cancel" << std::endl;
	std::cout << " + Debug keys: F3 overlay, F4 obstacle ahead of the colonist, F6 clear obstacles, "
		"F7 open/close Exit Door, F8 edge scrolling" << std::endl;
}

Game::~Game()
{
	if (Camera::current == camera)
		Camera::current = nullptr;
	delete camera;
	camera = nullptr;

	if (instance == this)
		instance = nullptr;
}

// ---------------------------------------------------------------------------- viewport and camera

void Game::updateViewport(int logical_width, int logical_height)
{
	window_width = std::max(logical_width, 1);
	window_height = std::max(logical_height, 1);
	// High-DPI: GL works in drawable pixels; UI, mouse and picking stay in logical pixels.
	int drawable_width = window_width;
	int drawable_height = window_height;
	SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
	glViewport(0, 0, std::max(drawable_width, 1), std::max(drawable_height, 1));
	ui.layout(static_cast<float>(window_width), static_cast<float>(window_height));
	if (camera)
		applyCameraToFramework();
}

void Game::frameInitialView()
{
	// Frame the base, the deposits and the colonists (GDD 3.2: show the base and the
	// nearest mines), computed from scenario data instead of fixed coordinates.
	const ScenarioDefinition& scenario = simulation.getScenario();
	const NavigationGrid& grid = simulation.getGrid();
	const float cell = grid.getCellSize();
	float min_x = grid.cellCenter(scenario.interior_min).x - cell * 0.5f;
	float min_z = grid.cellCenter(scenario.interior_min).z - cell * 0.5f;
	float max_x = grid.cellCenter(scenario.interior_max).x + cell * 0.5f;
	float max_z = grid.cellCenter(scenario.interior_max).z + cell * 0.5f;
	auto include = [&](Cell c) {
		const Vector3 p = grid.cellCenter(c);
		min_x = std::min(min_x, p.x - cell * 0.5f); max_x = std::max(max_x, p.x + cell * 0.5f);
		min_z = std::min(min_z, p.z - cell * 0.5f); max_z = std::max(max_z, p.z + cell * 0.5f);
		};
	for (const DepositDefinition& deposit : scenario.deposits)
		include(deposit.cell);
	for (const InitialColonistDefinition& colonist : scenario.initial_colonists)
		include(colonist.cell);

	strategy_camera.setBounds(grid.getOriginX(), grid.getOriginZ(),
		grid.getOriginX() + grid.getWidth() * cell, grid.getOriginZ() + grid.getHeight() * cell);
	strategy_camera.frame(min_x - frame_padding_metres, min_z - frame_padding_metres,
		max_x + frame_padding_metres, max_z + frame_padding_metres,
		window_width / static_cast<float>(window_height));
	applyCameraToFramework();
}

void Game::applyCameraToFramework()
{
	camera->lookAt(strategy_camera.getEye(), strategy_camera.getFocus(), Vector3(0.0f, 1.0f, 0.0f));
	camera->setPerspective(strategy_camera.getSettings().vertical_fov_degrees,
		window_width / static_cast<float>(window_height), camera_near_plane, camera_far_plane);
}

void Game::updateCamera(float real_dt)
{
	CameraInput input;
	if (Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP)) input.pan_forward += 1.0f;
	if (Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) input.pan_forward -= 1.0f;
	if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) input.pan_right += 1.0f;
	if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) input.pan_right -= 1.0f;
	if (Input::isKeyPressed(SDL_SCANCODE_E)) input.rotate += 1.0f;
	if (Input::isKeyPressed(SDL_SCANCODE_Q)) input.rotate -= 1.0f;

	// Input::mouse_delta accumulates -xrel/-yrel, so the motion is its negation.
	if (middle_dragging)
		input.drag_pixels.set(-Input::mouse_delta.x, -Input::mouse_delta.y);

	input.zoom_notches = pending_zoom_notches;
	pending_zoom_notches = 0.0f;

	const Vector2 pointer = Input::mouse_position;
	if (edge_scroll_enabled && !ui.isPointerOverUi(pointer.x, pointer.y) && !middle_dragging)
	{
		float edge_forward = 0.0f, edge_right = 0.0f;
		StrategyCamera::edgeScrollAxes(pointer, static_cast<float>(window_width), static_cast<float>(window_height),
			strategy_camera.getSettings().edge_scroll_margin_pixels, edge_forward, edge_right);
		input.pan_forward += edge_forward;
		input.pan_right += edge_right;
	}

	strategy_camera.update(input, real_dt);
	applyCameraToFramework();
}

picking::Ray Game::makePointerRay(float x, float y) const
{
	picking::Ray ray;
	ray.origin = camera->eye;
	ray.direction = camera->getRayDirection(static_cast<int>(x), static_cast<int>(y),
		static_cast<float>(window_width), static_cast<float>(window_height));
	return ray;
}

// ---------------------------------------------------------------------------- frame update

void Game::update(double seconds_elapsed)
{
	const float real_dt = static_cast<float>(seconds_elapsed);

	// 1. UI hover, then the camera on real time (also while paused).
	ui.handlePointer(PointerEvent{ PointerType::Move, PointerButton::None, Input::mouse_position.x, Input::mouse_position.y });
	updateCamera(real_dt);

	// 2. World clicks are resolved with this frame's camera matrices.
	processWorldClick();
	updatePlacementGhost();

	// 3. Player/debug commands, once per frame, before the ticks (plan C4).
	applyQueuedCommands();

	// 4. Gameplay advances only in fixed steps.
	const int steps = simulation_clock.advance(seconds_elapsed);
	for (int i = 0; i < steps; ++i)
		fixedUpdate(simulation_clock.getStepSeconds());

	// 5. Views follow the simulation once per frame, interpolated between fixed steps.
	if (ui.getSelectedColonist() != INVALID_COLONIST)
	{
		const ColonistRecord* selected = simulation.findColonist(ui.getSelectedColonist());
		if (!selected || selected->state == ColonistState::Dead)
			ui.clearSelection();
	}
	colony_world.setSelectedColonist(ui.getSelectedColonist());
	colony_world.syncVisuals(simulation, static_cast<float>(simulation_clock.getInterpolationAlpha()));
	ui.update(real_dt);
}

void Game::fixedUpdate(double fixed_dt)
{
	// Gameplay timing uses simulation time, never the wall-clock Game::time.
	simulation.tick(static_cast<float>(fixed_dt));
}

void Game::processWorldClick()
{
	if (!pending_world_click)
		return;
	pending_world_click = false;
	if (ui.isBuildMode())
	{
		ui.showMessage("Nothing to place yet: building types arrive with buildings.json (P05)");
		return;
	}
	ColonistId id = INVALID_COLONIST;
	float distance = 0.0f;
	if (colony_world.pickColonist(makePointerRay(pending_click_position.x, pending_click_position.y), id, distance))
		ui.select(id);
	else
		ui.clearSelection();
}

bool Game::isDoorCell(Cell cell) const
{
	for (const ExitDoorDefinition& door : simulation.getScenario().exit_doors)
		for (const DoorPortal& portal : door.crossings)
			if (portal.inside == cell || portal.outside == cell)
				return true;
	return false;
}

void Game::updatePlacementGhost()
{
	ghost_visible = false;
	const Vector2 pointer = Input::mouse_position;
	if (ui.isBuildMode() && !ui.isPointerOverUi(pointer.x, pointer.y))
	{
		Vector3 point;
		Cell cell;
		if (colony_world.pickGround(makePointerRay(pointer.x, pointer.y), point) &&
			simulation.getGrid().worldToCell(point.x, point.z, cell))
		{
			// Temporary P04 rule until validatePlacement (P07): free, non-door cells.
			ghost_visible = true;
			ghost_cell = cell;
			if (simulation.getGrid().isBlocked(cell))
				ghost_status = "blocked";
			else if (isDoorCell(cell))
				ghost_status = "door crossing";
			else
				ghost_status = "free";
			ghost_valid = ghost_status == "free";
		}
	}
	colony_world.setPlacementGhost(ghost_visible, ghost_cell, ghost_valid, ui.getGhostQuarterTurns());
}

void Game::queueCommand(const GameCommand& command)
{
	command_queue.push_back(command);
}

void Game::applyQueuedCommands()
{
	// FIFO against the state left by the previous command; applied even while paused.
	std::vector<GameCommand> pending;
	pending.swap(command_queue);
	for (const GameCommand& command : pending)
	{
		const CommandResult result = simulation.apply(command);
		ui.showMessage(result.detail);
		std::cout << (result.accepted ? " + " : " - ") << result.detail << std::endl;
	}
}

// ---------------------------------------------------------------------------- speed, cancel, actions

void Game::setSpeedLevel(int level)
{
	speed_level = std::clamp(level, 0, max_speed_level);
	if (speed_level > 0)
		resume_speed_level = speed_level;
	simulation_clock.setPaused(speed_level == 0);
	simulation_clock.setTimeScale(static_cast<double>(std::max(speed_level, 1)));
}

void Game::togglePause()
{
	setSpeedLevel(speed_level == 0 ? resume_speed_level : 0);
}

void Game::cancelCurrent(bool from_escape)
{
	const ColonyUI::CancelResult result = ui.cancel();
	if (result == ColonyUI::CancelResult::Nothing && from_escape)
		ui.showMessage("Nothing to cancel. Close the window to quit (the menu arrives in P10).");
}

void Game::handleUiAction(UiAction action)
{
	switch (action)
	{
	case UiAction::TogglePause: togglePause(); break;
	case UiAction::SlowDown: setSpeedLevel(speed_level - 1); break;
	case UiAction::SpeedUp: setSpeedLevel(speed_level + 1); break;
	case UiAction::ToggleOverlay: colony_world.setDebugOverlayVisible(!colony_world.isDebugOverlayVisible()); break;
	case UiAction::ToggleBuildMode:   // ColonyUI already switched the mode
	case UiAction::CloseSelection:    // ColonyUI already cleared the selection
	case UiAction::None:
		break;
	}
}

ColonistId Game::getDebugColonist() const
{
	if (ui.getSelectedColonist() != INVALID_COLONIST)
		return ui.getSelectedColonist();
	const ScenarioDefinition& scenario = simulation.getScenario();
	if (scenario.debug_route)
		return scenario.debug_route->colonist_id;
	return simulation.getColonists().empty() ? INVALID_COLONIST : simulation.getColonists().front().id;
}

HudInfo Game::makeHudInfo() const
{
	HudInfo hud;
	hud.simulation_seconds = simulation.getSimulationTime();
	hud.paused = speed_level == 0;
	hud.speed_level = std::max(speed_level, 1);
	for (const ColonistRecord& colonist : simulation.getColonists())
		hud.colonists_alive += colonist.state != ColonistState::Dead ? 1 : 0;
	hud.has_door = simulation.getGrid().getDoorCount() > 0;
	hud.door_open = hud.has_door && simulation.getGrid().isDoorOpen(0);
	hud.fps = fps;
	hud.max_fps = max_fps;
	hud.debug_overlay = colony_world.isDebugOverlayVisible();
	hud.edge_scroll = edge_scroll_enabled;
	hud.navigation_revision = simulation.getGrid().getRevision();
	if (hud.debug_overlay)
		hud.gpu_stats = getGPUStats();
	hud.ghost_visible = ghost_visible;
	hud.ghost_cell = ghost_cell;
	hud.ghost_inside = ghost_visible && simulation.getGrid().getZone(ghost_cell) == Zone::Inside;
	hud.ghost_valid = ghost_valid;
	hud.ghost_status = ghost_status;
	return hud;
}

// ---------------------------------------------------------------------------- render

//what to do when the image has to be draw
void Game::render(void)
{
	// Set the clear color (the background color)
	glClearColor(0.0, 0.0, 0.0, 1.0);

	// Clear the window and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Set the camera as default
	camera->enable();

	// Set flags
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	const long draw_calls_before_scene = Mesh::num_meshes_rendered;
	colony_world.render(camera);
#ifndef NDEBUG
	if (!scene_stats_logged)
	{
		const AssetDefinition* terrain = asset_catalog.findAsset("terrain_tile");
		std::cout << " + Static scene: " << colony_world.getInstanceCount() << " instances, "
			<< colony_world.getStaticViewCount() << " asset/material groups ("
			<< colony_world.getInstancedViewCount() << " instanced); terrain="
			<< colony_world.getInstanceCountForAsset("terrain_tile") << " instances, "
			<< (terrain && terrain->mesh ? terrain->mesh->getNumDrawCalls() : 0)
			<< " mesh draw call(s); frame scene draw calls="
			<< (Mesh::num_meshes_rendered - draw_calls_before_scene) << std::endl;
		scene_stats_logged = true;
	}
#endif

	// HUD after the 3D scene.
	ui_renderer->render(ui, simulation, makeHudInfo());

	// Swap between front buffer and back buffer
	SDL_GL_SwapWindow(this->window);
}

// ---------------------------------------------------------------------------- events

//Keyboard event handler (sync input)
void Game::onKeyDown(SDL_KeyboardEvent event)
{
	if (event.repeat)
		return; // toggles react once per press
	switch (event.keysym.sym)
	{
	case SDLK_ESCAPE: cancelCurrent(true); break; // cancel/close; never quits (P10 adds the menu)
	case SDLK_b: ui.setBuildMode(!ui.isBuildMode()); break;
	case SDLK_r: if (ui.isBuildMode()) ui.rotateGhost(); break;
	case SDLK_SPACE: togglePause(); break;
	case SDLK_MINUS: case SDLK_KP_MINUS: setSpeedLevel(speed_level - 1); break;
	case SDLK_PLUS: case SDLK_EQUALS: case SDLK_KP_PLUS: setSpeedLevel(speed_level + 1); break;
		// P03 navigation debug keys (the GDD controls leave F3-F8 free); gameplay
		// changes go through the command queue like any player order.
	case SDLK_F3: handleUiAction(UiAction::ToggleOverlay); break;
	case SDLK_F4: queueCommand(GameCommand::placeDebugObstacleAhead(getDebugColonist())); break;
	case SDLK_F6: queueCommand(GameCommand::clearDebugObstacles()); break;
	case SDLK_F7:
		if (simulation.getGrid().getDoorCount() > 0)
			queueCommand(GameCommand::setDoorOpen(0, !simulation.getGrid().isDoorOpen(0)));
		break;
	case SDLK_F8:
		edge_scroll_enabled = !edge_scroll_enabled;
		ui.showMessage(edge_scroll_enabled ? "Edge scrolling on" : "Edge scrolling off");
		break;
#ifndef NDEBUG
	case SDLK_F5: Shader::ReloadAll(); break;
#endif
	}
}

void Game::onKeyUp(SDL_KeyboardEvent event)
{

}

void Game::onMouseButtonDown(SDL_MouseButtonEvent event)
{
	const float x = static_cast<float>(event.x);
	const float y = static_cast<float>(event.y);
	if (event.button == SDL_BUTTON_LEFT)
	{
		const UiPointerResult result = ui.handlePointer(PointerEvent{ PointerType::Down, PointerButton::Left, x, y });
		handleUiAction(result.action);
		world_press_active = !result.consumed;
		world_press_position.set(x, y);
	}
	else if (event.button == SDL_BUTTON_MIDDLE)
	{
		// Middle-drag rotates the camera, unless the press starts on the UI.
		middle_dragging = !ui.handlePointer(PointerEvent{ PointerType::Down, PointerButton::Middle, x, y }).consumed;
	}
	else if (event.button == SDL_BUTTON_RIGHT)
	{
		// GDD 3.3: right click cancels construction or closes an open menu.
		ui.handlePointer(PointerEvent{ PointerType::Down, PointerButton::Right, x, y });
		cancelCurrent(false);
	}
}

void Game::onMouseButtonUp(SDL_MouseButtonEvent event)
{
	const float x = static_cast<float>(event.x);
	const float y = static_cast<float>(event.y);
	if (event.button == SDL_BUTTON_LEFT)
	{
		const UiPointerResult result = ui.handlePointer(PointerEvent{ PointerType::Up, PointerButton::Left, x, y });
		handleUiAction(result.action);
		const float dx = x - world_press_position.x;
		const float dy = y - world_press_position.y;
		if (world_press_active && !result.consumed && dx * dx + dy * dy <= click_slop_pixels * click_slop_pixels)
		{
			pending_world_click = true; // resolved in update() after the camera moves
			pending_click_position.set(x, y);
		}
		world_press_active = false;
	}
	else if (event.button == SDL_BUTTON_MIDDLE)
	{
		middle_dragging = false;
	}
}

void Game::onMouseWheel(SDL_MouseWheelEvent event)
{
	if (event.y == 0)
		return;
	if (ui.isPointerOverUi(Input::mouse_position.x, Input::mouse_position.y))
		return;
	pending_zoom_notches += static_cast<float>(event.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.y : event.y);
}

void Game::onGamepadButtonDown(SDL_JoyButtonEvent event)
{

}

void Game::onGamepadButtonUp(SDL_JoyButtonEvent event)
{

}

void Game::onResize(int width, int height)
{
	std::cout << "window resized: " << width << "," << height << std::endl;
	updateViewport(width, height);
}

void Game::setMouseLocked(bool must_lock)
{
	// Kept for the framework interface. ARKFALL needs a free cursor for selection and
	// the UI, so locking is refused (plan P04).
	(void)must_lock;
	mouse_locked = false;
	SDL_ShowCursor(SDL_ENABLE);
	SDL_SetRelativeMouseMode(SDL_FALSE);
}