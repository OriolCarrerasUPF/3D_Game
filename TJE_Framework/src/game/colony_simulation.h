#pragma once

// Gameplay state and deterministic fixed-step updates (plan C1/C4), headless.
// No OpenGL, SDL or resource loading: Game calls tick() from fixedUpdate() and the
// view layer only reads the records below.
//
// P03: navigation grid from scenario.json, colonists that walk routes found by BFS
// (through the Exit Door, around blockers) with the plan's distance budget; a colonist
// with a debugRoute walks it stop -> stop.
// P05: building records from the scenario's initialBuildings and buildings.json
// (footprints block their cells; an Exit Door building opens its socket), the shared
// item inventory, operating work slots with reservation/release, nearest-job
// selection, production from physically present workers with atomic recipes, and the
// AssignJob role order. Needs and utilities arrive in P06, construction in P07.
// P05B (base layout change, plan C5): there is no fixed base. A completed Corridor or
// Exit Door turns its inside cells inside and the Exit Door registers its crossings as
// a door; domes (interior buildings) are worked from their entrances (access cells
// on inside cells), and a colonist present at a dome entrance is inside that dome
// (current_dome), which the view hides and the dome popup lists.
//
// P06 (plan C6/C7): Hunger, Thirst and Oxygen decay and kill at 0; a need at or below
// the threshold interrupts work for the best reachable service (eat/drink at Storage
// entrances, suit refill in a powered airlock chamber while the base has oxygen); one
// UtilitySnapshot per tick (all-or-nothing power, water allocated drinkers first, then
// Oxygenators, then Greenhouses; indoor oxygen coverage); idle colonists wait inside a
// dome (hidden) or in the base instead of outside.
//
// Tick order (plan P06):
//   1. utilities: one UtilitySnapshot read by everything below;
//   2. every colonist: decay needs, mark deaths (releasing their slots);
//   3. every colonist (colonist-ID order): validate work/service, interrupt work for an
//      urgent need, choose a job or an idle spot, move, arrive;
//   4. services (drinking, eating, suit refill) and building production (ID order).
//   Commands are applied by Game before the ticks; outcomes arrive in P09.
//
// Theory reuse (T13): state machine with explicit states and transitions (C6.1),
// grid pathfinding and nearest-target search (BFS findNearest).

#include "arkfall_types.h"
#include "game_definitions.h"
#include "inventory.h"
#include "navigation_grid.h"
#include "scenario_loader.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

// One operating work slot held by a colonist: (building, slot index). A colonist holds
// at most one; each slot has at most one owner (plan C6.1).
struct WorkReservation
{
	BuildingId building = INVALID_BUILDING;
	int slot = -1;
	bool active() const { return building != INVALID_BUILDING; }
};

// Utility state of one tick (plan C7), computed once at the start of the tick.
struct UtilitySnapshot
{
	float power_generated = 0.0f;
	float power_demand = 0.0f;
	bool powered = false;              // all-or-nothing: generation covers the whole demand
	float water_produced = 0.0f;       // powered extractors, units per second
	float water_for_drinking = 0.0f;
	float water_for_buildings = 0.0f;
	int drinkers_served = 0;
	int drinkers_waiting = 0;          // drinking but not allocated this tick (shortage)
	int oxygen_capacity = 0;           // operational Oxygenators
	int indoor_population = 0;         // living colonists on inside cells (domes and corridors)
	bool oxygen_covered = false;       // capacity > 0 and it covers everyone indoors
	std::vector<BuildingId> stopped;   // consumers stopped by a power or water shortage

	float waterUsed() const { return water_for_drinking + water_for_buildings; }
};

struct ColonistRecord
{
	ColonistId id = INVALID_COLONIST;
	std::string name;                // from the colonists.json pool, in colonist-ID order
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

	// P05 work: the reserved slot and the access cell walked to / worked at.
	WorkReservation reservation;
	// P05B: the building the colonist is inside, or INVALID_BUILDING: a dome whose entrance
	// it stands at (working, refilling or idle-waiting) or, since P06, an exterior building
	// (a mine) it works at. The view hides such colonists.
	BuildingId current_dome = INVALID_BUILDING;
	Cell work_cell;
	float job_retry_timer = 0.0f;     // time until an idle colonist looks for work again
	std::uint64_t evaluated_work_revision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t evaluated_grid_revision = std::numeric_limits<std::uint64_t>::max();

