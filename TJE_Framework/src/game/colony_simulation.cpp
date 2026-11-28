#include "colony_simulation.h"

#include "world_transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
	std::string cellText(Cell cell)
	{
		return "(" + std::to_string(cell.x) + ", " + std::to_string(cell.z) + ")";
	}

	const char* roleName(Job job)
	{
		switch (job)
		{
		case Job::Engineer: return "Engineer";
		case Job::Miner: return "Miner";
		case Job::Farmer: return "Farmer";
		}
		return "colonist";
	}

	bool containsCell(const std::vector<Cell>& cells, Cell cell)
	{
		return std::find(cells.begin(), cells.end(), cell) != cells.end();
	}

	// Work-seconds slack when comparing accumulated production time with a cycle, so
	// 150 ticks of 1/30 s complete a 5 s cycle despite float rounding.
	constexpr float production_epsilon = 1e-4f;
	// The one documented epsilon for flow comparisons (power and water, plan C7).
	constexpr float flow_epsilon = 1e-4f;

	constexpr NeedType need_order[need_type_count] = { NeedType::Oxygen, NeedType::Thirst, NeedType::Hunger };

	float& needRef(NeedValues& needs, NeedType need)
	{
		switch (need)
		{
		case NeedType::Hunger: return needs.hunger;
		case NeedType::Thirst: return needs.thirst;
		case NeedType::Oxygen: break;
		}
		return needs.oxygen;
	}

	std::string formatFlow(float value)
	{
		char text[32];
		std::snprintf(text, sizeof(text), "%.2f", value);
		return text;
	}
}

void ColonySimulation::reset()
{
	scenario = ScenarioDefinition();
	definitions = GameDefinitions();
	grid = NavigationGrid();
	colonists.clear();
	buildings.clear();
	inventory = Inventory();
	uses_cargo_capacity = true;
	next_building_id = 1;
	work_revision = 0;
	utilities = UtilitySnapshot();
	utility_signature = 0;
	debug_obstacles.clear();
	simulation_time = 0.0;
	initialized = false;
}

bool ColonySimulation::initialize(const ScenarioDefinition& definition, const GameDefinitions& game_definitions, std::string& error)
{
	reset();
	const ColonistCatalog& roles = game_definitions.colonists;
	std::vector<ColonistRecord> new_colonists;
	for (const InitialColonistDefinition& initial : definition.initial_colonists)
	{
		const ColonistRoleDefinition* role = roles.findRole(initial.job);
		if (!role)
		{
			error = "colonist " + std::to_string(initial.id) + " has a job without a role definition";
			return false;
		}
		ColonistRecord colonist;
		colonist.id = initial.id;
		colonist.job = initial.job;
		colonist.needs = initial.needs;
		colonist.move_speed = role->move_speed;
		colonist.last_cell = initial.cell;
		colonist.status = "idle";
		new_colonists.push_back(colonist);
	}

	// Display names: one unique name per colonist, handed out in stable ID order (C9.1).
	std::sort(new_colonists.begin(), new_colonists.end(),
		[](const ColonistRecord& a, const ColonistRecord& b) { return a.id < b.id; });
	const std::vector<std::string>& names = roles.getNames();
	if (names.size() < new_colonists.size())
	{
		error = "colonists.json: the name pool has " + std::to_string(names.size()) +
			" names but the scenario starts with " + std::to_string(new_colonists.size()) + " colonists";
		return false;
	}
	for (std::size_t i = 0; i < new_colonists.size(); ++i)
		new_colonists[i].name = names[i];

	scenario = definition;
	definitions = game_definitions;
	grid = buildNavigationGrid(scenario);
	colonists.swap(new_colonists);
	for (ColonistRecord& colonist : colonists)
	{
		colonist.position = grid.cellCenter(colonist.last_cell);
		colonist.previous_position = colonist.position;
		colonist.has_debug_route = scenario.debug_route && scenario.debug_route->colonist_id == colonist.id;
		colonist.work_cell = colonist.last_cell;
	}

	// Initial (complete) buildings, validated with the same placement rules as P07. A
	// corridor strip is placed cell by cell, row by row from `from`, so each cell joins
	// the network through the door or an earlier cell of the strip (C5).
	for (std::size_t i = 0; i < scenario.initial_buildings.size(); ++i)
	{
		const InitialBuildingDefinition& initial = scenario.initial_buildings[i];
		const std::string pointer = "/initialBuildings/" + std::to_string(i);
		const BuildingDefinition* def = definitions.buildings.findById(initial.type_id);
		if (!def)
		{
			error = pointer + "/type: unknown building type '" + initial.type_id + "' (not in buildings.json)";
			reset();
			return false;
		}
		if (initial.is_strip && def->placement != PlacementKind::Corridor)
		{
			error = pointer + ": '" + def->id + "' is not a corridor (give origin + quarterTurns)";
			reset();
			return false;
		}
		std::vector<std::pair<Cell, int>> placements;
		if (initial.is_strip)
			for (Cell cell : corridorStripCells(initial.strip_from, initial.strip_to))
				placements.push_back({ cell, 0 });
		else
			placements.push_back({ initial.origin, initial.quarter_turns });
		for (const auto& [origin, quarter_turns] : placements)
		{
			const PlacementResult placement = validatePlacement(def->type, origin, quarter_turns);
			if (!placement.valid)
			{
				error = pointer + ": " + def->label + " at " + cellText(origin) + " cannot be placed: " + placement.reason;
				reset();
				return false;
			}
			addCompleteBuilding(def->type, placement, quarter_turns, origin);
		}
	}

	// The scripted debug route must be walkable now that the doors and corridors exist.
	if (scenario.debug_route)
	{
		const DebugRouteDefinition& route = *scenario.debug_route;
		const ColonistRecord* walker = findColonist(route.colonist_id);
		if (!walker)
		{
			error = "/debugRoute/colonistId: unknown colonist id " + std::to_string(route.colonist_id);
			reset();
			return false;
		}
		Cell from = walker->last_cell;
		const std::size_t legs = route.stops.size() + (route.loop ? 1 : 0);
		for (std::size_t leg = 0; leg < legs; ++leg)
		{
			const std::size_t stop_index = leg % route.stops.size();
			const Cell to = route.stops[stop_index].cell;
			if (!grid.findPath(from, to, path_scratch))
			{
				error = "/debugRoute/stops/" + std::to_string(stop_index) + "/cell: no 4-neighbour path from " +
					cellText(from) + " to " + cellText(to);
				reset();
				return false;
			}
			from = to;
		}
	}

	inventory = Inventory(scenario.initial_inventory, 0);
	recomputeCapacity();
	if (inventory.getTotal() > inventory.getCapacity())
	{
		error = "/initialInventory: " + std::to_string(inventory.getTotal()) + " items do not fit in the starting capacity of " +
			std::to_string(inventory.getCapacity());
		reset();
		return false;
	}
	initialized = true;
	return true;
}

const ColonistRecord* ColonySimulation::findColonist(ColonistId id) const
{
	for (const ColonistRecord& colonist : colonists)
		if (colonist.id == id)
			return &colonist;
	return nullptr;
}

ColonistRecord* ColonySimulation::findColonistMutable(ColonistId id)
{
	for (ColonistRecord& colonist : colonists)
		if (colonist.id == id)
			return &colonist;
	return nullptr;
}

const BuildingRecord* ColonySimulation::findBuilding(BuildingId id) const
{
	for (const BuildingRecord& building : buildings)
		if (building.id == id)
			return &building;
	return nullptr;
}

BuildingRecord* ColonySimulation::findBuildingMutable(BuildingId id)
{
	for (BuildingRecord& building : buildings)
		if (building.id == id)
			return &building;
	return nullptr;
}

std::string ColonySimulation::describeBuilding(BuildingId id) const
{
	const BuildingRecord* building = findBuilding(id);
	const BuildingDefinition* def = building ? findDefinition(building->type) : nullptr;
	if (!def)
		return "building #" + std::to_string(id);
	return def->label + " #" + std::to_string(id);
}

void ColonySimulation::tick(float dt)
{
	if (!initialized || !(dt > 0.0f))
		return;
	// 1. One utility snapshot for the whole tick (plan C7): buildings completed during
	//    this tick become operational on the next one.
	computeUtilities();
	// 2. Needs decay; a need at zero kills and releases the colonist's slot at once.
	for (ColonistRecord& colonist : colonists)
		decayNeeds(colonist, dt);
	// 3. Colonists in ID order: work/service validation, interruption, job or idle
	//    spot choice, movement, arrival.
	for (ColonistRecord& colonist : colonists)
		updateColonist(colonist, dt);
	// 4. Services, then production from the workers present now (building-ID order).
	performServices(dt);
	for (BuildingRecord& building : buildings)
		updateProduction(building, dt);
	simulation_time += dt;
}

bool ColonySimulation::planRoute(ColonistRecord& colonist, Cell goal)
{
	const Cell old_next = colonist.route.finished() ? colonist.last_cell : colonist.route.cells[colonist.route.next_index];
	if (!grid.findPath(colonist.last_cell, goal, path_scratch))
	{
		colonist.route.clear();
		colonist.route_failed = true;
		colonist.failed_revision = grid.getRevision();
		colonist.retry_timer = route_retry_seconds;
		colonist.state = ColonistState::Idle;
		colonist.status = "no route from " + cellText(colonist.last_cell) + " to " + cellText(goal);
		return false;
	}

	colonist.route.cells = path_scratch;
	colonist.route_goal = goal;
	colonist.route_revision = grid.getRevision();
	colonist.route_failed = false;

	// The path starts at last_cell. If the colonist already stands on it, skip it;
	// if it was walking toward the same next cell, keep going forward; otherwise it
	// walks back to the centre of last_cell first (never through a blocked cell).
	const Vector3 last_center = grid.cellCenter(colonist.last_cell);
	const float dx = colonist.position.x - last_center.x;
	const float dz = colonist.position.z - last_center.z;
	const bool at_last_cell = dx * dx + dz * dz < 1e-8f;
	if (at_last_cell || (colonist.route.cells.size() > 1 && colonist.route.cells[1] == old_next))
		colonist.route.next_index = 1;
	else
		colonist.route.next_index = 0;

	colonist.state = colonist.route.finished() ? ColonistState::Idle : ColonistState::MovingToWork;
	colonist.status = "walking to " + cellText(goal);
	return true;
}

