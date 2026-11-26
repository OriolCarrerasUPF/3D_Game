/*  by Javi Agenjo 2013 UPF  javi.agenjo@gmail.com
	This class encapsulates the game, is in charge of creating the game, getting the user input, process the update and render.
	ARKFALL: Game is the application coordinator (plan C1). It owns the data catalogs,
	the headless ColonySimulation, the ColonyWorld views, the strategy camera and the UI,
	routes input UI-first, applies queued commands once per frame and advances the
	simulation in fixed steps.
*/

#pragma once

#include "framework/includes.h"
#include "framework/camera.h"
#include "framework/utils.h"
#include "asset_catalog.h"
#include "colony_simulation.h"
#include "colony_ui.h"
#include "colony_world.h"
#include "fixed_step_clock.h"
#include "game_definitions.h"
#include "picking.h"
#include "strategy_camera.h"

#include <memory>
#include <string>
#include <vector>

class ColonyUIRenderer;
struct HudInfo;

class Game
{
public:
	static Game* instance;

	//window
	SDL_Window* window;
	int window_width;   // logical (SDL window) size: UI layout, mouse and picking
	int window_height;

	//some globals
	long frame;
	float time;
	float elapsed_time;
	int fps;
	int max_fps;   // FPS cap applied by mainLoop(); 0 or negative = uncapped
	bool must_exit;

	//some vars
	Camera* camera; //our global camera
	bool mouse_locked; // framework field; ARKFALL keeps the cursor free (always false)

	AssetCatalog asset_catalog;
	GameDefinitions definitions;
	ColonyWorld colony_world;
	// Fixed 1/30 s simulation steps; frame time clamped to 0.25 s, max 8 steps per frame.
	FixedStepClock simulation_clock;
	ColonySimulation simulation;
	StrategyCamera strategy_camera;
	ColonyUI ui;
	std::unique_ptr<ColonyUIRenderer> ui_renderer;
	bool scene_stats_logged;

	// scenario_path: scenario.json to start (main.cpp: --scenario <path>, P05). The
	// default is the shipped start; dev/test scenarios live in data/arkfall/dev/.
	static constexpr const char* default_scenario_path = "data/arkfall/scenario.json";
	Game(int window_width, int window_height, SDL_Window* window, const std::string& scenario_path = default_scenario_path);
	~Game();

	//main functions
	void render(void);
	// Called once per frame with real (wall-clock) frame time.
	void update(double dt);
	// Called 0..8 times per frame with the fixed simulation step (seconds).
	void fixedUpdate(double fixed_dt);

	void setMouseLocked(bool must_lock);

	//events
	void onKeyDown(SDL_KeyboardEvent event);
	void onKeyUp(SDL_KeyboardEvent event);
	void onMouseButtonDown(SDL_MouseButtonEvent event);
	void onMouseButtonUp(SDL_MouseButtonEvent event);
	void onMouseWheel(SDL_MouseWheelEvent event);
	void onGamepadButtonDown(SDL_JoyButtonEvent event);
	void onGamepadButtonUp(SDL_JoyButtonEvent event);
	void onResize(int width, int height);

private:
	static constexpr int max_speed_level = 3;

	void updateViewport(int logical_width, int logical_height);
	void frameInitialView();
	void updateCamera(float real_dt);
	void applyCameraToFramework();
	picking::Ray makePointerRay(float x, float y) const;
	void processWorldClick();
	void updatePlacementGhost();
	void queueCommand(const GameCommand& command);
	void applyQueuedCommands();
	void handleUiAction(UiAction action);
	void cancelCurrent(bool from_escape);
	void setSpeedLevel(int level);
	void togglePause();
	bool isDoorCell(Cell cell) const;
	ColonistId getDebugColonist() const;
	HudInfo makeHudInfo() const;

	std::vector<GameCommand> command_queue;
	int speed_level = 1;          // 0 = paused, 1..3 = game speed
	int resume_speed_level = 1;   // speed restored when unpausing
	bool edge_scroll_enabled = false;
	bool middle_dragging = false;
	bool world_press_active = false;
	Vector2 world_press_position;
	bool pending_world_click = false;
	Vector2 pending_click_position;
	float pending_zoom_notches = 0.0f;
	// Build-mode ghost facts for the HUD.
	bool ghost_visible = false;
	Cell ghost_cell;
	bool ghost_valid = false;
	std::string ghost_status;
};