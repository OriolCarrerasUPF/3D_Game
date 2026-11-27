#include "navigation_grid.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
	// Fixed neighbour order: +X, -X, +Z, -Z (deterministic routes).
	const int neighbour_dx[4] = { 1, -1, 0, 0 };
	const int neighbour_dz[4] = { 0, 0, 1, -1 };
}

NavigationGrid::NavigationGrid(int width, int height, float cell_size, float origin_x, float origin_z)
	: width(width > 0 ? width : 0)
	, height(height > 0 ? height : 0)
	, cell_size(cell_size > 0.0f ? cell_size : 1.0f)
	, origin_x(origin_x)
	, origin_z(origin_z)
{
	const std::size_t count = static_cast<std::size_t>(this->width) * static_cast<std::size_t>(this->height);
	blocked.assign(count, 0);
	zones.assign(count, Zone::Outside);
	parent.assign(count, -1);
	visit_stamp.assign(count, 0);
}

bool NavigationGrid::inBounds(Cell cell) const
{
	return cell.x >= 0 && cell.z >= 0 && cell.x < width && cell.z < height;
}

Vector3 NavigationGrid::cellCenter(Cell cell) const
{
	return Vector3(origin_x + (static_cast<float>(cell.x) + 0.5f) * cell_size, 0.0f,
		origin_z + (static_cast<float>(cell.z) + 0.5f) * cell_size);
}

bool NavigationGrid::worldToCell(float x, float z, Cell& out_cell) const
{
	const Cell cell{ static_cast<int>(std::floor((x - origin_x) / cell_size)),
		static_cast<int>(std::floor((z - origin_z) / cell_size)) };
	if (!inBounds(cell))
		return false;
	out_cell = cell;
	return true;
}

void NavigationGrid::setBlocked(Cell cell, bool is_blocked)
{
	if (!inBounds(cell))
		return;
	std::uint8_t& value = blocked[index(cell)];
	const std::uint8_t new_value = is_blocked ? 1 : 0;
	if (value != new_value)
	{
		value = new_value;
		++revision;
	}
}

bool NavigationGrid::isBlocked(Cell cell) const
{
	return !inBounds(cell) || blocked[index(cell)] != 0;
}

void NavigationGrid::setZoneRect(Cell min, Cell max, Zone zone)
{
	for (int z = std::max(min.z, 0); z <= std::min(max.z, height - 1); ++z)
		for (int x = std::max(min.x, 0); x <= std::min(max.x, width - 1); ++x)
			zones[index(Cell{ x, z })] = zone;
	++revision;
}

void NavigationGrid::setZone(Cell cell, Zone zone)
{
	if (!inBounds(cell))
		return;
	Zone& value = zones[index(cell)];
	if (value != zone)
	{
		value = zone;
		++revision;
	}
}

Zone NavigationGrid::getZone(Cell cell) const
{
	return inBounds(cell) ? zones[index(cell)] : Zone::Outside;
}

std::uint64_t NavigationGrid::edgeKey(Cell a, Cell b) const
{
	const std::uint64_t ia = static_cast<std::uint64_t>(index(a));
	const std::uint64_t ib = static_cast<std::uint64_t>(index(b));
	return ia < ib ? (ia << 32) | ib : (ib << 32) | ia;
}

NavigationGrid::DoorIndex NavigationGrid::addDoor(const std::vector<DoorPortal>& portals, bool open)
{
	const DoorIndex door_index = doors.size();
	doors.push_back(Door{ portals, open });
	for (const DoorPortal& portal : portals)
	{
		if (inBounds(portal.outside) && inBounds(portal.inside))
			crossing_to_door[edgeKey(portal.outside, portal.inside)] = door_index;
	}
	++revision;
	return door_index;
}

void NavigationGrid::setDoorOpen(DoorIndex door, bool open)
{
	if (door >= doors.size() || doors[door].open == open)
		return;
	doors[door].open = open;
	++revision;
}

bool NavigationGrid::isDoorOpen(DoorIndex door) const
{
	return door < doors.size() && doors[door].open;
}