bool ColonySimulation::isRemainingRouteValid(const ColonistRecord& colonist) const
{
	const Route& route = colonist.route;
	Cell previous = colonist.last_cell;
	for (std::size_t i = route.next_index; i < route.cells.size(); ++i)
	{
		const Cell cell = route.cells[i];
		if (cell == previous)
		{
			if (!grid.isWalkable(cell))
				return false;
			continue;
		}
		if (!grid.canStep(previous, cell))
			return false;
		previous = cell;
	}
	return true;
}

void ColonySimulation::advanceAlongRoute(ColonistRecord& colonist, float distance_budget)
{
	Route& route = colonist.route;
	float remaining = distance_budget;
	while (remaining > 0.0f && !route.finished())
	{
		const Cell next_cell = route.cells[route.next_index];
		const Vector3 target = grid.cellCenter(next_cell);
		const float dx = target.x - colonist.position.x;
		const float dz = target.z - colonist.position.z;
		const float distance = std::sqrt(dx * dx + dz * dz);
		if (distance > 1e-6f)
			colonist.yaw = world_transform::yawFromDirection(dx, dz);

		if (distance <= remaining)
		{
			colonist.position = target; // snap exactly to the waypoint
			remaining -= distance;
			colonist.last_cell = next_cell;
			++route.next_index;
		}
		else
		{
			colonist.position.x += dx / distance * remaining;
			colonist.position.z += dz / distance * remaining;
			remaining = 0.0f;
		}
	}
}

void ColonySimulation::onRouteFinished(ColonistRecord& colonist)
{
	colonist.route.clear();
	colonist.state = ColonistState::Idle;
	if (!colonist.has_debug_route || !scenario.debug_route)
	{
		colonist.status = "idle";
		return;
	}
	const DebugRouteDefinition& route = *scenario.debug_route;
	colonist.status = "at " + route.stops[colonist.stop_index].label;
	colonist.wait_timer = route.dwell_seconds;
	++colonist.stop_index;
	if (colonist.stop_index >= route.stops.size())
	{
		if (route.loop)
			colonist.stop_index = 0;
		else
			colonist.debug_route_done = true;
	}
}

void ColonySimulation::updateColonist(ColonistRecord& colonist, float dt)
{
	colonist.previous_position = colonist.position;
	colonist.previous_yaw = colonist.yaw;
	if (colonist.state == ColonistState::Dead)
		return;
	// A colonist with the temporary P03 debugRoute follows its script and never takes jobs.
	if (colonist.has_debug_route)
		updateScriptedColonist(colonist, dt);
	else
		updateWorker(colonist, dt);
	updatePresence(colonist);
}

void ColonySimulation::updateScriptedColonist(ColonistRecord& colonist, float dt)
{
	// 1. A navigation change (door, obstacle) may invalidate the current route.
	if (!colonist.route.empty() && !colonist.route.finished() && colonist.route_revision != grid.getRevision())
	{
		if (isRemainingRouteValid(colonist))
			colonist.route_revision = grid.getRevision();
		else
			planRoute(colonist, colonist.route_goal);
	}

	// 2. Without a route: wait at the stop, retry a failed route, or start the next leg.
	if (colonist.route.empty() || colonist.route.finished())
	{
		if (colonist.route_failed)
		{
			colonist.retry_timer -= dt;
			if (colonist.retry_timer > 0.0f && colonist.failed_revision == grid.getRevision())
				return;
			if (!planRoute(colonist, colonist.route_goal))
				return;
		}
		else
		{
			if (colonist.wait_timer > 0.0f)
			{
				colonist.wait_timer -= dt;
				return;
			}
			if (!colonist.has_debug_route || colonist.debug_route_done || !scenario.debug_route)
				return;
			colonist.route_goal = scenario.debug_route->stops[colonist.stop_index].cell;
			if (!planRoute(colonist, colonist.route_goal))
				return;
		}
		if (colonist.route.finished())
		{
			onRouteFinished(colonist); // already standing on the goal
			return;
		}
	}

	// 3. Move along the route with this tick's distance budget.
	advanceAlongRoute(colonist, colonist.move_speed * dt);
	if (colonist.route.finished())
		onRouteFinished(colonist);
}

bool ColonySimulation::isDoorCell(Cell cell) const
{
	for (std::size_t door = 0; door < grid.getDoorCount(); ++door)
		for (const DoorPortal& portal : grid.getDoorPortals(door))
			if (portal.inside == cell || portal.outside == cell)
				return true;
	return false;
}

bool ColonySimulation::isPassageCell(Cell cell) const
{
	for (const BuildingRecord& building : buildings)
		if (containsCell(building.inside_cells, cell))
			return true;
	return false;
}

bool ColonySimulation::isNetworkCell(Cell cell) const
{
	if (isPassageCell(cell))
		return true;
	const BuildingRecord* building = buildingAt(cell);
	const BuildingDefinition* def = building ? findDefinition(building->type) : nullptr;
	return def && def->isDome();
}

const BuildingRecord* ColonySimulation::buildingAt(Cell cell) const
{
	for (const BuildingRecord& building : buildings)
		if (containsCell(building.footprint_cells, cell))
			return &building;
	return nullptr;
}

bool ColonySimulation::isUsableAccess(const BuildingRecord& building, Cell cell) const
{
	// A dome is entered only from the base: its access cells are entrance candidates and
	// count once they are inside (a corridor or an airlock chamber was built there).
	const BuildingDefinition* def = findDefinition(building.type);
	if (def && def->isDome())
		return grid.getZone(cell) == Zone::Inside;
	return true;
}

std::vector<Cell> ColonySimulation::usableAccessCells(const BuildingRecord& building) const
{
	std::vector<Cell> cells;
	for (Cell cell : building.access_cells)
		if (isUsableAccess(building, cell))
			cells.push_back(cell);
	return cells;
}

std::vector<ColonistId> ColonySimulation::occupants(BuildingId dome) const
{
	std::vector<ColonistId> ids;
	if (dome == INVALID_BUILDING)
		return ids;
	for (const ColonistRecord& colonist : colonists)
		if (colonist.state != ColonistState::Dead && colonist.current_dome == dome)
			ids.push_back(colonist.id);
	return ids;
}

void ColonySimulation::updatePresence(ColonistRecord& colonist)
{
	// Present inside a building: working, refilling or idle-waiting at a dome entrance, or
	// working at an exterior building's access cell. Anything else is visible.
	// Crossing a walkable dome: hidden inside it while standing in its footprint.
	if (colonist.state != ColonistState::Dead)
	{
		Cell standing;
		if (grid.worldToCell(colonist.position.x, colonist.position.z, standing))
			if (const BuildingRecord* room = findBuilding(grid.getRoom(standing)))
				if (room->isComplete())
				{
					colonist.current_dome = room->id;
					return;
				}
	}
	BuildingId candidate = INVALID_BUILDING;
	if (colonist.state != ColonistState::Dead && colonist.route.finished())
	{
		if (colonist.state == ColonistState::Working && colonist.reservation.active())
			candidate = colonist.reservation.building;
		else if (colonist.state == ColonistState::Refilling && colonist.in_service)
			candidate = colonist.service_building;
		else if (colonist.state == ColonistState::Idle && colonist.has_idle_target && colonist.last_cell == colonist.idle_cell)
			candidate = colonist.idle_dome;
	}
	BuildingId dome = INVALID_BUILDING;
	const BuildingRecord* building = findBuilding(candidate);
	const BuildingDefinition* def = building ? findDefinition(building->type) : nullptr;
	if (building && def && building->isComplete() && containsCell(building->access_cells, colonist.last_cell))
	{
		// Domes are entered from the base (an inside entrance). A worker at an exterior
		// building (a mine) is inside it too, but only while working there.
		if (def->isDome() ? grid.getZone(colonist.last_cell) == Zone::Inside
			: colonist.state == ColonistState::Working && !def->hasWalkableFootprint())
			dome = building->id;
	}
	colonist.current_dome = dome;
}

bool ColonySimulation::isCellReservedByColonist(Cell cell) const
{
	for (const ColonistRecord& colonist : colonists)
	{
		if (colonist.last_cell == cell || colonist.route_goal == cell)
			return true;
		if (!colonist.route.finished() && colonist.route.cells[colonist.route.next_index] == cell)
			return true;
		Cell standing;
		if (grid.worldToCell(colonist.position.x, colonist.position.z, standing) && standing == cell)
			return true;
	}
	if (scenario.debug_route)
		for (const RouteStopDefinition& stop : scenario.debug_route->stops)
			if (stop.cell == cell)
				return true;
	return false;
}

