#include "game.h"
#include "framework/utils.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "framework/input.h"
#include "scene_parser.h"
#include "scenario_loader.h"

#include <cmath>
#include <set>
#include <stdexcept>

namespace
{
	const float camera_pan_speed = 30.0f;
	const float camera_reference_pan_distance = 30.0f;
	const float camera_zoom_step = 0.9f;
	const float camera_min_distance = 0.0f;
	const float camera_max_distance = 120.0f;
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
	, must_exit(false)
	, camera(nullptr)
	, mouse_locked(false)
	, asset_catalog()
	, colony_world(asset_catalog)
	, simulation_clock()
	, colonist_catalog()
	, simulation()
	, overlay_revision(0)
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

	ColonistCatalogLoadResult colonist_result = colonist_catalog.load("data/arkfall/colonists.json");
	if (!colonist_result)
		throw std::runtime_error(colonist_result.error->describe());
	colonist_result = colonist_catalog.validateVisuals(asset_catalog);
	if (!colonist_result)
		throw std::runtime_error(colonist_result.error->describe());

	// Only assets referenced by data are loaded: the static scene plus the role visuals
	// used for dynamic colonist views.
	std::set<std::string> referenced_assets;
	for (const StaticSceneInstance& instance : scene_result.instances)
		referenced_assets.insert(instance.asset_id);
	for (const auto& [job, role] : colonist_catalog.getRoles())
	{
		(void)job;
		referenced_assets.insert(role.asset_id);
	}

	// Validate all authored JSON and references before creating any OpenGL assets.
	AssetCatalogLoadResult resource_result = asset_catalog.loadReferencedAssets(referenced_assets);
	if (!resource_result)
		throw std::runtime_error(resource_result.error->describe());

	DataLoadError scene_build_error;
	if (!colony_world.buildStaticViews(scene_result, scene_build_error))
		throw std::runtime_error(scene_build_error.describe());

	std::string simulation_error;
	if (!simulation.initialize(scenario_result.scenario, colonist_catalog, simulation_error))
		throw std::runtime_error("scenario.json: " + simulation_error);
	spawnColonistViews();
	syncColonistViews(0.0f);
	colony_world.setDebugOverlayVisible(true);
	rebuildNavigationOverlay();

	std::cout << " + P03 debug keys: F3 overlay, F4 obstacle ahead of the colonist, F6 clear obstacles, F7 open/close Exit Door" << std::endl;

	// OpenGL flags
	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);

	// Create our camera after fallible resource loading so construction failures need no GL cleanup here.
	camera = new Camera();
	camera->lookAt(Vector3(0.f, 18.f, 24.f), Vector3(0.f, 0.f, 0.f), Vector3(0.f, 1.f, 0.f));
	camera->setPerspective(48.f, window_width / (float)window_height, 0.1f, 1000.f);

	// Hide the cursor
	SDL_ShowCursor(!mouse_locked); //hide or show the mouse
	instance = this;
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

	// Render the FPS, Draw Calls, etc
	drawText(2, 2, getGPUStats(), Vector3(1, 1, 1), 2);

	// Swap between front buffer and back buffer
	SDL_GL_SwapWindow(this->window);
}

void Game::update(double seconds_elapsed)
{
	// Camera and (later) UI use real frame time and keep working while paused.
	updateCamera(seconds_elapsed);

	// Gameplay advances only in fixed steps (plan C4).
	const int steps = simulation_clock.advance(seconds_elapsed);
	for (int i = 0; i < steps; ++i)
		fixedUpdate(simulation_clock.getStepSeconds());

	// Views follow the simulation once per frame, interpolated between fixed steps.
	const float alpha = static_cast<float>(simulation_clock.getInterpolationAlpha());
	syncColonistViews(alpha);
	if (overlay_revision != simulation.getGrid().getRevision())
		rebuildNavigationOverlay();
	updatePathOverlay(alpha);
}

void Game::fixedUpdate(double fixed_dt)
{
	// Gameplay timing uses simulation time, never the wall-clock Game::time.
	simulation.tick(static_cast<float>(fixed_dt));
}

void Game::spawnColonistViews()
{
	for (const auto& [id, view] : colonist_views)
	{
		(void)id;
		colony_world.destroyDynamicView(view);
	}
	colonist_views.clear();
	for (const ColonistRecord& colonist : simulation.getColonists())
	{
		const ColonistRoleDefinition* role = colonist_catalog.findRole(colonist.job);
		if (!role)
			throw std::runtime_error("colonist " + std::to_string(colonist.id) + " has no role visual");
		DataLoadError error;
		const DynamicViewId view = colony_world.createDynamicView(role->asset_id, role->material_id, error);
		if (view == invalid_dynamic_view)
			throw std::runtime_error(error.describe());
		colonist_views[colonist.id] = view;
	}
}