	// P06 services: the need being refilled and where (Storage entrance or airlock chamber).
	bool in_service = false;
	NeedType service_need = NeedType::Oxygen;
	BuildingId service_building = INVALID_BUILDING;
	Cell service_cell;
	float service_timer = 0.0f;       // eating progress
	bool meal_taken = false;          // the vegetable is taken when eating starts
	bool water_allocated = false;     // drinking water allocated in this tick's snapshot
	float service_retry_timer = 0.0f; // urgent need without a service: time until the next try
	std::uint64_t service_work_revision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t service_grid_revision = std::numeric_limits<std::uint64_t>::max();
	std::string need_alert;           // "needs water: ..." while an urgent need has no service
	// P06 idle rule: where an idle colonist waits (a dome entrance, else an inside cell).
	bool has_idle_target = false;
	Cell idle_cell;
	BuildingId idle_dome = INVALID_BUILDING;
	std::uint64_t idle_grid_revision = std::numeric_limits<std::uint64_t>::max();
	std::string death_cause;          // "thirst", "hunger", "lack of oxygen"

	// Temporary P03 scripted route.
	bool has_debug_route = false;
	bool debug_route_done = false;
	std::size_t stop_index = 0;       // stop currently travelled to / waited at
	float wait_timer = 0.0f;          // dwell time left at the current stop
	float retry_timer = 0.0f;         // time until a failed route is retried
	std::uint64_t failed_revision = 0;
	bool route_failed = false;

	std::string status;               // current task / reason, shown in the colonist popup
};

// Why a building is or is not producing right now (shown in the building popup).
enum class BuildingStatus : std::uint8_t
{
	Idle,           // complete, no operation (Storage, Solar Panel, Exit Door ...)
	Running,        // workers present and the next cycle can commit
	NoWorkers,      // operation needs workers and none is present
	MissingInput,   // recipe inputs are not in the inventory
	StorageFull,    // the output does not fit in the item capacity
	Disabled,
	NoPower,        // P06: power demand not covered (all-or-nothing brownout)
	NoWater         // P06: its water flow could not be allocated
};

struct BuildingRecord
{
	BuildingId id = INVALID_BUILDING;
	BuildingType type = BuildingType::Storage;
	ConstructionState construction = ConstructionState::Complete;
	float construction_progress = 0.0f;   // seconds of work (P07)
	bool enabled = true;

	Cell origin;                          // minimum corner of the rotated footprint
	int quarter_turns = 0;
	int width = 0;                        // rotated size in cells
	int height = 0;
	std::vector<Cell> footprint_cells;    // blocked, except for corridors and airlocks
	std::vector<Cell> access_cells;       // domes: entrance candidates
	std::vector<Cell> inside_cells;       // become inside on completion (corridor cell, airlock chamber)
	std::vector<DoorPortal> crossings;    // Exit Door crossings (world cells)
	std::optional<std::size_t> door_index; // NavigationGrid door of a completed Exit Door
	std::optional<std::size_t> deposit_index; // mines: the deposit inside the footprint

	std::vector<ColonistId> slots;        // operating slot owners (INVALID_COLONIST = free)
	float production_progress = 0.0f;     // work-seconds in the current cycle
	long long cycles_completed = 0;
	// P06 utilities, from this tick's snapshot.
	bool powered = false;                 // no demand, or the colony's generation covers all demand
	bool water_supplied = false;          // no demand, or its full flow was allocated
	bool operational = false;             // complete, enabled, powered and supplied
	BuildingStatus status = BuildingStatus::Idle;
	std::string status_text;              // blocking reason / state for the UI

	bool isComplete() const { return construction == ConstructionState::Complete; }
};

// Result of the shared placement rules (plan C5). P05/P05B check bounds, overlaps,
// spawn cells, deposits, access cells and the base network (corridor adjacency, dome
// entrance on the network, exterior buildings off it); P07 adds cost and the
// enclosure/connectivity checks.
struct PlacementResult
{
	bool valid = false;
	std::string code;      // stable: "ok", "out_of_bounds", "needs_network", "occupied", ...
	std::string reason;    // user-facing
	int width = 0;
	int height = 0;
	std::vector<Cell> footprint_cells;
	std::vector<Cell> access_cells;
	std::vector<Cell> inside_cells;
	std::vector<DoorPortal> crossings;
	std::optional<std::size_t> deposit_index;
};

