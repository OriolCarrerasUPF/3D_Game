/*  by Javi Agenjo 2013 UPF  javi.agenjo@gmail.com
	This class encapsulates the game, is in charge of creating the game, getting the user input, process the update and render.
*/

#pragma once

#include "framework/includes.h"
#include "framework/camera.h"
#include "framework/utils.h"
#include "asset_catalog.h"
#include "colony_world.h"
#include "fixed_step_clock.h"
#include "colonist_catalog.h"
#include "colony_simulation.h"

#include <cstdint>
#include <map>

class Game
{
public:
	static Game* instance;

	//window
	SDL_Window* window;
	int window_width;
	int window_height;

	//some globals
	long frame;
    float time;
	float elapsed_time;
	int fps;
	bool must_exit;

	//some vars
	Camera* camera; //our global camera
	bool mouse_locked; //tells if the mouse is locked (not seen)

	AssetCatalog asset_catalog;
	ColonyWorld colony_world;
	// Fixed 1/30 s simulation steps; frame time clamped to 0.25 s, max 8 steps per frame.
	FixedStepClock simulation_clock;
	ColonistCatalog colonist_catalog;
	ColonySimulation simulation;
	std::map<ColonistId, DynamicViewId> colonist_views;
	std::uint64_t overlay_revision;
	bool scene_stats_logged;

	Game( int window_width, int window_height, SDL_Window* window );
	~Game();

	//main functions
	void render( void );
	// Called once per frame with real (wall-clock) frame time.
	void update( double dt );
	// Called 0..8 times per frame with the fixed simulation step (seconds).
	void fixedUpdate( double fixed_dt );

	void setMouseLocked(bool must_lock);

	//events
	void onKeyDown( SDL_KeyboardEvent event );
	void onKeyUp(SDL_KeyboardEvent event);
	void onMouseButtonDown( SDL_MouseButtonEvent event );
	void onMouseButtonUp(SDL_MouseButtonEvent event);
	void onMouseWheel(SDL_MouseWheelEvent event);
	void onGamepadButtonDown(SDL_JoyButtonEvent event);
	void onGamepadButtonUp(SDL_JoyButtonEvent event);
	void onResize(int width, int height);

private:
	void updateCamera(double seconds_elapsed);
	void spawnColonistViews();
	void syncColonistViews(float alpha);
	void rebuildNavigationOverlay();
	void updatePathOverlay(float alpha);
	ColonistId getDebugColonist() const;
};