bool ColonySimulation::toggleDebugObstacle(Cell cell, std::string& reason)
{
	const std::pair<int, int> key{ cell.x, cell.z };
	if (debug_obstacles.count(key))
	{
		debug_obstacles.erase(key);
		grid.setBlocked(cell, false);
		return true;
	}
	if (!grid.isWalkable(cell))
	{
		reason = "cell " + cellText(cell) + " is outside the grid or already blocked by scenario data";
		return false;
	}
	if (isDoorCell(cell))
	{
		reason = "door crossing cells cannot be blocked";
		return false;
	}
	if (isCellReservedByColonist(cell))
	{
		reason = "a colonist stands on, enters or targets cell " + cellText(cell);
		return false;
	}
	debug_obstacles.insert(key);
	grid.setBlocked(cell, true);
	return true;
}

bool ColonySimulation::placeDebugObstacleAhead(ColonistId id, Cell& out_cell, std::string& reason)
{
	const ColonistRecord* colonist = findColonist(id);
	if (!colonist)
	{
		reason = "unknown colonist";
		return false;
	}
	const Route& route = colonist->route;
	if (route.finished())
	{
		reason = "the colonist is not walking right now";
		return false;
	}
	// Prefer a cell a few steps ahead so the detour is visible.
	for (std::size_t i = route.next_index + 2; i + 1 < route.cells.size(); ++i)
	{
		const Cell cell = route.cells[i];
		std::string ignored;
		if (!debug_obstacles.count({ cell.x, cell.z }) && toggleDebugObstacle(cell, ignored))
		{
			out_cell = cell;
			return true;
		}
	}
	reason = "no free cell ahead on the route (too close to the goal or the door)";
	return false;
}

void ColonySimulation::clearDebugObstacles()
{
	for (const auto& [x, z] : debug_obstacles)
		grid.setBlocked(Cell{ x, z }, false);
	debug_obstacles.clear();
}

CommandResult ColonySimulation::apply(const GameCommand& command)
{
	CommandResult result;
	auto finish = [&result](bool accepted, const std::string& code, const std::string& detail) {
		result.accepted = accepted;
		result.code = code;
		result.detail = detail;
		return result;
	};
	if (!initialized)
		return finish(false, "not_initialized", "the simulation has not started");

	switch (command.kind)
	{
	case CommandKind::PlaceDebugObstacleAhead:
	{
		if (!findColonist(command.colonist))
			return finish(false, "unknown_colonist", "unknown colonist " + std::to_string(command.colonist));
		std::string reason;
		Cell cell;
		if (!placeDebugObstacleAhead(command.colonist, cell, reason))
			return finish(false, "no_free_cell", "no obstacle placed: " + reason);
		result.cell = cell;
		return finish(true, "ok", "debug obstacle at " + cellText(cell) + ": the colonist must detour");
	}
	case CommandKind::ToggleDebugObstacle:
	{
		result.cell = command.cell;
		const bool was_obstacle = debug_obstacles.count({ command.cell.x, command.cell.z }) != 0;
		std::string reason;
		if (!toggleDebugObstacle(command.cell, reason))
			return finish(false, "cell_refused", reason);
		return finish(true, "ok", (was_obstacle ? "debug obstacle removed at " : "debug obstacle at ") + cellText(command.cell));
	}
	case CommandKind::ClearDebugObstacles:
	{
		const std::size_t count = debug_obstacles.size();
		clearDebugObstacles();
		return finish(true, "ok", std::to_string(count) + " debug obstacle(s) removed");
	}
	case CommandKind::SetDoorOpen:
	{
		if (!setDoorOpen(command.index, command.flag))
			return finish(false, "unknown_door", "unknown Exit Door " + std::to_string(command.index));
		return finish(true, "ok", command.flag ? "Exit Door opened" : "Exit Door closed: the base is sealed");
	}
	case CommandKind::AssignJob:
	{
		ColonistRecord* colonist = findColonistMutable(command.colonist);
		if (!colonist)
			return finish(false, "unknown_colonist", "unknown colonist " + std::to_string(command.colonist));
		if (colonist->state == ColonistState::Dead)
			return finish(false, "colonist_dead", colonist->name + " is dead");
		if (colonist->job == command.job)
			return finish(false, "same_job", colonist->name + " is already a " + roleName(command.job));
		const ColonistRoleDefinition* role = definitions.colonists.findRole(command.job);
		if (!role)
			return finish(false, "unknown_role", std::string("no role definition for ") + roleName(command.job));
		// Release first (C6.1): the old slot is free before the new role looks for work. A
		// colonist on its way to (or at) a service keeps refilling: it holds no work slot.
		if (!colonist->in_service)
			releaseReservation(*colonist, "role changed");
		colonist->job = command.job;
		colonist->move_speed = role->move_speed;
		colonist->job_retry_timer = 0.0f;
		if (!colonist->in_service)
			colonist->status = std::string("new role: ") + roleName(command.job);
		++work_revision;
		return finish(true, "ok", colonist->name + " is now a " + roleName(command.job));
	}
	}
	return finish(false, "unknown_command", "unknown command");
}

bool ColonySimulation::setDoorOpen(std::size_t door_index, bool open)
{
	if (door_index >= grid.getDoorCount())
		return false;
	grid.setDoorOpen(door_index, open);
	return true;
}

bool ColonySimulation::assignJob(ColonistId id, Job job, std::string& reason)
{
	const CommandResult result = apply(GameCommand::assignJob(id, job));
	reason = result.detail;
	return result.accepted;
}

// ---------------------------------------------------------------------------- work (P05)

bool ColonySimulation::isWorkplaceFor(const BuildingRecord& building, Job job) const
{
	const BuildingDefinition* def = findDefinition(building.type);
	return def && building.isComplete() && building.enabled && def->has_operation && def->operation_role == job;
}

std::string ColonySimulation::productionBlocker(const BuildingRecord& building, BuildingStatus* out_status) const
{
	const BuildingDefinition* def = findDefinition(building.type);
	if (!def || !def->recipe)
		return "";
	// P06: production needs power and its full water flow (this tick's snapshot).
	if (def->power_demand > 0.0f && !building.powered)
	{
		if (out_status)
			*out_status = BuildingStatus::NoPower;
		return "no power";
	}
	if (def->water_demand > 0.0f && !building.water_supplied)
	{
		if (out_status)
			*out_status = BuildingStatus::NoWater;
		return "no water";
	}
	const RecipeDefinition& recipe = *def->recipe;
	switch (inventory.check(recipe.inputs, recipe.outputs))
	{
	case Inventory::Refusal::MissingInput:
	{
		std::string missing;
		for (std::size_t i = 0; i < resource_type_count; ++i)
		{
			const ResourceType type = static_cast<ResourceType>(i);
			if (inventory.get(type) < recipe.inputs[type])
				missing += (missing.empty() ? "" : ", ") + std::string(BuildingCatalog::resourceLabel(type));
		}
		if (out_status)
			*out_status = BuildingStatus::MissingInput;
		return "missing " + missing;
	}
	case Inventory::Refusal::NoSpace:
		if (out_status)
			*out_status = BuildingStatus::StorageFull;
		return "Storage full";
	case Inventory::Refusal::None:
		break;
	}
	return "";
}

int ColonySimulation::countReservedWorkers(const BuildingRecord& building) const
{
	int count = 0;
	for (ColonistId owner : building.slots)
		count += owner != INVALID_COLONIST ? 1 : 0;
	return count;
}

int ColonySimulation::countPresentWorkers(const BuildingRecord& building) const
{
	// Physically present: Working, holding a slot here and standing on an access cell.
	int count = 0;
	for (const ColonistRecord& colonist : colonists)
	{
		if (colonist.state == ColonistState::Working && colonist.reservation.building == building.id &&
			colonist.route.finished() && containsCell(building.access_cells, colonist.last_cell))
			++count;
	}
	return count;
}

void ColonySimulation::reserveSlot(ColonistRecord& colonist, BuildingRecord& building, int slot, Cell access_cell)
{
	building.slots[static_cast<std::size_t>(slot)] = colonist.id;
	colonist.reservation.building = building.id;
	colonist.reservation.slot = slot;
	colonist.work_cell = access_cell;
}

void ColonySimulation::releaseReservation(ColonistRecord& colonist, const std::string& reason)
{
	if (colonist.reservation.active())
	{
		BuildingRecord* building = findBuildingMutable(colonist.reservation.building);
		const int slot = colonist.reservation.slot;
		if (building && slot >= 0 && slot < static_cast<int>(building->slots.size()) &&
			building->slots[static_cast<std::size_t>(slot)] == colonist.id)
			building->slots[static_cast<std::size_t>(slot)] = INVALID_COLONIST;
		colonist.reservation = WorkReservation();
		++work_revision; // a slot became free: idle colonists may now take it
	}
	if (colonist.state != ColonistState::Dead)
		colonist.state = ColonistState::Idle;
	colonist.current_dome = INVALID_BUILDING;
	colonist.has_idle_target = false;
	colonist.job_retry_timer = job_retry_seconds;
	colonist.status = "idle: " + reason;
	stopAtCellCentre(colonist);
}

void ColonySimulation::stopAtCellCentre(ColonistRecord& colonist)
{
	// Stop at a cell centre: finish the step in progress if it is still legal, otherwise
	// walk back to the last reached cell. Never stop between two cells.
	const Vector3 last_center = grid.cellCenter(colonist.last_cell);
	const float dx = colonist.position.x - last_center.x;
	const float dz = colonist.position.z - last_center.z;
	if (dx * dx + dz * dz < 1e-8f)
	{
		colonist.route.clear();
		return;
	}
	Cell stop = colonist.last_cell;
	if (!colonist.route.finished())
	{
		const Cell next = colonist.route.cells[colonist.route.next_index];
		if (grid.canStep(colonist.last_cell, next))
			stop = next;
	}
	colonist.route.cells = { colonist.last_cell };
	colonist.route.next_index = 0;
	if (stop != colonist.last_cell)
	{
		colonist.route.cells.push_back(stop);
		colonist.route.next_index = 1;
	}
	colonist.route_goal = stop;
	colonist.route_revision = grid.getRevision();
}

