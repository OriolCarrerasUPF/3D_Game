// P05B base layout (headless): the corridor view rules (plan C5.2, same results as
// tools/base_layout_rules.py), inside cells built by corridors and Exit Doors, dome
// entrances, and colonists present inside a dome (dev/base_layout.json).

#include "test_harness.h"

#include "game/base_layout.h"
#include "game/colony_simulation.h"

#include <algorithm>
#include <cmath>

namespace
{
	using json_data::Json;
	using base_layout::CellSet;
	using base_layout::Piece;
	using base_layout::PieceKind;

	CellSet rect(int x0, int x1, int z0, int z1)
	{
		CellSet cells;
		for (int x = x0; x <= x1; ++x)
			for (int z = z0; z <= z1; ++z)
				cells.insert({ x, z });
		return cells;
	}

	CellSet unite(const CellSet& a, const CellSet& b)
	{
		CellSet out = a;
		out.insert(b.begin(), b.end());
		return out;
	}

	std::size_t countKind(const std::vector<Piece>& pieces, PieceKind kind)
	{
		return static_cast<std::size_t>(std::count_if(pieces.begin(), pieces.end(), [kind](const Piece& p) { return p.kind == kind; }));
	}

	GameDefinitions shippedDefinitions()
	{
		GameDefinitions definitions;
		const ColonistCatalogLoadResult colonists = definitions.colonists.load("data/arkfall/colonists.json");
		if (!colonists)
			test::fail(__FILE__, __LINE__, colonists.error->describe());
		const BuildingCatalogLoadResult buildings = definitions.buildings.load("data/arkfall/buildings.json");
		if (!buildings)
			test::fail(__FILE__, __LINE__, buildings.error->describe());
		return definitions;
	}

	bool startDev(ColonySimulation& simulation, const char* path)
	{
		const ScenarioLoadResult loaded = ScenarioLoader().load(path);
		if (!loaded)
		{
			test::fail(__FILE__, __LINE__, loaded.error->describe());
			return false;
		}
		std::string error;
		if (!simulation.initialize(loaded.scenario, shippedDefinitions(), error))
		{
			test::fail(__FILE__, __LINE__, error);
			return false;
		}
		return true;
	}
}

TEST_CASE("base layout: L corridor matches the reference rules (walls, arches, junction)")
{
	// The Blender preview layout: a 2-cell corridor north from an airlock chamber, turning east.
	const CellSet corridor = unite(rect(0, 1, 3, 7), rect(2, 6, 6, 7));
	const CellSet inside = unite(corridor, rect(0, 1, 1, 2));
	const std::vector<Piece> walls = base_layout::wallPieces(corridor, inside, {});
	const std::vector<Piece> arches = base_layout::archPieces(corridor, inside);
	const std::vector<Piece> caps = base_layout::junctionPieces(corridor, inside);
	// Same numbers as tools/base_layout_rules.py for this layout.
	CHECK_EQ(walls.size(), static_cast<std::size_t>(22));
	CHECK_EQ(arches.size(), static_cast<std::size_t>(8));
	CHECK_EQ(caps.size(), static_cast<std::size_t>(1));
	CHECK_NEAR(walls[0].x, 0.5, 1e-6);
	CHECK_NEAR(walls[0].z, 3.5, 1e-6);
	CHECK_NEAR(walls[0].yaw_degrees, 0.0, 1e-6);   // west wall of (0,3)
	CHECK_NEAR(arches[0].x, 1.0, 1e-6);
	CHECK_NEAR(arches[0].z, 3.5, 1e-6);
	CHECK_NEAR(arches[3].x, 2.5, 1e-6);
	CHECK_NEAR(arches[3].z, 7.0, 1e-6);
	CHECK_NEAR(arches[3].yaw_degrees, 90.0, 1e-6);
	CHECK_NEAR(caps[0].x, 1.0, 1e-6);
	CHECK_NEAR(caps[0].z, 7.0, 1e-6);
	// No wall towards the airlock chamber (inside), one at the corridor's dead end.
	for (const Piece& wall : walls)
		CHECK(!(std::fabs(wall.z - 3.5f) < 1e-4f && std::fabs(wall.yaw_degrees - 90.0f) < 1e-4f));
	bool dead_end = false;
	for (const Piece& wall : walls)
		dead_end = dead_end || (std::fabs(wall.x - 6.5f) < 1e-4f && std::fabs(wall.yaw_degrees - 180.0f) < 1e-4f);
	CHECK(dead_end);
	CHECK_EQ(base_layout::corridorPieces(corridor, inside, {}).size(), static_cast<std::size_t>(31));
}