class ColonySimulation
{
public:
	static constexpr float route_retry_seconds = 2.0f; // plan C6.1
	static constexpr float job_retry_seconds = 2.0f;   // plan C6/C6.1 retry interval

	// Builds the grid, places the initial buildings (validated against buildings.json),
	// creates the inventory and the colonists (named from the pool in ID order). Fails,
	// leaving the simulation empty, on any data error; the message starts with the JSON
	// pointer in scenario.json when it comes from there.
	bool initialize(const ScenarioDefinition& scenario, const GameDefinitions& definitions, std::string& error);
	void reset();

	// One fixed simulation step (seconds).
	void tick(float dt);

	const NavigationGrid& getGrid() const { return grid; }
	const ScenarioDefinition& getScenario() const { return scenario; }
	const GameDefinitions& getDefinitions() const { return definitions; }
	const std::vector<ColonistRecord>& getColonists() const { return colonists; }
	const ColonistRecord* findColonist(ColonistId id) const;
	double getSimulationTime() const { return simulation_time; }
	bool isInitialized() const { return initialized; }

	// ---- P05: buildings, inventory, work ------------------------------------------
	const std::vector<BuildingRecord>& getBuildings() const { return buildings; }
	const BuildingRecord* findBuilding(BuildingId id) const;
	const BuildingDefinition* findDefinition(BuildingType type) const { return definitions.buildings.find(type); }
	const Inventory& getInventory() const { return inventory; }
	// True until the first Storage completes (the scenario's temporary cargo capacity).
	bool usesCargoCapacity() const { return uses_cargo_capacity; }
	// Increases whenever something that can change job eligibility changes (inventory,
	// buildings, slots, roles). Idle colonists re-evaluate on change or every 2 s.
	std::uint64_t getWorkRevision() const { return work_revision; }
	int countReservedWorkers(const BuildingRecord& building) const;
	int countPresentWorkers(const BuildingRecord& building) const;
	// "Iron Mine #3"
	std::string describeBuilding(BuildingId id) const;

	// Shared placement rules for any building with its origin (minimum corner of the
	// rotated footprint) and quarter turns. Pure: nothing is changed.
	PlacementResult validatePlacement(BuildingType type, Cell origin, int quarter_turns) const;

	// ---- P05B: base network and domes ----------------------------------------------
	// A cell that a Corridor or an Exit Door (complete or planned) turns inside.
	bool isPassageCell(Cell cell) const;
	// A passage cell or a cell of a dome footprint (walkable domes are part of the base).
	bool isNetworkCell(Cell cell) const;
	// The building whose footprint covers the cell, or nullptr.
	const BuildingRecord* buildingAt(Cell cell) const;
	// True when the cell is the outside or inside cell of a registered door crossing.
	bool isDoorCell(Cell cell) const;
	// Access cells usable for work and services: every access cell of an exterior
	// building; for a dome only its entrances (access candidates on inside cells).
	std::vector<Cell> usableAccessCells(const BuildingRecord& building) const;
	// Living colonists inside the building (current_dome: a dome, or an exterior building
	// while working there), in colonist-ID order.
	std::vector<ColonistId> occupants(BuildingId dome) const;

	// ---- P06: needs, services and utilities ----------------------------------------
	const UtilitySnapshot& getUtilities() const { return utilities; }
	const NeedRules& getNeedRules() const { return definitions.colonists.getNeedRules(); }
	int countLiving() const;
	// Current drain of one need for this colonist (0 while supplied: base oxygen
	// coverage indoors, a powered mine while working there).
	float currentDrain(const ColonistRecord& colonist, NeedType need) const;
	// A completed, open Exit Door that is powered while the base has oxygen coverage:
	// its chamber refills suits (plan C6.1).
	bool doorRefillsSuits(const BuildingRecord& building) const;
	// Cells where the need can be refilled now, in building-ID order (owners parallel).
	std::vector<Cell> serviceTargets(NeedType need, std::vector<BuildingId>* owners = nullptr) const;
	// Why no service exists for the need ("" when one exists).
	std::string serviceUnavailableReason(NeedType need) const;
	static float needValue(const NeedValues& needs, NeedType need);
	static const char* needLabel(NeedType need);   // "oxygen", "water", "food"

	// Changes a colonist's role, releasing its work slot first (plan C6.1).
	bool assignJob(ColonistId id, Job job, std::string& reason);

	// Applies one player/debug command against the current state (plan C4/C6.1).
	// Game calls it once per frame for every queued command, FIFO, before the ticks.
	CommandResult apply(const GameCommand& command);

