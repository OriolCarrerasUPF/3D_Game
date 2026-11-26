#include "colony_simulation.h"

#include "world_transform.h"

#include <algorithm>
#include <cmath>

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

	// Initial (complete) buildings, validated with the same placement rules as P07.
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
		PlacementResult placement;
		if (initial.on_socket)
		{
			if (def->placement != PlacementKind::DoorSocket)
			{
				error = pointer + ": '" + def->id + "' is a footprint building (give origin + quarterTurns)";
				reset();
				return false;
			}
			std::size_t door_index = 0;
			while (door_index < scenario.exit_doors.size() && scenario.exit_doors[door_index].id != initial.socket_id)
				++door_index;
			placement = validateDoorPlacement(door_index);
		}
		else
		{
			if (def->placement != PlacementKind::Footprint)
			{
				error = pointer + ": '" + def->id + "' is built on a door socket (give socket)";
				reset();
				return false;
			}
			placement = validatePlacement(def->type, initial.origin, initial.quarter_turns);
		}
		if (!placement.valid)
		{
			error = pointer + ": " + def->label + " cannot be placed: " + placement.reason;
			reset();
			return false;
		}
		addCompleteBuilding(def->type, placement, initial.quarter_turns, initial.origin);
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
	// 1. Colonists in ID order: reservations, job choice, movement, arrival.
	for (ColonistRecord& colonist : colonists)
		updateColonist(colonist, dt);
	// 2. Buildings in ID order: production from the workers present now.
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
	for (const ExitDoorDefinition& door : scenario.exit_doors)
		for (const DoorPortal& portal : door.crossings)
			if (portal.inside == cell || portal.outside == cell)
				return true;
	return false;
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
		// Release first (C6.1): the old slot is free before the new role looks for work.
		releaseReservation(*colonist, "role changed");
		colonist->job = command.job;
		colonist->move_speed = role->move_speed;
		colonist->job_retry_timer = 0.0f;
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
	colonist.job_retry_timer = job_retry_seconds;
	colonist.status = "idle: " + reason;

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
			if (grid.isWalkable(access))
			{
				targets.push_back(access);
				owners.push_back(building.id);
			}
		}
	}
	if (targets.empty())
		return false;

	Cell found;
	if (!grid.findNearest(colonist.last_cell, targets, found, path_scratch))
		return false;
	BuildingId owner = INVALID_BUILDING;
	for (std::size_t i = 0; i < targets.size(); ++i)
		if (targets[i] == found)
		{
			owner = owners[i];
			break;
		}
	BuildingRecord* building = findBuildingMutable(owner);
	if (!building)
		return false;
	int slot = 0;
	while (slot < static_cast<int>(building->slots.size()) && building->slots[static_cast<std::size_t>(slot)] != INVALID_COLONIST)
		++slot;

	// Reserve BEFORE walking (C6): the slot counts as taken for everyone else from now.
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

	// 2. A map change (door, obstacle, new building) may invalidate the walk to work.
	if (colonist.reservation.active() && colonist.state == ColonistState::MovingToWork &&
		!colonist.route.finished() && colonist.route_revision != grid.getRevision())
	{
		if (isRemainingRouteValid(colonist))
			colonist.route_revision = grid.getRevision();
		else if (!planRoute(colonist, colonist.work_cell))
			releaseReservation(colonist, "no route to " + describeBuilding(colonist.reservation.building));
		else
			colonist.status = "walking to " + describeBuilding(colonist.reservation.building);
	}

	// 3. Idle and standing on a cell centre: look for the nearest eligible job when
	//    something relevant changed or every job_retry_seconds (no per-tick busy loop).
	if (!colonist.reservation.active() && colonist.route.finished())
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
				colonist.status = "idle: " + idleReason(colonist);
			}
		}
	}

	// 4. Move; arriving at the reserved access cell starts work.
	if (!colonist.route.finished())
	{
		advanceAlongRoute(colonist, colonist.move_speed * dt);
		if (colonist.route.finished())
		{
			if (colonist.reservation.active() && colonist.state == ColonistState::MovingToWork)
				arriveAtWork(colonist);
			else
				colonist.route.clear();
		}
	}
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
	if (def->placement != PlacementKind::Footprint)
		return fail("wrong_placement", def->label + " is built on a door socket");

	// Shared quarter-turn transform (C5): cells and access offsets rotate together.
	const int q = footprint::normalizeQuarterTurns(quarter_turns);
	footprint::rotatedSize(def->footprint_width, def->footprint_height, q, result.width, result.height);
	for (int dz = 0; dz < def->footprint_height; ++dz)
		for (int dx = 0; dx < def->footprint_width; ++dx)
		{
			const Cell offset = footprint::rotateOffset(Cell{ dx, dz }, def->footprint_width, def->footprint_height, q);
			result.footprint_cells.push_back(Cell{ origin.x + offset.x, origin.z + offset.z });
		}
	for (Cell access : def->access_offsets)
	{
		const Cell offset = footprint::rotateOffset(access, def->footprint_width, def->footprint_height, q);
		result.access_cells.push_back(Cell{ origin.x + offset.x, origin.z + offset.z });
	}

	for (Cell cell : result.footprint_cells)
		if (!grid.inBounds(cell))
			return fail("out_of_bounds", "the footprint leaves the map");
	for (Cell cell : result.access_cells)
		if (!grid.inBounds(cell))
			return fail("access_blocked", "an access cell is outside the map");

	const Zone wanted = def->zone == BuildingZone::Interior ? Zone::Inside : Zone::Outside;
	for (const std::vector<Cell>* cells : { &result.footprint_cells, &result.access_cells })
		for (Cell cell : *cells)
			if (grid.getZone(cell) != wanted)
				return fail("wrong_zone", def->label + (wanted == Zone::Inside ? " must be built inside the base" : " must be built outside the base"));

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
		if (isDoorCell(cell))
			return fail("door_cells", "the footprint covers Exit Door crossing cells");
		for (const InitialColonistDefinition& spawn : scenario.initial_colonists)
			if (spawn.cell == cell)
				return fail("spawn_cell", "the footprint covers a colonist spawn cell " + cellText(cell));
		const bool own_deposit = result.deposit_index && scenario.deposits[*result.deposit_index].cell == cell;
		if (grid.isBlocked(cell) && !own_deposit)
			return fail("occupied", "the footprint overlaps a building or a blocked cell at " + cellText(cell));
	}
	for (const BuildingRecord& other : buildings)
		for (Cell access : other.access_cells)
			if (containsCell(result.footprint_cells, access))
				return fail("blocks_access", "it would cover an access cell of " + describeBuilding(other.id));
	for (const ColonistRecord& colonist : colonists)
	{
		if (colonist.state == ColonistState::Dead)
			continue;
		if (containsCell(result.footprint_cells, colonist.last_cell) ||
			(!colonist.route.finished() && containsCell(result.footprint_cells, colonist.route.cells[colonist.route.next_index])))
			return fail("colonist_in_the_way", colonist.name + " is standing there");
	}
	for (Cell access : result.access_cells)
		if (grid.isBlocked(access))
			return fail("access_blocked", "access cell " + cellText(access) + " is blocked");

	result.valid = true;
	result.code = "ok";
	result.reason = "valid placement";
	return result;
}