TEST_CASE("base layout: T and cross junctions get one cap, no hanging arches")
{
	const CellSet t = unite(rect(0, 9, 4, 5), rect(4, 5, 0, 3));
	CHECK_EQ(base_layout::wallPieces(t, t, {}).size(), static_cast<std::size_t>(32));
	CHECK_EQ(base_layout::archPieces(t, t).size(), static_cast<std::size_t>(12));
	const std::vector<Piece> t_caps = base_layout::junctionPieces(t, t);
	CHECK_EQ(t_caps.size(), static_cast<std::size_t>(1));
	CHECK_NEAR(t_caps[0].x, 5.0, 1e-6);
	CHECK_NEAR(t_caps[0].z, 5.0, 1e-6);

	const CellSet cross = unite(rect(0, 9, 4, 5), rect(4, 5, 0, 9));
	const std::vector<Piece> cross_caps = base_layout::junctionPieces(cross, cross);
	CHECK_EQ(cross_caps.size(), static_cast<std::size_t>(1));
	CHECK_NEAR(cross_caps[0].x, 5.0, 1e-6);
	// Every arch spans exactly two corridor cells with non-inside cells on both sides.
	for (const Piece& arch : base_layout::archPieces(cross, cross))
	{
		const bool along_x = arch.yaw_degrees == 0.0f;
		const int a = static_cast<int>(std::lround(along_x ? arch.x : arch.z));
		const int fixed = static_cast<int>(std::floor(along_x ? arch.z : arch.x));
		auto has = [&](int along) { return along_x ? cross.count({ along, fixed }) : cross.count({ fixed, along }); };
		CHECK(has(a - 1) && has(a) && !has(a - 2) && !has(a + 1));
	}
	// A 3-wide area and a single cell get neither arches nor caps.
	const CellSet wide = rect(0, 2, 0, 5);
	CHECK(base_layout::archPieces(wide, wide).empty());
	const CellSet single{ { 3, 3 } };
	CHECK(base_layout::archPieces(single, single).empty());
	CHECK(base_layout::junctionPieces(single, single).empty());
	CHECK_EQ(base_layout::wallPieces(single, single, {}).size(), static_cast<std::size_t>(4));
}

