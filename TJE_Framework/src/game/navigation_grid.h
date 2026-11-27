#pragma once

// Planar 4-neighbour navigation grid (plan C5), headless.
//
// * Cells are 1 m squares indexed (x, z) in [0, width) x [0, height). The one and
//   only cell <-> world conversion lives here (cellCenter / worldToCell).
// * Each cell has a `blocked` flag (deposits, static blockers, buildings later) and
//   a Zone (Inside / Outside the base). Walls are NOT cells: the base boundary is
//   the set of edges between zones, and a step across it is only allowed through
//   a crossing (DoorPortal) of an open Exit Door. A door is a list of crossings, so
//   1-cell and wider doors work the same way.
// * findPath/findNearest run a breadth-first search (all steps cost 1, so BFS gives
//   shortest routes). Neighbour order is fixed (+X, -X, +Z, -Z) so results are
//   deterministic. The scratch buffers are reused between searches.
// * Every change that can invalidate a route increments getRevision().
//
// Theory reuse (T13 pp. 18-21, 31): grid pathfinding on a flat world. The framework's
// extra/pathfinder A* is not used (plan A4: sorted-open/heap bug and stale state).

#include "arkfall_types.h"
#include "framework/framework.h"   // Vector3 (header-only math)

#include <cstdint>
#include <unordered_map>
#include <vector>

class NavigationGrid
{
public:
	using DoorIndex = std::size_t;

	NavigationGrid() = default;
	// Creates an all-outside, all-free grid. Width/height must be positive.
	NavigationGrid(int width, int height, float cell_size, float origin_x, float origin_z);

	int getWidth() const { return width; }
	int getHeight() const { return height; }
	float getCellSize() const { return cell_size; }
	float getOriginX() const { return origin_x; }
	float getOriginZ() const { return origin_z; }
	std::uint64_t getRevision() const { return revision; }

	// ---- Coordinates ------------------------------------------------------
	bool inBounds(Cell cell) const;
	// World-space centre of a cell at gameplay height y = 0.
	Vector3 cellCenter(Cell cell) const;
	// Cell containing world point (x, z); false when outside the grid.
	bool worldToCell(float x, float z, Cell& out_cell) const;

	// ---- Occupancy and zones ---------------------------------------------
	void setBlocked(Cell cell, bool blocked);
	bool isBlocked(Cell cell) const;          // out-of-bounds counts as blocked
	bool isWalkable(Cell cell) const { return inBounds(cell) && !isBlocked(cell); }
	// Marks the inclusive rectangle [min, max] with `zone`.
	void setZoneRect(Cell min, Cell max, Zone zone);
	// Marks one cell (P05B: completed Corridors and Exit Door chambers become inside).
	// Bumps the revision only when the zone changes.
	void setZone(Cell cell, Zone zone);
	Zone getZone(Cell cell) const;

	// ---- Exit Doors --------------------------------------------------------
	// Registers a door made of one or more crossings. Each crossing must join two
	// 4-neighbour cells in different zones (the caller/loader validates this).
	DoorIndex addDoor(const std::vector<DoorPortal>& portals, bool open);
	void setDoorOpen(DoorIndex door, bool open);
	bool isDoorOpen(DoorIndex door) const;
	std::size_t getDoorCount() const { return doors.size(); }
	const std::vector<DoorPortal>& getDoorPortals(DoorIndex door) const { return doors.at(door).portals; }
	// True when the edge a-b is a crossing of any door (open or not).
	bool isDoorCrossing(Cell a, Cell b) const;

	// ---- Movement rules -----------------------------------------------------
	// One 4-neighbour step: both cells walkable; if the zones differ the edge must
	// be a crossing of an open door. Diagonal or longer steps are never allowed.
	bool canStep(Cell from, Cell to) const;

	// Shortest 4-neighbour route including start and goal. Clears out_path first.
	// start == goal gives a one-cell path. Returns false (empty path) if either end
	// is not walkable or the goal is unreachable.
	bool findPath(Cell start, Cell goal, std::vector<Cell>& out_path) const;

	// Shortest route to the closest reachable target; ties go to the target that
	// appears first in `targets`. Clears outputs first; false if none is reachable.
	bool findNearest(Cell start, const std::vector<Cell>& targets, Cell& out_target,
		std::vector<Cell>& out_path) const;

private:
	struct Door
	{
		std::vector<DoorPortal> portals;
		bool open = false;
	};

	int index(Cell cell) const { return cell.z * width + cell.x; }
	Cell cellAt(int cell_index) const { return Cell{ cell_index % width, cell_index / width }; }
	std::uint64_t edgeKey(Cell a, Cell b) const;
	// Breadth-first search from start, one distance layer at a time. Returns the
	// target with the lowest priority in the first layer that contains any target
	// (is_target(cell_index, out_priority)), or -1. Fills `parent` for reached cells.
	template <typename IsTarget>
	int search(Cell start, IsTarget is_target) const;
	void buildPath(int goal_index, std::vector<Cell>& out_path) const;

	int width = 0;
	int height = 0;
	float cell_size = 1.0f;
	float origin_x = 0.0f;
	float origin_z = 0.0f;
	std::uint64_t revision = 0;

	std::vector<std::uint8_t> blocked;
	std::vector<Zone> zones;
	std::vector<Door> doors;
	std::unordered_map<std::uint64_t, DoorIndex> crossing_to_door;

	// BFS scratch, reused between searches (mutable: searches are logically const).
	mutable std::vector<int> parent;
	mutable std::vector<std::uint32_t> visit_stamp;
	mutable std::uint32_t current_stamp = 0;
	mutable std::vector<int> frontier;
	mutable std::vector<int> next_frontier;
};
