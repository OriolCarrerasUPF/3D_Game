#pragma once

// Strict loader for data/arkfall/scenario.json (schema 1), headless.
//
// scenario.json owns the logical map and the start state: grid, base interior, Exit
// Door crossings, deposits, static blockers, initial colonists, and from P05 the
// initial inventory, the temporary cargo capacity and the initial (complete)
// buildings; plus (temporarily, P03) a scripted debug route. It never contains
// visuals; scene.json does, and validateSceneBlockers() checks that both agree.
// Errors report file, JSON pointer and message; a scenario that fails validation is
// never partially used. Building TYPES and placement rules are checked against
// buildings.json when ColonySimulation::initialize places the initial buildings.

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

struct ExitDoorDefinition
{
	std::string id;
	bool open_at_start = false;
	std::vector<DoorPortal> crossings;
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
// has none). Footprint buildings give origin + quarter turns; an Exit Door gives the
// door socket (scenario exitDoors id) it is built on. type_id is a buildings.json id.
struct InitialBuildingDefinition
{
	std::string type_id;
	bool on_socket = false;
	Cell origin;
	int quarter_turns = 0;
	std::string socket_id;
};

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
	Cell interior_min;
	Cell interior_max;
	std::vector<ExitDoorDefinition> exit_doors;
	std::vector<DepositDefinition> deposits;
	std::vector<StaticBlockerDefinition> static_blockers;
	std::vector<InitialColonistDefinition> initial_colonists;
	ResourceAmounts initial_inventory;
	int cargo_capacity = 0;    // item capacity until the first Storage completes
	std::vector<InitialBuildingDefinition> initial_buildings;
	std::optional<DebugRouteDefinition> debug_route;

	bool isInterior(Cell cell) const
	{
		return cell.x >= interior_min.x && cell.x <= interior_max.x &&
			cell.z >= interior_min.z && cell.z <= interior_max.z;
	}
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

// Builds the navigation grid described by a validated scenario: zones, doors (with
// their initial open state), deposit cells and static blockers.
NavigationGrid buildNavigationGrid(const ScenarioDefinition& scenario);