bool ColonySimulation::selectJob(ColonistRecord& colonist)
{
	// Eligible: compatible role, a free slot, the next cycle can commit (inputs present,
	// output fits) and a walkable access cell. Targets are listed in building-ID order so
	// findNearest's tie rule (first listed) breaks equal distances by building ID (C6).
	std::vector<Cell> targets;
	std::vector<BuildingId> owners;
	for (const BuildingRecord& building : buildings)
	{
		if (!isWorkplaceFor(building, colonist.job) || countReservedWorkers(building) >= static_cast<int>(building.slots.size()))
			continue;
		if (!productionBlocker(building).empty())
			continue;
		for (Cell access : building.access_cells)
		{
			if (grid.isWalkable(access) && isUsableAccess(building, access))
			{
				targets.push_back(access);
				owners.push_back(building.id);
			}
		}
	}
	// The nearest target wins; an outdoor job the suit cannot last is dropped (with every
	// other access cell of that building) and the search repeats (plan C6).
	Cell found;
	BuildingRecord* building = nullptr;
	while (!targets.empty())
	{
		if (!grid.findNearest(colonist.last_cell, targets, found, path_scratch))
			return false;
		BuildingId owner = INVALID_BUILDING;
		for (std::size_t i = 0; i < targets.size(); ++i)
			if (targets[i] == found)
			{
				owner = owners[i];
				break;
			}
		building = findBuildingMutable(owner);
		if (!building)
			return false;
		const BuildingDefinition* def = findDefinition(building->type);
		if (!def || def->isDome() || suitAllowsOutdoorJob(colonist, found, path_scratch))
			break;
		std::vector<Cell> kept_targets;
		std::vector<BuildingId> kept_owners;
		for (std::size_t i = 0; i < targets.size(); ++i)
			if (owners[i] != owner)
			{
				kept_targets.push_back(targets[i]);
				kept_owners.push_back(owners[i]);
			}
		targets.swap(kept_targets);
		owners.swap(kept_owners);
		building = nullptr;
	}
	if (!building)
		return false;
	int slot = 0;
	while (slot < static_cast<int>(building->slots.size()) && building->slots[static_cast<std::size_t>(slot)] != INVALID_COLONIST)
		++slot;

	// Reserve BEFORE walking (C6): the slot counts as taken for everyone else from now.
	colonist.has_idle_target = false;
	reserveSlot(colonist, *building, slot, found);
	if (!planRoute(colonist, found))
	{
		releaseReservation(colonist, "no route to " + describeBuilding(building->id));
		return false;
	}
	colonist.status = "walking to " + describeBuilding(building->id);
	if (colonist.route.finished())
		arriveAtWork(colonist); // already standing on the access cell
	return true;
}

std::string ColonySimulation::idleReason(const ColonistRecord& colonist) const
{
	bool any_workplace = false;
	bool any_free_slot = false;
	std::string blocked;
	for (const BuildingRecord& building : buildings)
	{
		if (!isWorkplaceFor(building, colonist.job))
			continue;
		any_workplace = true;
		if (countReservedWorkers(building) >= static_cast<int>(building.slots.size()))
			continue;
		any_free_slot = true;
		const std::string blocker = productionBlocker(building);
		if (!blocker.empty())
		{
			if (blocked.empty())
				blocked = describeBuilding(building.id) + ": " + blocker;
			continue;
		}
		if (usableAccessCells(building).empty())
		{
			if (blocked.empty())
				blocked = describeBuilding(building.id) + ": no entrance (connect it to a corridor)";
			continue;
		}
		const BuildingDefinition* def = findDefinition(building.type);
		Cell access;
		if (def && !def->isDome() && grid.findNearest(colonist.last_cell, usableAccessCells(building), access, return_scratch))
		{
			const std::vector<Cell> path = return_scratch;
			if (!suitAllowsOutdoorJob(colonist, access, path))
				return "suit oxygen too low for " + describeBuilding(building.id);
		}
		return "no route to " + describeBuilding(building.id);
	}
	if (!any_workplace)
		return std::string("no workplace for ") + roleName(colonist.job) + "s yet";
	if (!any_free_slot)
		return std::string("every ") + roleName(colonist.job) + " slot is taken";
	return blocked;
}

void ColonySimulation::arriveAtWork(ColonistRecord& colonist)
{
	colonist.route.clear();
	// Entering the access cell changes state only if the job is still available (C6.1).
	const BuildingRecord* building = findBuilding(colonist.reservation.building);
	if (!building || !isWorkplaceFor(*building, colonist.job) || !(colonist.last_cell == colonist.work_cell))
	{
		releaseReservation(colonist, "the job is no longer available");
		return;
	}
	const std::string blocker = productionBlocker(*building);
	if (!blocker.empty())
	{
		releaseReservation(colonist, describeBuilding(building->id) + ": " + blocker);
		return;
	}
	colonist.state = ColonistState::Working;
	colonist.status = "working at " + describeBuilding(building->id);
}

void ColonySimulation::updateWorker(ColonistRecord& colonist, float dt)
{
	const NeedRules& rules = getNeedRules();

	// 1. Validate the current reservation and job (C6.1 precedence after Dead).
	if (colonist.reservation.active())
	{
		const BuildingRecord* building = findBuilding(colonist.reservation.building);
		const int slot = colonist.reservation.slot;
		const bool still_owned = building && slot >= 0 && slot < static_cast<int>(building->slots.size()) &&
			building->slots[static_cast<std::size_t>(slot)] == colonist.id;
		if (!still_owned || !isWorkplaceFor(*building, colonist.job))
			releaseReservation(colonist, "the job is no longer available");
		else
		{
			const std::string blocker = productionBlocker(*building);
			if (!blocker.empty())
				releaseReservation(colonist, describeBuilding(building->id) + ": " + blocker);
		}
	}

	// 2. An active service must still exist (Storage disabled, water gone, door unpowered
	//    or no base oxygen); otherwise it ends and the need keeps draining.
	if (colonist.in_service && !isServiceStillValid(colonist))
		endService(colonist, false, std::string("the ") + needLabel(colonist.service_need) + " service stopped");

	// 3. A need at or below the threshold interrupts work for the best reachable service
	//    (C6.1). Without any service the colonist keeps its task, the need keeps draining
	//    and the search is retried after serviceRetrySeconds or a relevant revision.
	bool urgent = false;
	for (NeedType need : need_order)
		urgent = urgent || needValue(colonist.needs, need) <= rules.refill_threshold;
	if (!urgent)
	{
		colonist.need_alert.clear();
		colonist.service_retry_timer = 0.0f;
	}
	else if (!colonist.in_service)
	{
		colonist.service_retry_timer -= dt;
		if (colonist.service_retry_timer <= 0.0f || colonist.service_work_revision != work_revision ||
			colonist.service_grid_revision != grid.getRevision())
		{
			colonist.service_retry_timer = rules.service_retry_seconds;
			colonist.service_work_revision = work_revision;
			colonist.service_grid_revision = grid.getRevision();
			std::string reason;
			if (startService(colonist, reason))
				colonist.need_alert.clear();
			else
				colonist.need_alert = reason;
		}
	}

	// 4. A map change (door, obstacle, new building) may invalidate the current walk.
	if (!colonist.route.finished() && colonist.route_revision != grid.getRevision())
	{
		if (isRemainingRouteValid(colonist))
			colonist.route_revision = grid.getRevision();
		else if (colonist.reservation.active() && colonist.state == ColonistState::MovingToWork)
		{
			if (!planRoute(colonist, colonist.work_cell))
				releaseReservation(colonist, "no route to " + describeBuilding(colonist.reservation.building));
			else
				colonist.status = "walking to " + describeBuilding(colonist.reservation.building);
		}
		else if (colonist.in_service)
		{
			if (planRoute(colonist, colonist.service_cell))
				colonist.state = colonist.route.finished() ? ColonistState::Idle : ColonistState::MovingToService;
			else
				endService(colonist, false, "no route to " + describeBuilding(colonist.service_building));
			if (colonist.in_service && colonist.route.finished())
				arriveAtService(colonist);
		}
		else
		{
			// Idle walk (or the short stop step after a release): same goal or stop.
			if (planRoute(colonist, colonist.route_goal))
				colonist.state = ColonistState::Idle;
			else
			{
				colonist.has_idle_target = false;
				stopAtCellCentre(colonist);
			}
		}
	}

	// 5. Idle (no work, no service): the nearest eligible job when something relevant
	//    changed or every job_retry_seconds (no per-tick busy loop), otherwise wait inside
	//    (idle rule, plan C6).
	if (!colonist.reservation.active() && !colonist.in_service)
	{
		colonist.job_retry_timer -= dt;
		if (colonist.job_retry_timer <= 0.0f || colonist.evaluated_work_revision != work_revision ||
			colonist.evaluated_grid_revision != grid.getRevision())
		{
			colonist.job_retry_timer = job_retry_seconds;
			colonist.evaluated_work_revision = work_revision;
			colonist.evaluated_grid_revision = grid.getRevision();
			if (!selectJob(colonist))
			{
				colonist.state = ColonistState::Idle;
				updateIdleTarget(colonist, idleReason(colonist));
			}
		}
	}

	// 6. Move; arriving at the reserved access cell starts work, at the service cell refilling.
	if (!colonist.route.finished())
	{
		advanceAlongRoute(colonist, colonist.move_speed * dt);
		if (colonist.route.finished())
		{
			if (colonist.reservation.active() && colonist.state == ColonistState::MovingToWork)
				arriveAtWork(colonist);
			else if (colonist.in_service && colonist.state == ColonistState::MovingToService)
				arriveAtService(colonist);
			else
			{
				colonist.route.clear();
				if (colonist.has_idle_target && colonist.last_cell == colonist.idle_cell)
					updateIdleTarget(colonist, idleReason(colonist));
			}
		}
	}
}

