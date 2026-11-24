#pragma once

// Gameplay state and deterministic fixed-step updates (plan C1/C4), headless.
// No OpenGL, SDL or resource loading: Game calls tick() from fixedUpdate() and the
// view layer only reads the records below.
//
// P03 scope: one navigation grid built from scenario.json and the initial colonists.
// A colonist with a debugRoute walks stop -> stop (route found by BFS, so it uses
// the Exit Door and detours around blockers), waits dwellSeconds at each stop and
// loops. Movement uses the plan's distance budget, snaps exactly to waypoints and
// never overshoots. Jobs, needs and buildings arrive in P05-P07.
//
// Theory reuse (T13): state machine with explicit states and transitions; grid
// pathfinding. ColonistState keeps the full C6.1 list; P03 uses Idle/MovingToWork.

#include "arkfall_types.h"
#include "colonist_catalog.h"
#include "navigation_grid.h"
#include "scenario_loader.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

struct ColonistRecord
{
	ColonistId id = INVALID_COLONIST;
	Job job = Job::Engineer;
	ColonistState state = ColonistState::Idle;
	NeedValues needs;
	float move_speed = 0.0f;

	// Gameplay position on the XZ plane (y = 0) and facing (world_transform yaw).
	Vector3 position;
	float yaw = 0.0f;
	// State at the start of the last tick, for render interpolation.
	Vector3 previous_position;
	float previous_yaw = 0.0f;

	Cell last_cell;                  // last waypoint reached (always a walkable cell)
	Route route;                     // route.cells[0] == last_cell when planned
	Cell route_goal;
	std::uint64_t route_revision = 0; // grid revision the route was validated against

	// Temporary P03 scripted route.
	bool has_debug_route = false;
	bool debug_route_done = false;
	std::size_t stop_index = 0;       // stop currently travelled to / waited at
	float wait_timer = 0.0f;          // dwell time left at the current stop
	float retry_timer = 0.0f;         // time until a failed route is retried
	std::uint64_t failed_revision = 0;
	bool route_failed = false;

	std::string status;               // human readable, for debug output / later UI
};

class ColonySimulation
{
public:
	static constexpr float route_retry_seconds = 2.0f; // plan C6.1

	// Builds the grid and colonists. Fails (and leaves the simulation empty) if a
	// colonist's job has no role definition.
	bool initialize(const ScenarioDefinition& scenario, const ColonistCatalog& roles, std::string& error);
	void reset();

	// One fixed simulation step (seconds).
	void tick(float dt);

	const NavigationGrid& getGrid() const { return grid; }
	const ScenarioDefinition& getScenario() const { return scenario; }
	const std::vector<ColonistRecord>& getColonists() const { return colonists; }
	const ColonistRecord* findColonist(ColonistId id) const;
	double getSimulationTime() const { return simulation_time; }
	bool isInitialized() const { return initialized; }

	// ---- P03 debug commands (applied immediately, between ticks) -----------
	// Blocks a free cell a few steps ahead on the colonist's current route to show the
	// detour. Never blocks door cells, route goals or cells a colonist stands on/enters.
	bool placeDebugObstacleAhead(ColonistId id, Cell& out_cell, std::string& reason);
	// Blocks/unblocks one specific cell (only cells not blocked by scenario data).
	bool toggleDebugObstacle(Cell cell, std::string& reason);
	void clearDebugObstacles();
	const std::set<std::pair<int, int>>& getDebugObstacles() const { return debug_obstacles; }
	bool setDoorOpen(std::size_t door_index, bool open);

private:
	void updateColonist(ColonistRecord& colonist, float dt);
	bool planRoute(ColonistRecord& colonist, Cell goal);
	bool isRemainingRouteValid(const ColonistRecord& colonist) const;
	void advanceAlongRoute(ColonistRecord& colonist, float distance_budget);
	void onRouteFinished(ColonistRecord& colonist);
	bool isCellReservedByColonist(Cell cell) const;
	bool isDoorCell(Cell cell) const;

	ScenarioDefinition scenario;
	NavigationGrid grid;
	std::vector<ColonistRecord> colonists;
	std::set<std::pair<int, int>> debug_obstacles;
	double simulation_time = 0.0;
	bool initialized = false;
	std::vector<Cell> path_scratch;
};