PlacementResult ColonySimulation::validateDoorPlacement(std::size_t door_index) const
{
	PlacementResult result;
	result.code = "unknown_socket";
	result.reason = "unknown door socket";
	if (!findDefinition(BuildingType::ExitDoor))
	{
		result.code = "unknown_type";
		result.reason = "buildings.json has no Exit Door";
		return result;
	}
	if (door_index >= scenario.exit_doors.size() || door_index >= grid.getDoorCount())
		return result;
	for (const BuildingRecord& other : buildings)
		if (other.door_index == door_index)
		{
			result.code = "socket_taken";
			result.reason = "the socket already has " + describeBuilding(other.id);
			return result;
		}
	// The crossing cells are the door's access cells (outside first: the first Door is
	// built from the exterior side, P07).
	for (const DoorPortal& portal : scenario.exit_doors[door_index].crossings)
		result.access_cells.push_back(portal.outside);
	for (const DoorPortal& portal : scenario.exit_doors[door_index].crossings)
		result.access_cells.push_back(portal.inside);
	result.door_index = door_index;
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
	building.door_index = placement.door_index;
	building.deposit_index = placement.deposit_index;
	building.slots.assign(def && def->has_operation ? static_cast<std::size_t>(def->operation_slots) : 0u, INVALID_COLONIST);

	// Footprints block their cells (each change bumps the grid revision); a completed
	// Exit Door opens its socket.
	for (Cell cell : building.footprint_cells)
		grid.setBlocked(cell, true);
	if (building.door_index)
		grid.setDoorOpen(*building.door_index, true);

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
	if (!def->recipe)
	{
		building.status = BuildingStatus::Idle;
		if (def->storage_capacity > 0)
			building.status_text = "+" + std::to_string(def->storage_capacity) + " item capacity";
		else if (building.door_index)
			building.status_text = grid.isDoorOpen(*building.door_index) ? "door open" : "door closed";
		else
			building.status_text = "complete (utilities arrive in P06)";
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
		building.status_text = std::string("no ") + roleName(def->operation_role) + " present";
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