void Game::syncColonistViews(float alpha)
{
	for (const ColonistRecord& colonist : simulation.getColonists())
	{
		const auto it = colonist_views.find(colonist.id);
		if (it == colonist_views.end())
			continue;
		const Vector3 position(lerp(colonist.previous_position.x, colonist.position.x, alpha), 0.0f,
			lerp(colonist.previous_position.z, colonist.position.z, alpha));
		const float yaw = lerpAngleRad(colonist.previous_yaw, colonist.yaw, alpha);
		colony_world.setDynamicViewPose(it->second, position, yaw);
	}
}

void Game::rebuildNavigationOverlay()
{
	const NavigationGrid& grid = simulation.getGrid();
	overlay_revision = grid.getRevision();
	const float half = grid.getCellSize() * 0.45f;
	auto addCellQuad = [&](std::vector<Vector3>& out, Cell cell) {
		const Vector3 c = grid.cellCenter(cell);
		const float y = colony_world.getVisualGroundHeight(c.x, c.z) + 0.03f;
		const Vector3 a(c.x - half, y, c.z - half), b(c.x + half, y, c.z - half);
		const Vector3 d(c.x - half, y, c.z + half), e(c.x + half, y, c.z + half);
		out.insert(out.end(), { a, b, e, a, e, d });
	};

	std::vector<Vector3> blocked;
	for (int z = 0; z < grid.getHeight(); ++z)
		for (int x = 0; x < grid.getWidth(); ++x)
			if (grid.isBlocked(Cell{ x, z }))
				addCellQuad(blocked, Cell{ x, z });
	colony_world.setDebugLayer("blocked", "debug_blocked", ColonyWorld::DebugPrimitive::Triangles, blocked);

	std::vector<Vector3> open_doors, closed_doors;
	for (std::size_t door = 0; door < grid.getDoorCount(); ++door)
		for (const DoorPortal& portal : grid.getDoorPortals(door))
		{
			std::vector<Vector3>& out = grid.isDoorOpen(door) ? open_doors : closed_doors;
			addCellQuad(out, portal.outside);
			addCellQuad(out, portal.inside);
		}
	colony_world.setDebugLayer("door_open", "debug_door_open", ColonyWorld::DebugPrimitive::Triangles, open_doors);
	colony_world.setDebugLayer("door_closed", "debug_door_closed", ColonyWorld::DebugPrimitive::Triangles, closed_doors);
}

void Game::updatePathOverlay(float alpha)
{
	const NavigationGrid& grid = simulation.getGrid();
	std::vector<Vector3> lines;
	for (const ColonistRecord& colonist : simulation.getColonists())
	{
		if (colonist.route.finished())
			continue;
		Vector3 from(lerp(colonist.previous_position.x, colonist.position.x, alpha), 0.0f,
			lerp(colonist.previous_position.z, colonist.position.z, alpha));
		from.y = colony_world.getVisualGroundHeight(from.x, from.z) + 0.15f;
		for (std::size_t i = colonist.route.next_index; i < colonist.route.cells.size(); ++i)
		{
			Vector3 to = grid.cellCenter(colonist.route.cells[i]);
			to.y = colony_world.getVisualGroundHeight(to.x, to.z) + 0.15f;
			lines.push_back(from);
			lines.push_back(to);
			from = to;
		}
	}
	colony_world.setDebugLayer("path", "debug_path", ColonyWorld::DebugPrimitive::Lines, lines);
}

ColonistId Game::getDebugColonist() const
{
	const ScenarioDefinition& scenario = simulation.getScenario();
	if (scenario.debug_route)
		return scenario.debug_route->colonist_id;
	return simulation.getColonists().empty() ? INVALID_COLONIST : simulation.getColonists().front().id;
}

