// P03 automated checks for NavigationGrid (plan P03 / E2 "Navigation").

#include "test_harness.h"

#include "game/navigation_grid.h"

#include <cstdlib>

namespace
{
	// 12x12 grid, origin (-6, -6). Base interior x=4..8, z=4..8 with a door on the
	// west side: outside (3,6) -> inside (4,6).
	NavigationGrid makeBaseGrid(bool door_open = true, int door_width = 1)
	{
		NavigationGrid grid(12, 12, 1.0f, -6.0f, -6.0f);
		grid.setZoneRect(Cell{ 4, 4 }, Cell{ 8, 8 }, Zone::Inside);
		std::vector<DoorPortal> portals;
		for (int i = 0; i < door_width; ++i)
			portals.push_back(DoorPortal{ Cell{ 3, 6 + i }, Cell{ 4, 6 + i } });
		grid.addDoor(portals, door_open);
		return grid;
	}

	bool isContinuous(const NavigationGrid& grid, const std::vector<Cell>& path)
	{
		for (std::size_t i = 1; i < path.size(); ++i)
			if (!grid.canStep(path[i - 1], path[i]))
				return false;
		return true;
	}

	int zoneChanges(const NavigationGrid& grid, const std::vector<Cell>& path)
	{
		int changes = 0;
		for (std::size_t i = 1; i < path.size(); ++i)
			changes += grid.getZone(path[i - 1]) != grid.getZone(path[i]) ? 1 : 0;
		return changes;
	}
}

TEST_CASE("navigation: cell <-> world conversion round trip")
{
	NavigationGrid grid(64, 64, 1.0f, -32.0f, -32.0f);
	const Vector3 center = grid.cellCenter(Cell{ 18, 31 });
	CHECK_NEAR(center.x, -13.5, 1e-6);
	CHECK_NEAR(center.z, -0.5, 1e-6);
	CHECK_NEAR(center.y, 0.0, 1e-6);
	for (int z = 0; z < 64; z += 7)
		for (int x = 0; x < 64; x += 5)
		{
			Cell back;
			const Vector3 c = grid.cellCenter(Cell{ x, z });
			CHECK(grid.worldToCell(c.x, c.z, back));
			CHECK(back == (Cell{ x, z }));
		}
	Cell edge;
	CHECK(grid.worldToCell(-8.0f, 0.0f, edge)); // exactly on the west base edge -> cell 24
	CHECK_EQ(edge.x, 24);
	CHECK(!grid.worldToCell(-32.01f, 0.0f, edge));
	CHECK(!grid.worldToCell(0.0f, 32.0f, edge));
}

TEST_CASE("navigation: start equals goal gives a one-cell path")
{
	NavigationGrid grid = makeBaseGrid();
	std::vector<Cell> path{ Cell{ 9, 9 } }; // stale content must be cleared
	CHECK(grid.findPath(Cell{ 1, 1 }, Cell{ 1, 1 }, path));
	CHECK_EQ(path.size(), static_cast<std::size_t>(1));
	CHECK(path.front() == (Cell{ 1, 1 }));
}

TEST_CASE("navigation: shortest route in open terrain")
{
	NavigationGrid grid = makeBaseGrid();
	std::vector<Cell> path;
	CHECK(grid.findPath(Cell{ 0, 0 }, Cell{ 3, 2 }, path));
	CHECK_EQ(path.size(), static_cast<std::size_t>(6)); // Manhattan 5 + start
	CHECK(isContinuous(grid, path));
	CHECK(path.front() == (Cell{ 0, 0 }));
	CHECK(path.back() == (Cell{ 3, 2 }));
}

TEST_CASE("navigation: inside and outside connect only through the door")
{
	NavigationGrid grid = makeBaseGrid();
	// Walls are edges: a step across the boundary away from the door is refused.
	CHECK(!grid.canStep(Cell{ 3, 5 }, Cell{ 4, 5 }));
	CHECK(!grid.canStep(Cell{ 6, 3 }, Cell{ 6, 4 }));
	CHECK(grid.canStep(Cell{ 3, 6 }, Cell{ 4, 6 }));
	CHECK(grid.canStep(Cell{ 4, 6 }, Cell{ 3, 6 }));
	CHECK(!grid.canStep(Cell{ 3, 6 }, Cell{ 5, 6 })); // not a 4-neighbour step
	CHECK(!grid.canStep(Cell{ 3, 5 }, Cell{ 4, 6 })); // diagonal

	std::vector<Cell> path;
	CHECK(grid.findPath(Cell{ 0, 0 }, Cell{ 7, 7 }, path));
	CHECK(isContinuous(grid, path));
	CHECK_EQ(zoneChanges(grid, path), 1);
	bool used_door = false;
	for (std::size_t i = 1; i < path.size(); ++i)
		if (path[i - 1] == (Cell{ 3, 6 }) && path[i] == (Cell{ 4, 6 }))
			used_door = true;
	CHECK(used_door);
	// Wall cells are not lost: border cells of both zones stay walkable.
	CHECK(grid.isWalkable(Cell{ 4, 4 }));
	CHECK(grid.isWalkable(Cell{ 3, 4 }));
}

