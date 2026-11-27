#pragma once

// Strict loader for data/arkfall/scenario.json (schema 1), headless.
//
// scenario.json owns the logical map and the start state: grid, deposits, static
// blockers, initial colonists, and from P05 the initial inventory, the temporary cargo
// capacity and the initial (complete) buildings; plus an optional scripted debug route
// (dev scenarios). Since P05B there is no predefined base: every cell starts outside
// and the base is made of Exit Doors and corridors (initialBuildings, plan C5). It never
// contains visuals; scene.json does, and validateSceneBlockers() checks that both
// agree. Errors report file, JSON pointer and message; a scenario that fails validation
// is never partially used. Building TYPES, placement rules and debug-route
// reachability are checked when ColonySimulation::initialize places the buildings.

#include "arkfall_types.h"
#include "data_load_error.h"
#include "json_data.h"
#include "navigation_grid.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct StaticSceneInstance;

struct GridDefinition
{
	int width = 0;
	int height = 0;
	float cell_size = 1.0f;
	float origin_x = 0.0f;
	float origin_z = 0.0f;
};

struct StaticBlockerDefinition
{
	std::string id;
	Cell min;   // inclusive
	Cell max;   // inclusive
};

struct NeedValues
{
	float hunger = 100.0f;
	float thirst = 100.0f;
	float oxygen = 100.0f;
};

struct InitialColonistDefinition
{
	ColonistId id = INVALID_COLONIST;
	Job job = Job::Engineer;
	Cell cell;
	NeedValues needs;
};

struct RouteStopDefinition
{
	std::string label;
	Cell cell;
};

// A building that exists complete at the start (dev/test scenarios; the shipped start
// has none). Most give origin + quarter turns; a corridor strip gives `from` and `to`:
// the inclusive rectangle between them, straight and exactly 2 cells wide, expanded by
// ColonySimulation into one Corridor per cell, row by row from `from` (P05B).
// type_id is a buildings.json id.
struct InitialBuildingDefinition
{
	std::string type_id;
	Cell origin;
	int quarter_turns = 0;
	bool is_strip = false;
	Cell strip_from;
	Cell strip_to;
};

// Cells of a corridor strip in placement order: rows across the 2-cell width, starting
// at `from` and moving towards `to` along the strip.
std::vector<Cell> corridorStripCells(Cell from, Cell to);

// Temporary P03 scripted route: the colonist walks stop to stop (looping if asked),
// waiting dwell_seconds at each stop. Replaced by real job selection in P05.
struct DebugRouteDefinition
{
	ColonistId colonist_id = INVALID_COLONIST;
	bool loop = false;
	float dwell_seconds = 0.0f;
	std::vector<RouteStopDefinition> stops;
};

struct ScenarioDefinition
{
	GridDefinition grid;
	std::vector<DepositDefinition> deposits;
	std::vector<StaticBlockerDefinition> static_blockers;
	std::vector<InitialColonistDefinition> initial_colonists;
	ResourceAmounts initial_inventory;
	int cargo_capacity = 0;    // item capacity until the first Storage completes
	std::vector<InitialBuildingDefinition> initial_buildings;
	std::optional<DebugRouteDefinition> debug_route;
};

struct ScenarioLoadResult
{
	ScenarioDefinition scenario;
	std::optional<DataLoadError> error;
	explicit operator bool() const { return !error.has_value(); }
};

class ScenarioLoader
{
public:
	static constexpr int supported_schema_version = 1;
	static constexpr int max_grid_size = 256;
	static constexpr int max_item_amount = 100000;

	ScenarioLoadResult load(const std::filesystem::path& path) const;
	// Parses an already loaded document; `path` only labels errors (used by tests).
	ScenarioLoadResult parse(const json_data::Json& document, const std::filesystem::path& path) const;

	// Every scene instance with blocksNavigation must stand on a deposit/blocker cell,
	// and every deposit/blocker must have at least one blocking visual (no invisible
	// blockers). Visual-only instances (walls, floors) are ignored.
	static bool validateSceneBlockers(const ScenarioDefinition& scenario,
		const std::vector<StaticSceneInstance>& scene_instances,
		const std::filesystem::path& scene_path, DataLoadError& error);
};

// Builds the navigation grid described by a validated scenario: an all-outside grid
// with the deposit cells and static blockers. Inside cells and doors come from the
// buildings (ColonySimulation).
NavigationGrid buildNavigationGrid(const ScenarioDefinition& scenario);