void Game::updateCamera(double seconds_elapsed)
{
	const float camera_distance = static_cast<float>((camera->eye - camera->center).length());
	const float zoom_speed_scale = camera_distance / camera_reference_pan_distance;
	float speed = static_cast<float>(seconds_elapsed) * camera_pan_speed * zoom_speed_scale;

	// Mouse input to rotate the cam
	if (Input::isMousePressed(SDL_BUTTON_LEFT) || mouse_locked) //is left button pressed?
	{
		camera->rotate(-Input::mouse_delta.x * 0.005f, Vector3(0.0f,1.0f,0.0f));
		camera->rotate(-Input::mouse_delta.y * 0.005f, camera->getLocalVector( Vector3(1.0f,0.0f,0.0f)));
	}

	// Pan on the map plane. Projecting the camera's forward direction onto XZ
	// prevents W/S from climbing or descending when the camera looks downward.
	Vector3 forward = camera->center - camera->eye;
	forward.y = 0.0f;
	if (forward.length() < 0.0001f)
		forward.set(0.0f, 0.0f, -1.0f);
	else
		forward.normalize();
	const Vector3 right = forward.cross(Vector3(0.0f, 1.0f, 0.0f)).normalize();

	Vector3 pan_direction(0.0f, 0.0f, 0.0f);
	if (Input::isKeyPressed(SDL_SCANCODE_W) || Input::isKeyPressed(SDL_SCANCODE_UP)) pan_direction += forward;
	if (Input::isKeyPressed(SDL_SCANCODE_S) || Input::isKeyPressed(SDL_SCANCODE_DOWN)) pan_direction -= forward;
	if (Input::isKeyPressed(SDL_SCANCODE_A) || Input::isKeyPressed(SDL_SCANCODE_LEFT)) pan_direction -= right;
	if (Input::isKeyPressed(SDL_SCANCODE_D) || Input::isKeyPressed(SDL_SCANCODE_RIGHT)) pan_direction += right;

	if (pan_direction.length() > 0.0f)
	{
		if (pan_direction.length() > 1.0f)
			pan_direction.normalize();
		pan_direction *= speed;
		camera->eye += pan_direction;
		camera->center += pan_direction;
		camera->updateViewMatrix();
	}
}

//Keyboard event handler (sync input)
void Game::onKeyDown( SDL_KeyboardEvent event )
{
	switch(event.keysym.sym)
	{
		case SDLK_ESCAPE: must_exit = true; break; //ESC key, kill the app
		// P03 navigation debug keys (the GDD controls leave F3-F7 free).
		case SDLK_F3:
			colony_world.setDebugOverlayVisible(!colony_world.isDebugOverlayVisible());
			break;
		case SDLK_F4:
		{
			Cell cell;
			std::string reason;
			if (simulation.placeDebugObstacleAhead(getDebugColonist(), cell, reason))
				std::cout << " + Debug obstacle at (" << cell.x << ", " << cell.z << "): the colonist must detour" << std::endl;
			else
				std::cout << " - No debug obstacle placed: " << reason << std::endl;
			break;
		}
		case SDLK_F6:
			simulation.clearDebugObstacles();
			std::cout << " + Debug obstacles cleared" << std::endl;
			break;
		case SDLK_F7:
			if (simulation.getGrid().getDoorCount() > 0)
			{
				const bool open = !simulation.getGrid().isDoorOpen(0);
				simulation.setDoorOpen(0, open);
				std::cout << " + Exit Door " << (open ? "opened" : "closed (the boundary is sealed)") << std::endl;
			}
			break;
		#ifndef NDEBUG
		case SDLK_F5: Shader::ReloadAll(); break;
		#endif
	}
}

void Game::onKeyUp(SDL_KeyboardEvent event)
{

}

void Game::onMouseButtonDown( SDL_MouseButtonEvent event )
{
	if (event.button == SDL_BUTTON_MIDDLE) //middle mouse
	{
		mouse_locked = !mouse_locked;
		SDL_ShowCursor(!mouse_locked);
		SDL_SetRelativeMouseMode((SDL_bool)(mouse_locked));
	}
}

void Game::onMouseButtonUp(SDL_MouseButtonEvent event)
{

}

void Game::onMouseWheel(SDL_MouseWheelEvent event)
{
	if (!camera || event.y == 0)
		return;

	const float wheel_delta = static_cast<float>(event.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.y : event.y);
	const Vector3 eye_offset = camera->eye - camera->center;
	const float distance = static_cast<float>(eye_offset.length());
	if (distance <= 0.0001f)
		return;

	float zoomed_distance = distance * std::pow(camera_zoom_step, wheel_delta);
	if (zoomed_distance < camera_min_distance)
		zoomed_distance = camera_min_distance;
	else if (zoomed_distance > camera_max_distance)
		zoomed_distance = camera_max_distance;

	const Vector3 direction = eye_offset / distance;
	camera->eye = camera->center + direction * zoomed_distance;
	camera->updateViewMatrix();
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
	glViewport( 0,0, width, height );
	camera->aspect =  width / (float)height;
	window_width = width;
	window_height = height;
}

void Game::setMouseLocked(bool must_lock)
{
	SDL_ShowCursor(!must_lock);

	SDL_SetRelativeMouseMode((SDL_bool)must_lock);

	mouse_locked = must_lock;
}