TEST_CASE("base layout: open edges and passage-axis offsets")
{
	const CellSet corridor = rect(0, 1, 0, 3);
	// A dome entrance on (1,2) facing east leaves that edge open.
	const base_layout::EdgeSet open{ { { 1, 2 }, { 2, 2 } } };
	const std::vector<Piece> closed = base_layout::wallPieces(corridor, corridor, {});
	const std::vector<Piece> with_entrance = base_layout::wallPieces(corridor, corridor, open);
	CHECK_EQ(closed.size(), with_entrance.size() + 1);

	base_layout::OffsetMap offsets;
	base_layout::addCorridorOffsets(base_layout::corridorPieces(corridor, corridor, {}), offsets);
	// Arches along X: the west cell moves +0.5 in x, the east cell -0.5.
	CHECK_NEAR(base_layout::offsetAt(offsets, Cell{ 0, 1 }).first, 0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(offsets, Cell{ 1, 1 }).first, -0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(offsets, Cell{ 1, 1 }).second, 0.0, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(offsets, Cell{ 7, 7 }).first, 0.0, 1e-6);   // no offset elsewhere

	// L junction: towards the block centre on both axes.
	const CellSet l = unite(rect(0, 1, 0, 5), rect(2, 6, 4, 5));
	base_layout::OffsetMap l_offsets;
	base_layout::addCorridorOffsets(base_layout::corridorPieces(l, l, {}), l_offsets);
	CHECK_NEAR(base_layout::offsetAt(l_offsets, Cell{ 0, 4 }).first, 0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(l_offsets, Cell{ 0, 4 }).second, 0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(l_offsets, Cell{ 1, 5 }).first, -0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(l_offsets, Cell{ 1, 5 }).second, -0.5, 1e-6);

	// Airlock: across the door (local X): world X at 0 deg, world Z at 90/270 deg.
	base_layout::OffsetMap airlock;
	base_layout::addAirlockOffsets({ Cell{ 4, 0 }, Cell{ 5, 0 }, Cell{ 4, 1 }, Cell{ 5, 1 }, Cell{ 4, 2 }, Cell{ 5, 2 } }, 0, airlock);
	CHECK_NEAR(base_layout::offsetAt(airlock, Cell{ 4, 2 }).first, 0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(airlock, Cell{ 5, 0 }).first, -0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(airlock, Cell{ 5, 0 }).second, 0.0, 1e-6);
	base_layout::OffsetMap turned;
	base_layout::addAirlockOffsets({ Cell{ 0, 6 }, Cell{ 1, 6 }, Cell{ 2, 6 }, Cell{ 0, 7 }, Cell{ 1, 7 }, Cell{ 2, 7 } }, 3, turned);
	CHECK_NEAR(base_layout::offsetAt(turned, Cell{ 1, 6 }).second, 0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(turned, Cell{ 1, 7 }).second, -0.5, 1e-6);
	CHECK_NEAR(base_layout::offsetAt(turned, Cell{ 1, 7 }).first, 0.0, 1e-6);
}

TEST_CASE("base layout: inside cells, door and dome entrances come from the buildings")
{
	ColonySimulation simulation;
	if (!startDev(simulation, "data/arkfall/dev/base_layout.json"))
		return;
	const NavigationGrid& grid = simulation.getGrid();
	// L corridor: (25..30, 30..31) then north (29..30, 32..37); airlock chamber (23..24, 30..31).
	CHECK(grid.getZone(Cell{ 25, 30 }) == Zone::Inside);
	CHECK(grid.getZone(Cell{ 30, 37 }) == Zone::Inside);
	CHECK(grid.getZone(Cell{ 23, 31 }) == Zone::Inside);
	CHECK(grid.getZone(Cell{ 22, 31 }) == Zone::Outside);
	CHECK(grid.getZone(Cell{ 31, 37 }) == Zone::Outside);
	CHECK_EQ(grid.getDoorCount(), static_cast<std::size_t>(1));
	// Outside and inside only connect through the two crossings.
	int crossings = 0;
	for (int z = 0; z < grid.getHeight(); ++z)
		for (int x = 0; x < grid.getWidth(); ++x)
			for (const Cell n : { Cell{ x + 1, z }, Cell{ x, z + 1 } })
				if (grid.inBounds(n) && grid.getZone(Cell{ x, z }) != grid.getZone(n) && grid.canStep(Cell{ x, z }, n))
					++crossings;
	CHECK_EQ(crossings, 2);

	// Domes: the Storage's entrances are its candidates on the corridor; the processor's
	// is (30,34) on the north branch.
	const BuildingRecord* storage = nullptr;
	const BuildingRecord* processor = nullptr;
	for (const BuildingRecord& building : simulation.getBuildings())
	{
		if (building.type == BuildingType::Storage) storage = &building;
		if (building.type == BuildingType::IronProcessor) processor = &building;
	}
	CHECK(storage && processor);
	if (!storage || !processor)
		return;
	CHECK_EQ(simulation.usableAccessCells(*storage).size(), static_cast<std::size_t>(2));
	const std::vector<Cell> entrances = simulation.usableAccessCells(*processor);
	CHECK_EQ(entrances.size(), static_cast<std::size_t>(1));
	CHECK(!entrances.empty() && entrances[0] == (Cell{ 30, 34 }));
}

