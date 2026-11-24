#pragma once

// Shared gameplay types for ARKFALL.
// Standard C++ only: no OpenGL, SDL or framework includes, so the simulation
// and tests can use it without a window.

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
// Colonists
// ---------------------------------------------------------------------------

// Colonist role (plan C2). Loaded from scenario.json / colonists.json by name.
enum class Job : std::uint8_t
{
	Engineer,
	Miner,
	Farmer
};

// Full state list from plan C6.1. P03 only uses Idle and MovingToWork;
// the rest are filled in from P05/P06, so this enum does not have to change later.
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
// per frame, FIFO, before that frame's ticks (also while paused). Later steps add
// AssignJob (P05), PlaceBuilding (P07), CallRocket (P09) and Restart (P10).
// ---------------------------------------------------------------------------

enum class CommandKind : std::uint8_t
{
	PlaceDebugObstacleAhead,   // P03 debug: block a cell ahead on a colonist's route
	ToggleDebugObstacle,       // P03 debug: block/unblock one cell
	ClearDebugObstacles,       // P03 debug: remove every debug obstacle
	SetDoorOpen                // debug until P07: open/close an Exit Door socket
};

struct GameCommand
{
	CommandKind kind = CommandKind::ClearDebugObstacles;
	ColonistId colonist = INVALID_COLONIST;
	Cell cell;
	std::uint32_t index = 0;   // door index for SetDoorOpen
	bool flag = false;         // open/close for SetDoorOpen

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
