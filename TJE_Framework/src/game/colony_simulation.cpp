#include "colony_simulation.h"

#include "world_transform.h"

#include <cmath>

namespace
{
	std::string cellText(Cell cell)
	{
		return "(" + std::to_string(cell.x) + ", " + std::to_string(cell.z) + ")";
	}
}

void ColonySimulation::reset()
{
	scenario = ScenarioDefinition();
	grid = NavigationGrid();
	colonists.clear();
	debug_obstacles.clear();
	simulation_time = 0.0;
	initialized = false;
}

bool ColonySimulation::initialize(const ScenarioDefinition& definition, const ColonistCatalog& roles, std::string& error)
{
	reset();
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

	scenario = definition;
	grid = buildNavigationGrid(scenario);
	colonists.swap(new_colonists);
	for (ColonistRecord& colonist : colonists)
	{
		colonist.position = grid.cellCenter(colonist.last_cell);
		colonist.previous_position = colonist.position;
		colonist.has_debug_route = scenario.debug_route && scenario.debug_route->colonist_id == colonist.id;
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

void ColonySimulation::tick(float dt)
{
	if (!initialized || !(dt > 0.0f))
		return;
	for (ColonistRecord& colonist : colonists)
		updateColonist(colonist, dt);
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

bool ColonySimulation::setDoorOpen(std::size_t door_index, bool open)
{
	if (door_index >= grid.getDoorCount())
		return false;
	grid.setDoorOpen(door_index, open);
	return true;
}