TEST_CASE("base layout: a Miner works hidden inside the processor dome (acceptance)")
{
	ColonySimulation simulation;
	if (!startDev(simulation, "data/arkfall/dev/base_layout.json"))
		return;
	const BuildingRecord* processor = nullptr;
	for (const BuildingRecord& building : simulation.getBuildings())
		if (building.type == BuildingType::IronProcessor)
			processor = &building;
	if (!processor)
		return;
	const NavigationGrid& grid = simulation.getGrid();
	std::vector<Cell> previous;
	for (const ColonistRecord& c : simulation.getColonists())
		previous.push_back(c.last_cell);

	ColonistId worker = INVALID_COLONIST;
	bool crossed_door = false;
	for (int tick = 0; tick < 30 * 120 && worker == INVALID_COLONIST; ++tick)
	{
		simulation.tick(1.0f / 30.0f);
		for (std::size_t i = 0; i < simulation.getColonists().size(); ++i)
		{
			const ColonistRecord& c = simulation.getColonists()[i];
			if (!(c.last_cell == previous[i]))
			{
				// Every reached waypoint is a legal step: nobody walks through a wall.
				CHECK(grid.canStep(previous[i], c.last_cell));
				if (grid.getZone(previous[i]) != grid.getZone(c.last_cell))
				{
					crossed_door = true;
					CHECK(simulation.isDoorCell(previous[i]) && simulation.isDoorCell(c.last_cell));
				}
				previous[i] = c.last_cell;
			}
			if (c.current_dome == processor->id)
				worker = c.id;
		}
	}
	CHECK(crossed_door);
	CHECK(worker != INVALID_COLONIST);
	if (worker == INVALID_COLONIST)
		return;
	const ColonistRecord* miner = simulation.findColonist(worker);
	CHECK(miner->state == ColonistState::Working);
	CHECK(miner->last_cell == (Cell{ 30, 34 }));
	const std::vector<ColonistId> inside = simulation.occupants(processor->id);
	CHECK_EQ(inside.size(), static_cast<std::size_t>(1));
	CHECK(!inside.empty() && inside[0] == worker);

	// Changing role releases the slot: the colonist leaves the dome (visible again).
	std::string reason;
	CHECK(simulation.assignJob(worker, Job::Engineer, reason));
	CHECK_EQ(miner->current_dome, INVALID_BUILDING);
	CHECK(simulation.occupants(processor->id).empty());
	// P06 idle rule: the new Engineer has no job, so it waits at the nearest dome
	// entrance, which is where it stands: inside the processor again, but idle.
	simulation.tick(1.0f / 30.0f);
	CHECK(miner->state == ColonistState::Idle);
	CHECK(!miner->reservation.active());
}

TEST_CASE("base layout: same inputs give the same base (deterministic replay)")
{
	ColonySimulation a, b;
	if (!startDev(a, "data/arkfall/dev/base_layout.json") || !startDev(b, "data/arkfall/dev/base_layout.json"))
		return;
	for (int tick = 0; tick < 30 * 60; ++tick)
	{
		a.tick(1.0f / 30.0f);
		b.tick(1.0f / 30.0f);
	}
	for (std::size_t i = 0; i < a.getColonists().size(); ++i)
	{
		CHECK(a.getColonists()[i].last_cell == b.getColonists()[i].last_cell);
		CHECK_EQ(a.getColonists()[i].current_dome, b.getColonists()[i].current_dome);
	}
	CHECK(a.getInventory().getAmounts() == b.getInventory().getAmounts());
}

namespace
{
	// data/arkfall/dev/dome_chain.json: door -> corridor -> Storage -> corridor -> Iron
	// Processor; the processor is reachable only by walking through the Storage dome
	// (walkable domes, P06). One Miner, so the processor is its nearest job.
	json_data::Json chainScenario()
	{
		json_data::Json doc;
		DataLoadError error;
		if (!json_data::loadJson("data/arkfall/dev/dome_chain.json", doc, error))
			test::fail(__FILE__, __LINE__, error.describe());
		doc["initialColonists"] = json_data::Json::parse(R"([
			{"id": 1, "job": "miner", "cell": [17, 30], "needs": {"hunger": 100, "thirst": 100, "oxygen": 100}}])");
		return doc;
	}
}