TEST_CASE("navigation: a closed door seals the boundary")
{
	NavigationGrid grid = makeBaseGrid(false);
	std::vector<Cell> path{ Cell{ 0, 0 } };
	CHECK(!grid.findPath(Cell{ 0, 0 }, Cell{ 6, 6 }, path));
	CHECK(path.empty()); // failed searches return empty routes
	CHECK(!grid.canStep(Cell{ 3, 6 }, Cell{ 4, 6 }));
	CHECK(grid.isDoorCrossing(Cell{ 3, 6 }, Cell{ 4, 6 }));

	const std::uint64_t before = grid.getRevision();
	grid.setDoorOpen(0, true);
	CHECK(grid.getRevision() > before);
	CHECK(grid.findPath(Cell{ 0, 0 }, Cell{ 6, 6 }, path));
}

TEST_CASE("navigation: wider doors are lists of crossings")
{
	NavigationGrid grid = makeBaseGrid(true, 2);
	CHECK(grid.canStep(Cell{ 3, 6 }, Cell{ 4, 6 }));
	CHECK(grid.canStep(Cell{ 3, 7 }, Cell{ 4, 7 }));
	CHECK(!grid.canStep(Cell{ 3, 8 }, Cell{ 4, 8 }));
	std::vector<Cell> path;
	CHECK(grid.findPath(Cell{ 0, 11 }, Cell{ 5, 7 }, path));
	CHECK_EQ(zoneChanges(grid, path), 1);
	CHECK(isContinuous(grid, path));
}

TEST_CASE("navigation: obstacles force a detour and unreachable goals fail")
{
	NavigationGrid grid = makeBaseGrid();
	std::vector<Cell> path;
	CHECK(grid.findPath(Cell{ 0, 1 }, Cell{ 2, 1 }, path));
	CHECK_EQ(path.size(), static_cast<std::size_t>(3));

	const std::uint64_t before = grid.getRevision();
	grid.setBlocked(Cell{ 1, 1 }, true);
	CHECK(grid.getRevision() > before);
	CHECK(grid.findPath(Cell{ 0, 1 }, Cell{ 2, 1 }, path));
	CHECK_EQ(path.size(), static_cast<std::size_t>(5)); // around the obstacle
	CHECK(isContinuous(grid, path));
	for (const Cell& cell : path)
		CHECK(!(cell == (Cell{ 1, 1 })));

	// Blocking the door cell inside makes the base unreachable from outside.
	grid.setBlocked(Cell{ 4, 6 }, true);
	CHECK(!grid.findPath(Cell{ 0, 0 }, Cell{ 6, 6 }, path));
	CHECK(path.empty());
	// Blocked or out-of-grid endpoints fail.
	CHECK(!grid.findPath(Cell{ 1, 1 }, Cell{ 0, 0 }, path));
	CHECK(!grid.findPath(Cell{ 0, 0 }, Cell{ 12, 0 }, path));
	// Unchanged state does not bump the revision.
	const std::uint64_t stable = grid.getRevision();
	grid.setBlocked(Cell{ 1, 1 }, true);
	CHECK_EQ(grid.getRevision(), stable);
}

TEST_CASE("navigation: repeated searches are independent and deterministic")
{
	NavigationGrid grid = makeBaseGrid();
	std::vector<Cell> first, again, other;
	CHECK(grid.findPath(Cell{ 0, 0 }, Cell{ 7, 7 }, first));
	for (int i = 0; i < 50; ++i)
	{
		CHECK(grid.findPath(Cell{ 11, 11 }, Cell{ 0, 11 }, other));
		CHECK(grid.findPath(Cell{ 0, 0 }, Cell{ 7, 7 }, again));
		CHECK(again.size() == first.size());
		bool same = again.size() == first.size();
		for (std::size_t c = 0; same && c < again.size(); ++c)
			same = again[c] == first[c];
		CHECK(same);
	}
}

TEST_CASE("navigation: findNearest picks the closest reachable target")
{
	NavigationGrid grid = makeBaseGrid();
	Cell target;
	std::vector<Cell> path;
	// (6,6) is inside: 8 steps through the door from (0,6); (0,10) is 4 steps away.
	CHECK(grid.findNearest(Cell{ 0, 6 }, { Cell{ 6, 6 }, Cell{ 0, 10 } }, target, path));
	CHECK(target == (Cell{ 0, 10 }));
	CHECK_EQ(path.size(), static_cast<std::size_t>(5));
	// Equal distance: the first listed target wins.
	CHECK(grid.findNearest(Cell{ 5, 0 }, { Cell{ 7, 0 }, Cell{ 3, 0 } }, target, path));
	CHECK(target == (Cell{ 7, 0 }));
	CHECK(grid.findNearest(Cell{ 5, 0 }, { Cell{ 3, 0 }, Cell{ 7, 0 } }, target, path));
	CHECK(target == (Cell{ 3, 0 }));
	// Nothing reachable.
	grid.setDoorOpen(0, false);
	CHECK(!grid.findNearest(Cell{ 0, 0 }, { Cell{ 6, 6 } }, target, path));
	CHECK(path.empty());
}