// ---------------------------------------------------------------------------- needs and utilities (P06)

float ColonySimulation::needValue(const NeedValues& needs, NeedType need)
{
	switch (need)
	{
	case NeedType::Hunger: return needs.hunger;
	case NeedType::Thirst: return needs.thirst;
	case NeedType::Oxygen: break;
	}
	return needs.oxygen;
}

const char* ColonySimulation::needLabel(NeedType need)
{
	switch (need)
	{
	case NeedType::Hunger: return "food";
	case NeedType::Thirst: return "water";
	case NeedType::Oxygen: break;
	}
	return "oxygen";
}

int ColonySimulation::countLiving() const
{
	int living = 0;
	for (const ColonistRecord& colonist : colonists)
		living += colonist.state != ColonistState::Dead ? 1 : 0;
	return living;
}

void ColonySimulation::computeUtilities()
{
	// Plan C7, once per tick before needs, tasks and production: all-or-nothing power;
	// water from powered extractors allocated to drinkers first (lowest Thirst first, then
	// colonist ID), then Oxygenators, then the other consumers (Greenhouses), each in
	// building-ID order and only with its full flow; oxygen capacity of the operational
	// Oxygenators against every living colonist on an inside cell.
	const NeedRules& rules = getNeedRules();
	UtilitySnapshot snapshot;
	for (BuildingRecord& building : buildings)
	{
		building.powered = false;
		building.water_supplied = false;
		building.operational = false;
		const BuildingDefinition* def = findDefinition(building.type);
		if (!def || !building.isComplete() || !building.enabled)
			continue;
		snapshot.power_generated += def->power_output;
		snapshot.power_demand += def->power_demand;
	}
	snapshot.powered = snapshot.power_demand <= snapshot.power_generated + flow_epsilon;
	for (BuildingRecord& building : buildings)
	{
		const BuildingDefinition* def = findDefinition(building.type);
		if (!def || !building.isComplete() || !building.enabled)
			continue;
		building.powered = def->power_demand <= 0.0f || snapshot.powered;
		if (!building.powered)
			snapshot.stopped.push_back(building.id);
		else
			snapshot.water_produced += def->water_output;
	}

	float remaining = snapshot.water_produced;
	const float drink_flow = rules.drink_per_second * rules.water_per_thirst_point;
	std::vector<ColonistRecord*> drinkers;
	for (ColonistRecord& colonist : colonists)
	{
		colonist.water_allocated = false;
		if (colonist.state == ColonistState::Refilling && colonist.in_service && colonist.service_need == NeedType::Thirst)
			drinkers.push_back(&colonist);
	}
	std::stable_sort(drinkers.begin(), drinkers.end(), [](const ColonistRecord* a, const ColonistRecord* b) {
		if (a->needs.thirst != b->needs.thirst)
			return a->needs.thirst < b->needs.thirst;
		return a->id < b->id;
	});
	for (ColonistRecord* drinker : drinkers)
	{
		if (remaining + flow_epsilon >= drink_flow)
		{
			drinker->water_allocated = true;
			remaining -= drink_flow;
			snapshot.water_for_drinking += drink_flow;
			++snapshot.drinkers_served;
		}
		else
			++snapshot.drinkers_waiting;
	}
	for (int pass = 0; pass < 2; ++pass)
	{
		for (BuildingRecord& building : buildings)
		{
			const BuildingDefinition* def = findDefinition(building.type);
			if (!def || !building.isComplete() || !building.enabled || !building.powered || def->water_demand <= 0.0f)
				continue;
			if ((pass == 0) != (def->oxygen_capacity > 0))
				continue;
			if (remaining + flow_epsilon >= def->water_demand)
			{
				building.water_supplied = true;
				remaining -= def->water_demand;
				snapshot.water_for_buildings += def->water_demand;
			}
			else
				snapshot.stopped.push_back(building.id);
		}
	}

	std::uint64_t signature = 1469598103934665603ull;
	auto mix = [&signature](std::uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
	for (BuildingRecord& building : buildings)
	{
		const BuildingDefinition* def = findDefinition(building.type);
		if (!def || !building.isComplete() || !building.enabled)
			continue;
		if (def->water_demand <= 0.0f)
			building.water_supplied = true;
		building.operational = building.powered && building.water_supplied;
		if (building.operational)
			snapshot.oxygen_capacity += def->oxygen_capacity;
		mix(building.id);
		mix(building.operational ? 1u : 0u);
	}
	for (const ColonistRecord& colonist : colonists)
		if (colonist.state != ColonistState::Dead && grid.getZone(colonist.last_cell) == Zone::Inside)
			++snapshot.indoor_population;
	snapshot.oxygen_covered = snapshot.oxygen_capacity > 0 && snapshot.indoor_population <= snapshot.oxygen_capacity;
	mix(snapshot.oxygen_covered ? 1u : 0u);
	mix(snapshot.water_produced > flow_epsilon ? 1u : 0u);
	utilities = snapshot;
	// Operation changes can make jobs and services (in)eligible: idle colonists re-evaluate.
	if (signature != utility_signature)
	{
		utility_signature = signature;
		++work_revision;
	}
}

bool ColonySimulation::isSuppliedByMine(const ColonistRecord& colonist) const
{
	// A Miner working at a powered mine breathes the mine's local supply; this never adds
	// base capacity or refills anyone else (plan C6.1).
	if (colonist.state != ColonistState::Working || !colonist.reservation.active() || !colonist.route.finished())
		return false;
	const BuildingRecord* building = findBuilding(colonist.reservation.building);
	const BuildingDefinition* def = building ? findDefinition(building->type) : nullptr;
	return building && def && def->required_deposit && building->operational &&
		containsCell(building->access_cells, colonist.last_cell);
}

float ColonySimulation::currentDrain(const ColonistRecord& colonist, NeedType need) const
{
	const NeedRules& rules = getNeedRules();
	if (need != NeedType::Oxygen)
		return rules.drainFor(need);
	if (grid.getZone(colonist.last_cell) == Zone::Inside)
		return utilities.oxygen_covered ? 0.0f : rules.oxygen_drain;
	return isSuppliedByMine(colonist) ? 0.0f : rules.oxygen_drain;
}

void ColonySimulation::decayNeeds(ColonistRecord& colonist, float dt)
{
	// The temporary P03 scripted walker has no needs.
	if (colonist.state == ColonistState::Dead || colonist.has_debug_route)
		return;
	for (NeedType need : need_order)
	{
		float& value = needRef(colonist.needs, need);
		value = std::max(0.0f, value - currentDrain(colonist, need) * dt);
	}
	for (NeedType need : need_order)
		if (needValue(colonist.needs, need) <= 0.0f)
		{
			killColonist(colonist, need);
			return;
		}
}

void ColonySimulation::killColonist(ColonistRecord& colonist, NeedType cause)
{
	needRef(colonist.needs, cause) = 0.0f;
	colonist.in_service = false;
	colonist.service_building = INVALID_BUILDING;
	colonist.meal_taken = false;
	colonist.water_allocated = false;
	releaseReservation(colonist, "dead"); // frees the slot at once (C6)
	colonist.state = ColonistState::Dead;
	colonist.route.clear();
	colonist.current_dome = INVALID_BUILDING;
	colonist.has_idle_target = false;
	colonist.need_alert.clear();
	colonist.death_cause = cause == NeedType::Oxygen ? "lack of oxygen" : cause == NeedType::Thirst ? "thirst" : "hunger";
	colonist.status = "dead (" + colonist.death_cause + ")";
	++work_revision;
}

bool ColonySimulation::doorRefillsSuits(const BuildingRecord& building) const
{
	return building.door_index && building.isComplete() && building.enabled && grid.isDoorOpen(*building.door_index) &&
		building.powered && utilities.oxygen_covered;
}

std::vector<Cell> ColonySimulation::serviceTargets(NeedType need, std::vector<BuildingId>* owners) const
{
	const NeedRules& rules = getNeedRules();
	std::vector<Cell> cells;
	auto add = [&](Cell cell, BuildingId id) {
		if (!grid.isWalkable(cell))
			return;
		cells.push_back(cell);
		if (owners)
			owners->push_back(id);
	};
	for (const BuildingRecord& building : buildings)
	{
		const BuildingDefinition* def = findDefinition(building.type);
		if (!def || !building.isComplete() || !building.enabled)
			continue;
		switch (need)
		{
		case NeedType::Oxygen:
			if (doorRefillsSuits(building))
				for (const DoorPortal& crossing : building.crossings)
					add(crossing.inside, building.id);
			break;
		case NeedType::Thirst:
			if (utilities.water_produced > flow_epsilon && def->offersService(ServiceType::Drink))
				for (Cell cell : usableAccessCells(building))
					add(cell, building.id);
			break;
		case NeedType::Hunger:
			if (inventory.get(ResourceType::Vegetables) >= rules.vegetables_per_meal && def->offersService(ServiceType::Eat))
				for (Cell cell : usableAccessCells(building))
					add(cell, building.id);
			break;
		}
	}
	return cells;
}

std::string ColonySimulation::serviceUnavailableReason(NeedType need) const
{
	if (!serviceTargets(need).empty())
		return "";
	if (need == NeedType::Oxygen)
	{
		bool any_door = false;
		for (const BuildingRecord& building : buildings)
			any_door = any_door || (building.door_index && building.isComplete());
		if (!any_door)
			return "no Exit Door to refill the suit";
		if (!utilities.powered)
			return "no suit refill: the Exit Door has no power";
		if (utilities.oxygen_capacity == 0)
			return "no suit refill: no working Oxygenator";
		if (!utilities.oxygen_covered)
			return "no suit refill: Oxygenators cover " + std::to_string(utilities.oxygen_capacity) + " of " +
				std::to_string(utilities.indoor_population) + " indoors";
		return "no suit refill: the Exit Door is closed";
	}
	const ServiceType service = need == NeedType::Thirst ? ServiceType::Drink : ServiceType::Eat;
	bool any_point = false;
	bool any_entrance = false;
	for (const BuildingRecord& building : buildings)
	{
		const BuildingDefinition* def = findDefinition(building.type);
		if (!def || !building.isComplete() || !building.enabled || !def->offersService(service))
			continue;
		any_point = true;
		any_entrance = any_entrance || !usableAccessCells(building).empty();
	}
	if (!any_point)
		return need == NeedType::Thirst ? "no Storage to drink at" : "no Storage to eat at";
	if (!any_entrance)
		return "the Storage has no entrance (connect it to a corridor)";
	if (need == NeedType::Thirst)
		return "no water (needs a powered Water Extractor)";
	return "no vegetables";
}

bool ColonySimulation::startService(ColonistRecord& colonist, std::string& reason)
{
	// Several urgent needs: the one with the shortest time to zero after travel wins, tie
	// order Oxygen, Thirst, Hunger (plan C6/C6.1).
	const NeedRules& rules = getNeedRules();
	bool found = false;
	NeedType best_need = NeedType::Oxygen;
	Cell best_cell;
	BuildingId best_owner = INVALID_BUILDING;
	float best_score = 0.0f;
	std::string missing;
	const float speed = std::max(colonist.move_speed, 0.01f);
	for (NeedType need : need_order)
	{
		const float value = needValue(colonist.needs, need);
		if (value > rules.refill_threshold)
			continue;
		std::vector<BuildingId> owners;
		const std::vector<Cell> targets = serviceTargets(need, &owners);
		Cell cell;
		if (targets.empty() || !grid.findNearest(colonist.last_cell, targets, cell, path_scratch))
		{
			const std::string why = targets.empty() ? serviceUnavailableReason(need)
				: std::string("no route to a ") + needLabel(need) + " point";
			missing += (missing.empty() ? "" : "; ") + std::string("needs ") + needLabel(need) + ": " + why;
			continue;
		}
		const float drain = currentDrain(colonist, need);
		const float travel = static_cast<float>(path_scratch.size() > 0 ? path_scratch.size() - 1 : 0) * grid.getCellSize() / speed;
		const float score = drain > 0.0f ? value / drain - travel : std::numeric_limits<float>::max();
		if (!found || score < best_score)
		{
			found = true;
			best_score = score;
			best_need = need;
			best_cell = cell;
			for (std::size_t i = 0; i < targets.size(); ++i)
				if (targets[i] == cell)
				{
					best_owner = owners[i];
					break;
				}
		}
	}
	if (!found)
	{
		reason = missing;
		return false;
	}

	// Release the work slot before going to the service (C6.1).
	if (colonist.reservation.active())
		releaseReservation(colonist, std::string("needs ") + needLabel(best_need));
	colonist.has_idle_target = false;
	colonist.in_service = true;
	colonist.service_need = best_need;
	colonist.service_building = best_owner;
	colonist.service_cell = best_cell;
	colonist.service_timer = 0.0f;
	colonist.meal_taken = false;
	colonist.water_allocated = false;
	if (!planRoute(colonist, best_cell))
	{
		reason = "no route to " + describeBuilding(best_owner);
		endService(colonist, false, reason);
		return false;
	}
	if (colonist.route.finished())
	{
		arriveAtService(colonist);
		return colonist.in_service;
	}
	colonist.state = ColonistState::MovingToService;
	const char* verb = best_need == NeedType::Oxygen ? "refill the suit" : best_need == NeedType::Thirst ? "drink" : "eat";
	colonist.status = std::string("going to ") + verb + " at " + describeBuilding(best_owner);
	return true;
}

bool ColonySimulation::isServiceStillValid(const ColonistRecord& colonist) const
{
	const BuildingRecord* building = findBuilding(colonist.service_building);
	const BuildingDefinition* def = building ? findDefinition(building->type) : nullptr;
	if (!building || !def || !building->isComplete() || !building->enabled)
		return false;
	switch (colonist.service_need)
	{
	case NeedType::Oxygen:
	{
		if (!doorRefillsSuits(*building))
			return false;
		for (const DoorPortal& crossing : building->crossings)
			if (crossing.inside == colonist.service_cell)
				return true;
		return false;
	}
	case NeedType::Thirst:
		return def->offersService(ServiceType::Drink) && utilities.water_produced > flow_epsilon &&
			containsCell(building->access_cells, colonist.service_cell) && isUsableAccess(*building, colonist.service_cell);
	case NeedType::Hunger:
		return def->offersService(ServiceType::Eat) && containsCell(building->access_cells, colonist.service_cell) &&
			isUsableAccess(*building, colonist.service_cell) &&
			(colonist.meal_taken || inventory.get(ResourceType::Vegetables) >= getNeedRules().vegetables_per_meal);
	}
	return false;
}

void ColonySimulation::arriveAtService(ColonistRecord& colonist)
{
	colonist.route.clear();
	// Entering the service cell changes state only if the service is still there (C6.1).
	if (!(colonist.last_cell == colonist.service_cell) || !isServiceStillValid(colonist))
	{
		endService(colonist, false, std::string("the ") + needLabel(colonist.service_need) + " service is no longer available");
		return;
	}
	if (colonist.service_need == NeedType::Hunger && !colonist.meal_taken)
	{
		ResourceAmounts meal;
		meal[ResourceType::Vegetables] = getNeedRules().vegetables_per_meal;
		if (inventory.apply(meal, ResourceAmounts()) != Inventory::Refusal::None)
		{
			endService(colonist, false, "no vegetables left");
			return;
		}
		colonist.meal_taken = true;
		++work_revision; // inventory changed
	}
	colonist.state = ColonistState::Refilling;
	const std::string where = describeBuilding(colonist.service_building);
	switch (colonist.service_need)
	{
	case NeedType::Oxygen: colonist.status = "refilling the suit at " + where; break;
	case NeedType::Thirst: colonist.status = "drinking at " + where; break;
	case NeedType::Hunger: colonist.status = "eating at " + where; break;
	}
}

void ColonySimulation::endService(ColonistRecord& colonist, bool finished, const std::string& reason)
{
	colonist.in_service = false;
	colonist.service_building = INVALID_BUILDING;
	colonist.service_timer = 0.0f;
	colonist.meal_taken = false;
	colonist.water_allocated = false;
	if (colonist.state != ColonistState::Dead)
		colonist.state = ColonistState::Idle;
	colonist.current_dome = INVALID_BUILDING;
	colonist.has_idle_target = false;
	colonist.job_retry_timer = 0.0f; // look for work (or the next need) at once
	colonist.service_retry_timer = finished ? 0.0f : getNeedRules().service_retry_seconds;
	colonist.status = finished ? reason : "idle: " + reason;
	stopAtCellCentre(colonist);
}

void ColonySimulation::performServices(float dt)
{
	const NeedRules& rules = getNeedRules();
	for (ColonistRecord& colonist : colonists)
	{
		if (colonist.state == ColonistState::Dead)
			continue;
		if (colonist.in_service && colonist.state == ColonistState::Refilling)
		{
			if (!isServiceStillValid(colonist))
			{
				endService(colonist, false, std::string("the ") + needLabel(colonist.service_need) + " service stopped");
				continue;
			}
			const std::string where = describeBuilding(colonist.service_building);
			switch (colonist.service_need)
			{
			case NeedType::Thirst:
				// Water flows only to drinkers allocated in this tick's snapshot (C7).
				if (colonist.water_allocated)
				{
					colonist.needs.thirst = std::min(100.0f, colonist.needs.thirst + rules.drink_per_second * dt);
					colonist.status = "drinking at " + where;
				}
				else
					colonist.status = "waiting for water at " + where;
				if (colonist.needs.thirst >= 100.0f - 1e-4f)
				{
					colonist.needs.thirst = 100.0f;
					endService(colonist, true, "finished drinking");
				}
				break;
			case NeedType::Hunger:
				colonist.service_timer += dt;
				if (colonist.service_timer + 1e-4f >= rules.eat_seconds)
				{
					colonist.needs.hunger = 100.0f;
					endService(colonist, true, "finished eating");
				}
				break;
			case NeedType::Oxygen:
				colonist.needs.oxygen = std::min(100.0f, colonist.needs.oxygen + rules.suit_refill_per_second * dt);
				if (colonist.needs.oxygen >= 100.0f - 1e-4f)
				{
					colonist.needs.oxygen = 100.0f;
					endService(colonist, true, "suit refilled");
				}
				break;
			}
			continue;
		}
		// Passing through the chamber of a powered airlock while the base has oxygen also
		// tops the suit up (the inward-crossing refill of plan C6.1).
		const BuildingRecord* building = buildingAt(colonist.last_cell);
		if (building && containsCell(building->inside_cells, colonist.last_cell) && doorRefillsSuits(*building))
			colonist.needs.oxygen = std::min(100.0f, colonist.needs.oxygen + rules.suit_refill_per_second * dt);
	}
}

bool ColonySimulation::suitAllowsOutdoorJob(const ColonistRecord& colonist, Cell job_cell, const std::vector<Cell>& path_to_job) const
{
	// Outside cells on the way out plus the way back to the nearest suit refill point must
	// leave the suit above the threshold (plan C6). Without any refill point the return
	// trip cannot be planned, so only the way out counts.
	const NeedRules& rules = getNeedRules();
	auto outsideCells = [this](const std::vector<Cell>& path) {
		int count = 0;
		for (std::size_t i = 1; i < path.size(); ++i)
			count += grid.getZone(path[i]) == Zone::Outside ? 1 : 0;
		return count;
	};
	int cells = outsideCells(path_to_job);
	const std::vector<Cell> refill = serviceTargets(NeedType::Oxygen);
	if (!refill.empty())
	{
		Cell target;
		if (!grid.findNearest(job_cell, refill, target, return_scratch))
			return false;
		cells += outsideCells(return_scratch);
	}
	const float seconds = static_cast<float>(cells) * grid.getCellSize() / std::max(colonist.move_speed, 0.01f);
	return colonist.needs.oxygen - rules.oxygen_drain * seconds > rules.refill_threshold;
}

bool ColonySimulation::isIdleTargetValid(const ColonistRecord& colonist) const
{
	if (!grid.isWalkable(colonist.idle_cell))
		return false;
	if (colonist.idle_dome != INVALID_BUILDING)
	{
		const BuildingRecord* dome = findBuilding(colonist.idle_dome);
		return dome && dome->isComplete() && containsCell(usableAccessCells(*dome), colonist.idle_cell);
	}
	return grid.getZone(colonist.idle_cell) == Zone::Inside && !isDoorCell(colonist.idle_cell);
}

void ColonySimulation::updateIdleTarget(ColonistRecord& colonist, const std::string& reason)
{
	// Idle rule (plan C6): wait at the nearest reachable entrance of a completed dome
	// (inside it, hidden); with no dome, on the nearest inside cell that is not a door
	// cell; with no reachable inside cell, where it stands.
	const bool heading_there = colonist.route.finished() ? colonist.last_cell == colonist.idle_cell
		: colonist.route_goal == colonist.idle_cell;
	const bool keep = colonist.has_idle_target && colonist.idle_grid_revision == grid.getRevision() &&
		heading_there && isIdleTargetValid(colonist);
	if (!keep)
	{
		colonist.has_idle_target = false;
		std::vector<Cell> targets;
		std::vector<BuildingId> owners;
		for (const BuildingRecord& building : buildings)
		{
			const BuildingDefinition* def = findDefinition(building.type);
			if (!def || !def->isDome() || !building.isComplete())
				continue;
			for (Cell cell : usableAccessCells(building))
				if (grid.isWalkable(cell))
				{
					targets.push_back(cell);
					owners.push_back(building.id);
				}
		}
		if (targets.empty())
			for (const BuildingRecord& building : buildings)
				for (Cell cell : building.inside_cells)
					if (grid.getZone(cell) == Zone::Inside && grid.isWalkable(cell) && !isDoorCell(cell) && !containsCell(targets, cell))
					{
						targets.push_back(cell);
						owners.push_back(INVALID_BUILDING);
					}
		Cell found;
		if (!targets.empty() && grid.findNearest(colonist.last_cell, targets, found, path_scratch))
		{
			colonist.has_idle_target = true;
			colonist.idle_cell = found;
			colonist.idle_dome = INVALID_BUILDING;
			for (std::size_t i = 0; i < targets.size(); ++i)
				if (targets[i] == found)
				{
					colonist.idle_dome = owners[i];
					break;
				}
			colonist.idle_grid_revision = grid.getRevision();
			if (!(colonist.route.finished() && colonist.last_cell == found) && !planRoute(colonist, found))
			{
				colonist.has_idle_target = false;
				stopAtCellCentre(colonist);
			}
			colonist.state = ColonistState::Idle;
		}
	}

	const std::string alert = colonist.need_alert.empty() ? std::string() : colonist.need_alert + "; ";
	if (!colonist.has_idle_target)
	{
		colonist.status = alert + "idle: " + reason;
		return;
	}
	const std::string where = colonist.idle_dome != INVALID_BUILDING ? describeBuilding(colonist.idle_dome) : std::string("the base");
	const bool arrived = colonist.route.finished() && colonist.last_cell == colonist.idle_cell;
	colonist.status = alert + "idle: " + reason + (arrived ? " - waiting in " : " - going to wait in ") + where;
}

// ---------------------------------------------------------------------------- buildings (P05)

PlacementResult ColonySimulation::validatePlacement(BuildingType type, Cell origin, int quarter_turns) const
{
	PlacementResult result;
	auto fail = [&result](const std::string& code, const std::string& reason) {
		result.valid = false;
		result.code = code;
		result.reason = reason;
		return result;
	};
	const BuildingDefinition* def = findDefinition(type);
	if (!def)
		return fail("unknown_type", "unknown building type");

	// Shared quarter-turn transform (C5): cells, access offsets, inside cells and door
	// crossings rotate together.
	const int q = footprint::normalizeQuarterTurns(quarter_turns);
	footprint::rotatedSize(def->footprint_width, def->footprint_height, q, result.width, result.height);
	auto place = [&](Cell offset) {
		const Cell rotated = footprint::rotateOffset(offset, def->footprint_width, def->footprint_height, q);
		return Cell{ origin.x + rotated.x, origin.z + rotated.z };
	};
	for (int dz = 0; dz < def->footprint_height; ++dz)
		for (int dx = 0; dx < def->footprint_width; ++dx)
			result.footprint_cells.push_back(place(Cell{ dx, dz }));
	for (Cell offset : def->inside_offsets)
		result.inside_cells.push_back(place(offset));
	for (const DoorPortal& crossing : def->crossings)
		result.crossings.push_back(DoorPortal{ place(crossing.outside), place(crossing.inside) });
	const bool walkable_footprint = def->hasWalkableFootprint();
	bool access_off_map = false;
	for (Cell access : def->access_offsets)
	{
		const Cell cell = place(access);
		if (!grid.inBounds(cell))
		{
			// A corridor keeps the neighbours that exist; other buildings need all of them.
			access_off_map = access_off_map || !walkable_footprint;
			continue;
		}
		result.access_cells.push_back(cell);
	}

	for (Cell cell : result.footprint_cells)
		if (!grid.inBounds(cell))
			return fail("out_of_bounds", "the footprint leaves the map");
	if (access_off_map)
		return fail("access_blocked", "an access cell is outside the map");

	// Deposits: only a mine may cover one, and it must cover exactly one free deposit of
	// its own resource; that deposit cell becomes part of its footprint (C5).
	std::vector<std::size_t> covered;
	for (std::size_t i = 0; i < scenario.deposits.size(); ++i)
		if (containsCell(result.footprint_cells, scenario.deposits[i].cell))
			covered.push_back(i);
	if (def->required_deposit)
	{
		std::size_t matching = 0;
		for (std::size_t index : covered)
		{
			if (scenario.deposits[index].type != *def->required_deposit)
				return fail("wrong_deposit", def->label + " covers a deposit of the wrong resource");
			++matching;
			result.deposit_index = index;
		}
		if (matching != 1)
			return fail("needs_deposit", def->label + " must cover exactly one deposit of its resource");
		for (const BuildingRecord& other : buildings)
			if (other.deposit_index == result.deposit_index)
				return fail("deposit_taken", "that deposit already has " + describeBuilding(other.id));
	}
	else if (!covered.empty())
		return fail("deposit_in_the_way", "only a mine can be built on a deposit");

	for (Cell cell : result.footprint_cells)
	{
		if (const BuildingRecord* other = buildingAt(cell))
			return fail("occupied", "the footprint overlaps " + describeBuilding(other->id) + " at " + cellText(cell));
		for (const InitialColonistDefinition& spawn : scenario.initial_colonists)
			if (spawn.cell == cell && !walkable_footprint) // colonists may start in a corridor
				return fail("spawn_cell", "the footprint covers a colonist spawn cell " + cellText(cell));
		const bool own_deposit = result.deposit_index && scenario.deposits[*result.deposit_index].cell == cell;
		if (grid.isBlocked(cell) && !own_deposit)
			return fail("occupied", "the footprint overlaps a blocked cell at " + cellText(cell));
	}
	// Access cells of other buildings stay free, except that a corridor may run over a
	// dome's entrance candidates (that is how a dome gets its entrance) and over another
	// corridor's neighbours. A completed corridor needs no access cell any more.
	for (const BuildingRecord& other : buildings)
	{
		const BuildingDefinition* other_def = findDefinition(other.type);
		const bool other_corridor = other_def && other_def->placement == PlacementKind::Corridor;
		if (other_corridor && other.isComplete())
			continue;
		const bool corridor_allowed = def->placement == PlacementKind::Corridor && other_def && (other_def->isDome() || other_corridor);
		for (Cell access : other.access_cells)
			if (containsCell(result.footprint_cells, access) && !corridor_allowed)
				return fail("blocks_access", "it would cover an access cell of " + describeBuilding(other.id));
	}
	if (!walkable_footprint)
		for (const ColonistRecord& colonist : colonists)
		{
			if (colonist.state == ColonistState::Dead)
				continue;
			if (containsCell(result.footprint_cells, colonist.last_cell) ||
				(!colonist.route.finished() && containsCell(result.footprint_cells, colonist.route.cells[colonist.route.next_index])))
				return fail("colonist_in_the_way", colonist.name + " is standing there");
		}

	// The base network (C5): corridors grow from the network, domes need an entrance
	// candidate on it, and everything else keeps its cells off it.
	auto onNetwork = [this](Cell cell) { return isNetworkCell(cell) || grid.getZone(cell) == Zone::Inside; };
	if (def->placement == PlacementKind::Corridor)
	{
		// It joins next to an Exit Door chamber or another corridor, or on an entrance
		// candidate of a dome (walkable domes, P06: the dome opens onto it).
		bool joined = false;
		const Cell cell = result.footprint_cells.front();
		for (const Cell neighbour : { Cell{ cell.x + 1, cell.z }, Cell{ cell.x - 1, cell.z }, Cell{ cell.x, cell.z + 1 }, Cell{ cell.x, cell.z - 1 } })
			joined = joined || isPassageCell(neighbour);
		for (const BuildingRecord& other : buildings)
		{
			const BuildingDefinition* other_def = findDefinition(other.type);
			joined = joined || (other_def && other_def->isDome() && containsCell(other.access_cells, cell));
		}
		if (!joined)
			return fail("needs_network", "a corridor must join the base: build it next to an Exit Door chamber, another corridor or a dome entrance");
	}
	else if (def->isDome())
	{
		bool entrance = false;
		for (Cell access : result.access_cells)
		{
			// A dome is entered from a corridor or a chamber, never from another dome.
			const BuildingRecord* other = buildingAt(access);
			const BuildingDefinition* other_def = other ? findDefinition(other->type) : nullptr;
			if (other && other_def && !other_def->hasWalkableFootprint())
				return fail("access_blocked", "an entrance of " + def->label + " would open into " + describeBuilding(other->id));
			entrance = entrance || isPassageCell(access);
		}
		if (!entrance)
			return fail("needs_network", def->label + " needs an entrance on a corridor or an Exit Door chamber");
	}
	else
	{
		// Exterior buildings and the airlock's own access cells stay outside the network.
		for (Cell access : result.access_cells)
			if (onNetwork(access))
				return fail("on_network", def->label + " must be reached from outside the base");
		if (def->zone == BuildingZone::Exterior)
			for (Cell cell : result.footprint_cells)
				if (onNetwork(cell))
					return fail("on_network", def->label + " must be built outside the base");
	}

	// Access cells must be walkable: every one for a blocked footprint, at least one for a
	// corridor or an airlock (built from any free neighbour).
	bool any_walkable = false;
	for (Cell access : result.access_cells)
	{
		const bool walkable = grid.isWalkable(access) && !containsCell(result.footprint_cells, access);
		any_walkable = any_walkable || walkable;
		if (!walkable && !walkable_footprint)
			return fail("access_blocked", "access cell " + cellText(access) + " is blocked");
	}
	if (!any_walkable)
		return fail("access_blocked", def->label + " has no free access cell");

	result.valid = true;
	result.code = "ok";
	result.reason = "valid placement";
	return result;
}

BuildingId ColonySimulation::addCompleteBuilding(BuildingType type, const PlacementResult& placement, int quarter_turns, Cell origin)
{
	const BuildingDefinition* def = findDefinition(type);
	BuildingRecord building;
	building.id = next_building_id++;
	building.type = type;
	building.construction = ConstructionState::Complete;
	building.origin = origin;
	building.quarter_turns = footprint::normalizeQuarterTurns(quarter_turns);
	building.width = placement.width;
	building.height = placement.height;
	building.footprint_cells = placement.footprint_cells;
	building.access_cells = placement.access_cells;
	building.inside_cells = placement.inside_cells;
	building.crossings = placement.crossings;
	building.deposit_index = placement.deposit_index;
	building.slots.assign(def && def->has_operation ? static_cast<std::size_t>(def->operation_slots) : 0u, INVALID_COLONIST);

	// Completion effects (C5): domes and exterior buildings block their footprint;
	// corridors and airlocks turn their inside cells inside, and an airlock registers its
	// crossings as one open door. Each change bumps the grid revision, so routes replan.
	if (def && def->isDome())
	{
		// Walkable domes (team decision, P06): a completed dome is an inside room that
		// colonists may cross, entered and left only through its entrances (room
		// openings between each entrance candidate and its footprint neighbour). Walking
		// through it is hidden; the inside is still not simulated as space.
		for (Cell cell : building.footprint_cells)
		{
			grid.setZone(cell, Zone::Inside);
			grid.setRoom(cell, building.id);
		}
		for (Cell access : building.access_cells)
			for (Cell cell : building.footprint_cells)
				if (std::abs(cell.x - access.x) + std::abs(cell.z - access.z) == 1)
					grid.addRoomOpening(access, cell);
	}
	else if (!def || !def->hasWalkableFootprint())
		for (Cell cell : building.footprint_cells)
			grid.setBlocked(cell, true);
	for (Cell cell : building.inside_cells)
		grid.setZone(cell, Zone::Inside);
	if (!building.crossings.empty())
		building.door_index = grid.addDoor(building.crossings, true);

	buildings.push_back(building);
	++work_revision;
	recomputeCapacity();
	return building.id;
}

void ColonySimulation::recomputeCapacity()
{
	// 100 item units per completed Storage (F3); the temporary cargo capacity applies only
	// until the first one completes. A lower capacity never deletes items.
	int storage_capacity = 0;
	for (const BuildingRecord& building : buildings)
	{
		const BuildingDefinition* def = findDefinition(building.type);
		if (def && building.isComplete() && building.enabled)
			storage_capacity += def->storage_capacity;
	}
	uses_cargo_capacity = storage_capacity == 0;
	inventory.setCapacity(uses_cargo_capacity ? scenario.cargo_capacity : storage_capacity);
}

void ColonySimulation::updateProduction(BuildingRecord& building, float dt)
{
	const BuildingDefinition* def = findDefinition(building.type);
	if (!def)
		return;
	if (!building.isComplete())
	{
		building.status = BuildingStatus::Idle;
		building.status_text = "blueprint";
		return;
	}
	if (!building.enabled)
	{
		building.status = BuildingStatus::Disabled;
		building.status_text = "disabled";
		return;
	}
	const bool no_entrance = def->isDome() && usableAccessCells(building).empty();
	if (!def->recipe)
	{
		building.status = BuildingStatus::Idle;
		if (!building.powered)
			building.status = BuildingStatus::NoPower;
		else if (!building.water_supplied)
			building.status = BuildingStatus::NoWater;
		const std::string shortage = !building.powered ? "no power" : !building.water_supplied ? "no water" : "";
		std::string text;
		if (def->storage_capacity > 0)
			text = "+" + std::to_string(def->storage_capacity) + " item capacity; food and water point" +
				(no_entrance ? "; no entrance yet (connect it to a corridor)" : "");
		else if (building.door_index)
		{
			// An unpowered door stays passable; power and base oxygen only gate the refill.
			text = grid.isDoorOpen(*building.door_index) ? "door open" : "door closed";
			if (doorRefillsSuits(building))
				text += "; refills suits";
			else if (!building.powered)
				text += "; no suit refill (no power)";
			else if (!utilities.oxygen_covered)
				text += "; no suit refill (no base oxygen)";
		}
		else if (def->placement == PlacementKind::Corridor)
			text = "corridor (inside)";
		else if (!shortage.empty())
			text = shortage;
		else if (def->power_output > 0.0f)
			text = "generating " + formatFlow(def->power_output) + " power";
		else if (def->water_output > 0.0f)
			text = "pumping " + formatFlow(def->water_output) + " water/s";
		else if (def->oxygen_capacity > 0)
			text = "air for " + std::to_string(def->oxygen_capacity) + " colonists (" + std::to_string(utilities.indoor_population) +
				" indoors, capacity " + std::to_string(utilities.oxygen_capacity) + ")";
		else
			text = "complete";
		if (no_entrance && def->storage_capacity == 0)
			text += "; no entrance (connect it to a corridor)";
		building.status_text = text;
		return;
	}

	// C7 atomic production: accumulate work time from PRESENT workers only; commit each
	// cycle's inputs and outputs together, or keep at most one ready cycle and stop.
	const RecipeDefinition& recipe = *def->recipe;
	const int present = countPresentWorkers(building);
	const int rate = recipe.per_present_worker ? present : (present > 0 ? 1 : 0);
	BuildingStatus blocked_status = BuildingStatus::Running;
	const std::string blocker = productionBlocker(building, &blocked_status);
	if (!blocker.empty())
	{
		building.production_progress = std::min(building.production_progress + dt * static_cast<float>(rate), recipe.seconds);
		building.status = blocked_status;
		building.status_text = blocker;
		return;
	}
	if (rate == 0)
	{
		building.status = BuildingStatus::NoWorkers;
		building.status_text = no_entrance ? std::string("no entrance (connect it to a corridor)")
			: std::string("no ") + roleName(def->operation_role) + " present";
		return;
	}

	building.status = BuildingStatus::Running;
	building.status_text = "running";
	building.production_progress += dt * static_cast<float>(rate);
	while (building.production_progress + production_epsilon >= recipe.seconds)
	{
		BuildingStatus status = BuildingStatus::Running;
		const std::string refused = productionBlocker(building, &status);
		if (!refused.empty() || inventory.apply(recipe.inputs, recipe.outputs) != Inventory::Refusal::None)
		{
			building.production_progress = recipe.seconds; // one ready cycle, nothing deleted
			building.status = status;
			building.status_text = refused.empty() ? "blocked" : refused;
			break;
		}
		building.production_progress = std::max(0.0f, building.production_progress - recipe.seconds);
		++building.cycles_completed;
		++work_revision; // inventory changed: other jobs may have become eligible
	}
}