bool NavigationGrid::isDoorCrossing(Cell a, Cell b) const
{
	if (!inBounds(a) || !inBounds(b))
		return false;
	return crossing_to_door.count(edgeKey(a, b)) != 0;
}

bool NavigationGrid::canStep(Cell from, Cell to) const
{
	if (!isWalkable(from) || !isWalkable(to))
		return false;
	if (std::abs(from.x - to.x) + std::abs(from.z - to.z) != 1)
		return false;
	if (getZone(from) == getZone(to))
		return true;
	const auto it = crossing_to_door.find(edgeKey(from, to));
	return it != crossing_to_door.end() && doors[it->second].open;
}

template <typename IsTarget>
int NavigationGrid::search(Cell start, IsTarget is_target) const
{
	// New generation stamp instead of clearing the visited buffer every search.
	if (++current_stamp == 0)
	{
		std::fill(visit_stamp.begin(), visit_stamp.end(), 0u);
		current_stamp = 1;
	}

	frontier.clear();
	next_frontier.clear();
	const int start_index = index(start);
	visit_stamp[start_index] = current_stamp;
	parent[start_index] = -1;
	frontier.push_back(start_index);

	while (!frontier.empty())
	{
		// Whole layer = same distance; the best target is the first by priority.
		int best = -1;
		int best_priority = 0;
		for (int cell_index : frontier)
		{
			int priority = 0;
			if (is_target(cell_index, priority) && (best == -1 || priority < best_priority))
			{
				best = cell_index;
				best_priority = priority;
			}
		}
		if (best != -1)
			return best;

		next_frontier.clear();
		for (int cell_index : frontier)
		{
			const Cell cell = cellAt(cell_index);
			for (int n = 0; n < 4; ++n)
			{
				const Cell next{ cell.x + neighbour_dx[n], cell.z + neighbour_dz[n] };
				if (!inBounds(next))
					continue;
				const int next_index = index(next);
				if (visit_stamp[next_index] == current_stamp || !canStep(cell, next))
					continue;
				visit_stamp[next_index] = current_stamp;
				parent[next_index] = cell_index;
				next_frontier.push_back(next_index);
			}
		}
		frontier.swap(next_frontier);
	}
	return -1;
}

void NavigationGrid::buildPath(int goal_index, std::vector<Cell>& out_path) const
{
	out_path.clear();
	for (int cell_index = goal_index; cell_index != -1; cell_index = parent[cell_index])
		out_path.push_back(cellAt(cell_index));
	std::reverse(out_path.begin(), out_path.end());
}

bool NavigationGrid::findPath(Cell start, Cell goal, std::vector<Cell>& out_path) const
{
	out_path.clear();
	if (!isWalkable(start) || !isWalkable(goal))
		return false;
	if (start == goal)
	{
		out_path.push_back(start);
		return true;
	}
	const int goal_index = index(goal);
	const int found = search(start, [goal_index](int cell_index, int& priority) {
		priority = 0;
		return cell_index == goal_index;
	});
	if (found == -1)
		return false;
	buildPath(found, out_path);
	return true;
}

bool NavigationGrid::findNearest(Cell start, const std::vector<Cell>& targets, Cell& out_target,
	std::vector<Cell>& out_path) const
{
	out_path.clear();
	if (!isWalkable(start) || targets.empty())
		return false;

	// Target priority = first position in `targets` (stable tie-break).
	std::unordered_map<int, int> target_priority;
	for (std::size_t i = 0; i < targets.size(); ++i)
	{
		if (isWalkable(targets[i]))
			target_priority.emplace(index(targets[i]), static_cast<int>(i));
	}
	if (target_priority.empty())
		return false;

	const int found = search(start, [&target_priority](int cell_index, int& priority) {
		const auto it = target_priority.find(cell_index);
		if (it == target_priority.end())
			return false;
		priority = it->second;
		return true;
	});
	if (found == -1)
		return false;
	out_target = cellAt(found);
	buildPath(found, out_path);
	return true;
}
