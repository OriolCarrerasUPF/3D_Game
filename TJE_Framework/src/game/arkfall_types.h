#pragma once

// Shared gameplay types for ARKFALL.
// Standard C++ only: no OpenGL, SDL or framework includes, so the simulation
// and tests can use it without a window.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// IDs
// Stable integer IDs instead of pointers (plan C2). Zero means "none/invalid".
// ---------------------------------------------------------------------------
using ColonistId = std::uint32_t;
using BuildingId = std::uint32_t;

constexpr ColonistId INVALID_COLONIST = 0;
constexpr BuildingId INVALID_BUILDING = 0;

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

// One 1 m navigation cell. x and z are grid indices in [0, width) / [0, height),
// not world coordinates. NavigationGrid converts between the two.
struct Cell
{
	int x = 0;
	int z = 0;

	bool operator==(const Cell& other) const { return x == other.x && z == other.z; }
	bool operator!=(const Cell& other) const { return !(*this == other); }
};

// Which side of the base wall a cell is on. A step between two cells in
// different zones is only allowed through a DoorPortal.
enum class Zone : std::uint8_t
{
	Outside,
	Inside
};

// One allowed crossing through the base wall: two neighbouring cells,
// one in each zone. A wider door is simply several portals.
struct DoorPortal
{
	Cell outside;
	Cell inside;
};

// ---------------------------------------------------------------------------
// World content loaded from scenario.json
// ---------------------------------------------------------------------------

enum class DepositType : std::uint8_t
{
	Iron,
	Copper
};

struct DepositDefinition
{
	std::string id;
	DepositType type = DepositType::Iron;
	Cell cell;   // anchor cell; also blocks navigation
};

// ---------------------------------------------------------------------------
// Stored items (plan C2/C7). Six integer item types share one capacity; water,
// power and oxygen are flows/capacities, never items. JSON keys: ironOre,
// copperOre, steel, components, vegetables, aurite.
// ---------------------------------------------------------------------------

enum class ResourceType : std::uint8_t
{
	IronOre,
	CopperOre,
	Steel,
	Components,
	Vegetables,
	Aurite
};

constexpr std::size_t resource_type_count = 6;

// Integer amount per item type (costs, recipe inputs/outputs, inventory contents).
struct ResourceAmounts
{
	std::array<int, resource_type_count> values{};

	int& operator[](ResourceType type) { return values[static_cast<std::size_t>(type)]; }
	int operator[](ResourceType type) const { return values[static_cast<std::size_t>(type)]; }
	int total() const
	{
		int sum = 0;
		for (int value : values)
			sum += value;
		return sum;
	}
	bool empty() const { return total() == 0; }
	bool operator==(const ResourceAmounts& other) const { return values == other.values; }
	bool operator!=(const ResourceAmounts& other) const { return values != other.values; }
};

// ---------------------------------------------------------------------------
// Buildings (plan C2/C8). The enum only names the 13 MVP types plus the Corridor
// (base layout change, P05B); every value (footprint, cost, slots, recipe, visuals
// ...) comes from buildings.json.
// ---------------------------------------------------------------------------

enum class BuildingType : std::uint8_t
{
	ExitDoor,
	Storage,
	Oxygenator,
	Greenhouse,
	IronProcessor,
	CopperProcessor,
	AuriteProcessor,
	IronMine,
	CopperMine,
	WaterExtractor,
	SolarPanel,
	LandingPad,
	HavenBeacon,
	Corridor
};

constexpr std::size_t building_type_count = 14;

enum class ConstructionState : std::uint8_t
{
	Blueprint,   // P07: paid and placed, waiting for an Engineer
	Complete
};

// ---------------------------------------------------------------------------
// Colonists
// ---------------------------------------------------------------------------

// Colonist role (plan C2). Loaded from scenario.json / colonists.json by name.
enum class Job : std::uint8_t
{
	Engineer,
	Miner,
	Farmer
};

// Full state list from plan C6.1. P05 uses Idle, MovingToWork and Working; the
// service states arrive with needs in P06, so this enum does not have to change later.
enum class ColonistState : std::uint8_t
{
	Idle,
	MovingToWork,
	Working,
	MovingToService,
	Refilling,
	Dead
};

// A path returned by NavigationGrid::findPath plus how far along it the colonist is.
struct Route
{
	std::vector<Cell> cells;
	std::size_t next_index = 0;   // index of the next cell to walk to

	bool empty() const { return cells.empty(); }
	bool finished() const { return next_index >= cells.size(); }
	void clear() { cells.clear(); next_index = 0; }
};

// ---------------------------------------------------------------------------
// Player commands (plan C2/C4/C6.1)
// The UI and input never change the simulation directly: they queue a GameCommand
// and ColonySimulation::apply() returns a CommandResult. Commands are applied once
// per frame, FIFO, before that frame's ticks (also while paused). AssignJob is the
// P05 role order from the colonist popup; later steps add PlaceBuilding (P07),
// CallRocket (P09) and Restart (P10).
// ---------------------------------------------------------------------------

enum class CommandKind : std::uint8_t
{
	PlaceDebugObstacleAhead,   // P03 debug: block a cell ahead on a colonist's route
	ToggleDebugObstacle,       // P03 debug: block/unblock one cell
	ClearDebugObstacles,       // P03 debug: remove every debug obstacle
	SetDoorOpen,               // debug: open/close a completed Exit Door (door index)
	AssignJob                  // P05: change a colonist's role (releases its work slot)
};

struct GameCommand
{
	CommandKind kind = CommandKind::ClearDebugObstacles;
	ColonistId colonist = INVALID_COLONIST;
	Cell cell;
	std::uint32_t index = 0;   // door index for SetDoorOpen
	bool flag = false;         // open/close for SetDoorOpen
	Job job = Job::Engineer;   // new role for AssignJob

	static GameCommand placeDebugObstacleAhead(ColonistId id)
	{
		GameCommand command;
		command.kind = CommandKind::PlaceDebugObstacleAhead;
		command.colonist = id;
		return command;
	}
	static GameCommand toggleDebugObstacle(Cell target)
	{
		GameCommand command;
		command.kind = CommandKind::ToggleDebugObstacle;
		command.cell = target;
		return command;
	}
	static GameCommand clearDebugObstacles()
	{
		GameCommand command;
		command.kind = CommandKind::ClearDebugObstacles;
		return command;
	}
	static GameCommand setDoorOpen(std::uint32_t door_index, bool open)
	{
		GameCommand command;
		command.kind = CommandKind::SetDoorOpen;
		command.index = door_index;
		command.flag = open;
		return command;
	}
	static GameCommand assignJob(ColonistId id, Job new_job)
	{
		GameCommand command;
		command.kind = CommandKind::AssignJob;
		command.colonist = id;
		command.job = new_job;
		return command;
	}
};

// Structured result: `code` is stable for tests/logic ("ok", "unknown_colonist", ...),
// `detail` is the user-facing explanation shown by the UI.
struct CommandResult
{
	bool accepted = false;
	std::string code;
	std::string detail;
	Cell cell;                 // affected cell, when there is one
};