	// ---- P03 debug command implementations (used by apply and by tests) ------
	// Blocks a free cell a few steps ahead on the colonist's current route to show the
	// detour. Never blocks door cells, route goals or cells a colonist stands on/enters.
	bool placeDebugObstacleAhead(ColonistId id, Cell& out_cell, std::string& reason);
	// Blocks/unblocks one specific cell (only cells not blocked by scenario data).
	bool toggleDebugObstacle(Cell cell, std::string& reason);
	void clearDebugObstacles();
	const std::set<std::pair<int, int>>& getDebugObstacles() const { return debug_obstacles; }
	bool setDoorOpen(std::size_t door_index, bool open);

private:
	ColonistRecord* findColonistMutable(ColonistId id);
	BuildingRecord* findBuildingMutable(BuildingId id);

	// Colonist update: shared pre-step, then the scripted P03 route or the P05 worker.
	void updateColonist(ColonistRecord& colonist, float dt);
	void updateScriptedColonist(ColonistRecord& colonist, float dt);
	void updateWorker(ColonistRecord& colonist, float dt);
	bool planRoute(ColonistRecord& colonist, Cell goal);
	bool isRemainingRouteValid(const ColonistRecord& colonist) const;
	void advanceAlongRoute(ColonistRecord& colonist, float distance_budget);
	void onRouteFinished(ColonistRecord& colonist);
	bool isCellReservedByColonist(Cell cell) const;
	bool isUsableAccess(const BuildingRecord& building, Cell cell) const;
	void updatePresence(ColonistRecord& colonist);
	// Ends the current step at a cell centre (never between two cells).
	void stopAtCellCentre(ColonistRecord& colonist);

	// P06 utilities, needs and services.
	void computeUtilities();
	void decayNeeds(ColonistRecord& colonist, float dt);
	void killColonist(ColonistRecord& colonist, NeedType cause);
	bool isSuppliedByMine(const ColonistRecord& colonist) const;
	bool startService(ColonistRecord& colonist, std::string& reason);
	bool isServiceStillValid(const ColonistRecord& colonist) const;
	void arriveAtService(ColonistRecord& colonist);
	void endService(ColonistRecord& colonist, bool finished, const std::string& reason);
	void performServices(float dt);
	void updateIdleTarget(ColonistRecord& colonist, const std::string& reason);
	bool isIdleTargetValid(const ColonistRecord& colonist) const;
	// Outdoor job check (plan C6): the suit must last the trip out and back to a refill point.
	bool suitAllowsOutdoorJob(const ColonistRecord& colonist, Cell job_cell, const std::vector<Cell>& path_to_job) const;

	// Work slots (plan C6/C6.1).
	bool isWorkplaceFor(const BuildingRecord& building, Job job) const;
	// Empty string: the next cycle can run; otherwise the blocking reason.
	std::string productionBlocker(const BuildingRecord& building, BuildingStatus* out_status = nullptr) const;
	bool selectJob(ColonistRecord& colonist);
	std::string idleReason(const ColonistRecord& colonist) const;
	void reserveSlot(ColonistRecord& colonist, BuildingRecord& building, int slot, Cell access_cell);
	// Idempotent: frees the slot, clears the target and stops at the nearest cell centre.
	void releaseReservation(ColonistRecord& colonist, const std::string& reason);
	void arriveAtWork(ColonistRecord& colonist);

	// Buildings and production.
	BuildingId addCompleteBuilding(BuildingType type, const PlacementResult& placement, int quarter_turns, Cell origin);
	void recomputeCapacity();
	void updateProduction(BuildingRecord& building, float dt);

	ScenarioDefinition scenario;
	GameDefinitions definitions;     // copy: the simulation never depends on caller lifetime
	NavigationGrid grid;
	std::vector<ColonistRecord> colonists;     // ascending ColonistId
	std::vector<BuildingRecord> buildings;     // ascending BuildingId
	Inventory inventory;
	bool uses_cargo_capacity = true;
	BuildingId next_building_id = 1;
	std::uint64_t work_revision = 0;
	UtilitySnapshot utilities;
	std::uint64_t utility_signature = 0;
	std::set<std::pair<int, int>> debug_obstacles;
	double simulation_time = 0.0;
	bool initialized = false;
	std::vector<Cell> path_scratch;
	mutable std::vector<Cell> return_scratch;
};