TEST_CASE("base layout: a colonist reaches a dome only reachable through another dome")
{
	const ScenarioLoadResult loaded = ScenarioLoader().parse(chainScenario(), "chain.json");
	if (!loaded)
	{
		test::fail(__FILE__, __LINE__, loaded.error->describe());
		return;
	}
	ColonySimulation simulation;
	std::string error;
	if (!simulation.initialize(loaded.scenario, shippedDefinitions(), error))
	{
		test::fail(__FILE__, __LINE__, error);
		return;
	}
	const NavigationGrid& grid = simulation.getGrid();
	const BuildingRecord* storage = nullptr;
	const BuildingRecord* processor = nullptr;
	for (const BuildingRecord& b : simulation.getBuildings())
	{
		if (b.type == BuildingType::Storage)
			storage = &b;
		if (b.type == BuildingType::IronProcessor)
			processor = &b;
	}
	CHECK(storage && processor);
	if (!storage || !processor)
		return;
	// The Storage is entered only through its entrances: (28, 30) on the first corridor and
	// (31, 30) on the second; its side walls stay closed, also towards the outside.
	CHECK(grid.canStep(Cell{ 28, 30 }, Cell{ 29, 30 }));
	CHECK(grid.canStep(Cell{ 30, 30 }, Cell{ 31, 30 }));
	CHECK(!grid.canStep(Cell{ 28, 31 }, Cell{ 29, 31 }));   // corridor beside the dome wall
	CHECK(!grid.canStep(Cell{ 29, 32 }, Cell{ 29, 31 }));   // outside -> dome
	std::vector<Cell> path;
	CHECK(grid.findPath(Cell{ 25, 30 }, Cell{ 34, 30 }, path));
	bool through_storage = false;
	for (Cell c : path)
		through_storage = through_storage || grid.getRoom(c) == storage->id;
	CHECK(through_storage);

	// The Miner walks door -> corridor -> Storage -> corridor and works at the processor;
	// it is hidden while crossing the Storage.
	const ColonistRecord* miner = simulation.findColonist(1);
	bool hidden_in_storage = false;
	for (int i = 0; i < 30 * 60 && miner->state != ColonistState::Working; ++i)
	{
		simulation.tick(1.0f / 30.0f);
		hidden_in_storage = hidden_in_storage || miner->current_dome == storage->id;
	}
	CHECK(hidden_in_storage);
	CHECK(miner->state == ColonistState::Working);
	CHECK_EQ(miner->reservation.building, processor->id);
	CHECK_EQ(miner->current_dome, processor->id);
	simulation.tick(1.0f / 30.0f);
	CHECK(simulation.occupants(storage->id).empty());
}

TEST_CASE("base layout: walkable dome placement rules")
{
	const ScenarioLoadResult loaded = ScenarioLoader().parse(chainScenario(), "chain.json");
	ColonySimulation simulation;
	std::string error;
	if (!loaded || !simulation.initialize(loaded.scenario, shippedDefinitions(), error))
	{
		test::fail(__FILE__, __LINE__, error);
		return;
	}
	auto code = [&](BuildingType type, int x, int z, int q = 0) { return simulation.validatePlacement(type, Cell{ x, z }, q).code; };
	// A corridor joins on a dome entrance candidate ((30, 29) is one of the Storage's)...
	CHECK_EQ(code(BuildingType::Corridor, 30, 29), std::string("ok"));
	// ...but not beside a dome wall that has no entrance there.
	CHECK_EQ(code(BuildingType::Corridor, 29, 32), std::string("needs_network"));
	// A dome cannot open into another dome.
	CHECK_EQ(code(BuildingType::Storage, 29, 32), std::string("access_blocked"));
	// Exterior buildings stay off the domes.
	CHECK_EQ(code(BuildingType::SolarPanel, 30, 26), std::string("ok"));
	CHECK(simulation.isNetworkCell(Cell{ 29, 30 }));
	CHECK(!simulation.isPassageCell(Cell{ 29, 30 }));
}
