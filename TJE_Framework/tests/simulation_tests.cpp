// P03 movement and scripted-route checks for ColonySimulation (headless), on the P05B
// base made of an Exit Door and a corridor.

#include "test_harness.h"

#include "game/colony_simulation.h"

#include <cmath>
#include <fstream>

namespace
{
	using json_data::Json;

	// P05B base: an Exit Door (2 x 3 airlock turned west, apron x=4, chamber x=5..6,
	// crossings (4,6)->(5,6) and (4,7)->(5,7)) and a corridor strip x=7..10, z=6..7.
	ScenarioDefinition makeScenario(bool with_route = true, bool loop = true)
	{
		Json doc = Json::parse(R"({
			"schemaVersion": 1,
			"grid": { "width": 16, "height": 16, "cellSize": 1.0, "originXZ": [-8, -8] },
			"deposits": [ { "id": "iron", "resource": "iron", "cell": [1, 1] } ],
			"staticBlockers": [],
			"initialColonists": [ { "id": 1, "job": "miner", "cell": [2, 7],
				"needs": { "hunger": 100, "thirst": 100, "oxygen": 100 } } ],
			"initialInventory": { "steel": 10 },
			"cargoCapacity": 100,
			"initialBuildings": [ { "type": "exit_door", "origin": [4, 6], "quarterTurns": 3 },
				{ "type": "corridor", "from": [7, 6], "to": [10, 7] } ],
			"debugRoute": { "colonistId": 1, "loop": true, "dwellSeconds": 0.5,
				"stops": [ { "label": "store", "cell": [9, 7] }, { "label": "mine", "cell": [2, 1] } ] }
		})");
		doc["debugRoute"]["loop"] = loop;
		if (!with_route)
			doc.erase("debugRoute");
		ScenarioLoadResult result = ScenarioLoader().parse(doc, "test_scenario.json");
		if (!result)
			test::fail(__FILE__, __LINE__, result.error->describe());
		return result.scenario;
	}

	GameDefinitions makeDefinitions(float speed)
	{
		const std::filesystem::path path = std::filesystem::temp_directory_path() / "arkfall_test_colonists.json";
		{
			std::ofstream out(path);
			out << R"({ "schemaVersion": 1, "roles": [
				{ "job": "engineer", "assetId": "astronaut", "materialId": "crew", "moveSpeed": )" << speed << R"( },
				{ "job": "miner", "assetId": "astronaut", "materialId": "crew", "moveSpeed": )" << speed << R"( },
				{ "job": "farmer", "assetId": "astronaut", "materialId": "crew", "moveSpeed": )" << speed << R"( } ],
				"needs": { "refillThreshold": 20, "drainPerSecond": { "hunger": 0.10, "thirst": 0.12, "oxygen": 0.10 },
				"drinkPerSecond": 20, "waterPerThirstPoint": 0.01, "eatSeconds": 3, "vegetablesPerMeal": 1,
				"suitRefillPerSecond": 20, "serviceRetrySeconds": 2 },
				"names": [ "Ada", "Bram", "Cleo" ] })";
		}
		GameDefinitions definitions;
		const ColonistCatalogLoadResult result = definitions.colonists.load(path);
		if (!result)
			test::fail(__FILE__, __LINE__, result.error->describe());
		// The doors and corridors of the test base come from the shipped buildings.json.
		const BuildingCatalogLoadResult buildings = definitions.buildings.load("data/arkfall/buildings.json");
		if (!buildings)
			test::fail(__FILE__, __LINE__, buildings.error->describe());
		std::error_code ec;
		std::filesystem::remove(path, ec);
		return definitions;
	}

	ColonySimulation makeSimulation(float speed = 2.0f, bool loop = true)
	{
		ColonySimulation simulation;
		std::string error;
		if (!simulation.initialize(makeScenario(true, loop), makeDefinitions(speed), error))
			test::fail(__FILE__, __LINE__, error);
		return simulation;
	}

	float distance2D(const Vector3& a, const Vector3& b)
	{
		return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
	}
}

TEST_CASE("simulation: colonists spawn on their scenario cell")
{
	ColonySimulation simulation = makeSimulation();
	CHECK(simulation.isInitialized());
	const ColonistRecord* colonist = simulation.findColonist(1);
	CHECK(colonist != nullptr);
	if (!colonist)
		return;
	CHECK(colonist->last_cell == (Cell{ 2, 7 }));
	CHECK_NEAR(colonist->position.x, -5.5, 1e-6);
	CHECK_NEAR(colonist->position.z, -0.5, 1e-6);
	CHECK(colonist->has_debug_route);
	CHECK(colonist->state == ColonistState::Idle);
}

TEST_CASE("simulation: movement follows the distance budget and never overshoots")
{
	const float dt = 1.0f / 30.0f;
	for (float speed : { 0.5f, 1.8f, 7.0f, 19.0f })
	{
		ColonySimulation simulation = makeSimulation(speed);
		Vector3 previous = simulation.findColonist(1)->position;
		for (int tick = 0; tick < 120; ++tick)
		{
			simulation.tick(dt);
			const ColonistRecord& c = *simulation.findColonist(1);
			// Per-tick displacement never exceeds speed * dt (path length >= straight distance).
			CHECK(distance2D(previous, c.position) <= speed * dt + 1e-4f);
			// The colonist is always on the segment between two cell centres of the grid.
			const float fx = c.position.x + 8.0f - 0.5f;
			const float fz = c.position.z + 8.0f - 0.5f;
			const bool on_row = std::fabs(fz - std::round(fz)) < 1e-4f;
			const bool on_column = std::fabs(fx - std::round(fx)) < 1e-4f;
			CHECK(on_row || on_column);
			previous = c.position;
		}
	}
}

TEST_CASE("simulation: the scripted route crosses the door and reaches each stop")
{
	ColonySimulation simulation = makeSimulation(2.0f, false);
	const NavigationGrid& grid = simulation.getGrid();
	bool crossed_inward = false;
	bool reached_store = false;
	bool reached_mine = false;
	Cell previous_cell = simulation.findColonist(1)->last_cell;
	for (int tick = 0; tick < 30 * 60; ++tick)
	{
		simulation.tick(1.0f / 30.0f);
		const ColonistRecord& c = *simulation.findColonist(1);
		if (!(c.last_cell == previous_cell))
		{
			// Every reached waypoint is a legal step from the previous one.
			CHECK(grid.canStep(previous_cell, c.last_cell));
			if (previous_cell == (Cell{ 4, 7 }) && c.last_cell == (Cell{ 5, 7 }))
				crossed_inward = true;
			previous_cell = c.last_cell;
		}
		reached_store = reached_store || c.last_cell == (Cell{ 9, 7 });
		reached_mine = reached_mine || (reached_store && c.last_cell == (Cell{ 2, 1 }));
	}
	CHECK(crossed_inward);
	CHECK(reached_store);
	CHECK(reached_mine);
	const ColonistRecord& done = *simulation.findColonist(1);
	CHECK(done.debug_route_done);
	CHECK(done.state == ColonistState::Idle);
	CHECK(done.last_cell == (Cell{ 2, 1 }));
	CHECK_NEAR(distance2D(done.position, grid.cellCenter(Cell{ 2, 1 })), 0.0, 1e-5);
}

TEST_CASE("simulation: yaw faces the walking direction")
{
	ColonySimulation simulation = makeSimulation(2.0f);
	for (int tick = 0; tick < 10; ++tick)
		simulation.tick(1.0f / 30.0f);
	const ColonistRecord& c = *simulation.findColonist(1);
	CHECK(c.state == ColonistState::MovingToWork);
	// From (2,7) the first steps go east (+X) toward the door.
	CHECK_NEAR(c.yaw, -0.5 * PI, 1e-4);
	CHECK(c.position.x > c.previous_position.x);
}

TEST_CASE("simulation: a new obstacle on the route causes a detour")
{
	ColonySimulation simulation = makeSimulation(1.0f);
	simulation.tick(1.0f / 30.0f); // plans (2,7) -> (9,7)
	Cell obstacle;
	std::string reason;
	CHECK(simulation.placeDebugObstacleAhead(1, obstacle, reason));
	CHECK(simulation.getGrid().isBlocked(obstacle));
	bool stepped_on_obstacle = false;
	bool reached_store = false;
	for (int tick = 0; tick < 30 * 30; ++tick)
	{
		simulation.tick(1.0f / 30.0f);
		const ColonistRecord& c = *simulation.findColonist(1);
		stepped_on_obstacle = stepped_on_obstacle || c.last_cell == obstacle;
		reached_store = reached_store || c.last_cell == (Cell{ 9, 7 });
		for (std::size_t i = c.route.next_index; i < c.route.cells.size(); ++i)
			CHECK(!(c.route.cells[i] == obstacle));
	}
	CHECK(!stepped_on_obstacle);
	CHECK(reached_store);

	// Door cells, stops and the colonist's own cell are refused.
	CHECK(!simulation.toggleDebugObstacle(Cell{ 4, 7 }, reason));
	CHECK(!simulation.toggleDebugObstacle(Cell{ 9, 7 }, reason));
	CHECK(!simulation.toggleDebugObstacle(simulation.findColonist(1)->last_cell, reason));
	simulation.clearDebugObstacles();
	CHECK(simulation.getDebugObstacles().empty());
	CHECK(!simulation.getGrid().isBlocked(obstacle));
}

TEST_CASE("simulation: a closed door stops the route and it resumes when reopened")
{
	ColonySimulation simulation = makeSimulation(2.0f);
	simulation.setDoorOpen(0, false);
	for (int tick = 0; tick < 30 * 5; ++tick)
		simulation.tick(1.0f / 30.0f);
	const ColonistRecord& waiting = *simulation.findColonist(1);
	CHECK(waiting.route_failed);
	CHECK(waiting.state == ColonistState::Idle);
	CHECK(simulation.getGrid().getZone(waiting.last_cell) == Zone::Outside);

	simulation.setDoorOpen(0, true);
	bool reached_store = false;
	for (int tick = 0; tick < 30 * 20; ++tick)
	{
		simulation.tick(1.0f / 30.0f);
		reached_store = reached_store || simulation.findColonist(1)->last_cell == (Cell{ 9, 7 });
	}
	CHECK(reached_store);
}

TEST_CASE("simulation: closing the door mid-route never lets the colonist cross")
{
	ColonySimulation simulation = makeSimulation(2.0f);
	// Walk until one step before the door, then close it.
	for (int tick = 0; tick < 30 * 10 && !(simulation.findColonist(1)->last_cell == (Cell{ 3, 7 })); ++tick)
		simulation.tick(1.0f / 30.0f);
	CHECK(simulation.findColonist(1)->last_cell == (Cell{ 3, 7 }));
	simulation.setDoorOpen(0, false);
	for (int tick = 0; tick < 30 * 10; ++tick)
	{
		simulation.tick(1.0f / 30.0f);
		CHECK(simulation.getGrid().getZone(simulation.findColonist(1)->last_cell) == Zone::Outside);
	}
}

TEST_CASE("simulation: same inputs give the same result (deterministic replay)")
{
	ColonySimulation a = makeSimulation(1.8f);
	ColonySimulation b = makeSimulation(1.8f);
	for (int tick = 0; tick < 30 * 40; ++tick)
	{
		a.tick(1.0f / 30.0f);
		b.tick(1.0f / 30.0f);
	}
	const ColonistRecord& ca = *a.findColonist(1);
	const ColonistRecord& cb = *b.findColonist(1);
	CHECK(ca.last_cell == cb.last_cell);
	CHECK_NEAR(ca.position.x, cb.position.x, 0.0);
	CHECK_NEAR(ca.position.z, cb.position.z, 0.0);
	CHECK_EQ(ca.stop_index, cb.stop_index);
	CHECK_NEAR(a.getSimulationTime(), 40.0, 1e-3);
}

TEST_CASE("simulation: without a debug route the idle colonist waits inside the base")
{
	ColonySimulation simulation;
	std::string error;
	CHECK(simulation.initialize(makeScenario(false), makeDefinitions(2.0f), error));
	for (int tick = 0; tick < 30 * 30; ++tick)
		simulation.tick(1.0f / 30.0f);
	// P06 idle rule: no job and no dome, so it waits on an inside cell that is not a door cell.
	const ColonistRecord& c = *simulation.findColonist(1);
	CHECK(c.state == ColonistState::Idle);
	CHECK(simulation.getGrid().getZone(c.last_cell) == Zone::Inside);
	CHECK(!simulation.isDoorCell(c.last_cell));
	CHECK(c.status.find("waiting in the base") != std::string::npos);
}

TEST_CASE("simulation: debug commands return structured results")
{
	ColonySimulation simulation = makeSimulation(1.0f);
	simulation.tick(1.0f / 30.0f); // plans the first leg (2,7) -> (9,7)

	CommandResult result = simulation.apply(GameCommand::placeDebugObstacleAhead(1));
	CHECK(result.accepted);
	CHECK_EQ(result.code, std::string("ok"));
	CHECK(simulation.getGrid().isBlocked(result.cell));
	const Cell obstacle = result.cell;

	result = simulation.apply(GameCommand::toggleDebugObstacle(obstacle));
	CHECK(result.accepted);
	CHECK(!simulation.getGrid().isBlocked(obstacle));

	result = simulation.apply(GameCommand::placeDebugObstacleAhead(99));
	CHECK(!result.accepted);
	CHECK_EQ(result.code, std::string("unknown_colonist"));

	result = simulation.apply(GameCommand::toggleDebugObstacle(Cell{ 4, 7 })); // door cell
	CHECK(!result.accepted);
	CHECK_EQ(result.code, std::string("cell_refused"));
	CHECK(!result.detail.empty());

	result = simulation.apply(GameCommand::setDoorOpen(0, false));
	CHECK(result.accepted);
	CHECK(!simulation.getGrid().isDoorOpen(0));
	result = simulation.apply(GameCommand::setDoorOpen(5, true));
	CHECK(!result.accepted);
	CHECK_EQ(result.code, std::string("unknown_door"));

	CHECK(simulation.apply(GameCommand::toggleDebugObstacle(Cell{ 0, 15 })).accepted);
	result = simulation.apply(GameCommand::clearDebugObstacles());
	CHECK(result.accepted);
	CHECK(simulation.getDebugObstacles().empty());
	CHECK(!simulation.getGrid().isBlocked(Cell{ 0, 15 }));

	ColonySimulation empty;
	CHECK_EQ(empty.apply(GameCommand::clearDebugObstacles()).code, std::string("not_initialized"));
}

TEST_CASE("simulation: a debug route that needs a missing door is rejected at start")
{
	Json doc = Json::parse(R"({
		"schemaVersion": 1,
		"grid": { "width": 16, "height": 16, "cellSize": 1.0, "originXZ": [-8, -8] },
		"deposits": [], "staticBlockers": [],
		"initialColonists": [ { "id": 1, "job": "miner", "cell": [2, 7], "needs": { "hunger": 100, "thirst": 100, "oxygen": 100 } } ],
		"initialInventory": {}, "cargoCapacity": 100,
		"initialBuildings": [ { "type": "exit_door", "origin": [4, 6], "quarterTurns": 3 },
			{ "type": "corridor", "from": [7, 6], "to": [10, 7] } ],
		"debugRoute": { "colonistId": 1, "loop": false, "dwellSeconds": 0,
			"stops": [ { "label": "store", "cell": [9, 7] } ] } })");
	const ScenarioLoadResult with_door = ScenarioLoader().parse(doc, "scenario.json");
	CHECK(with_door);
	ColonySimulation ok;
	std::string error;
	CHECK(ok.initialize(with_door.scenario, makeDefinitions(2.0f), error));

	// A corridor needs the door to join the base.
	Json no_door_doc = doc;
	no_door_doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [7, 6], "to": [10, 7] } ])");
	const ScenarioLoadResult no_door = ScenarioLoader().parse(no_door_doc, "scenario.json");
	CHECK(no_door);
	ColonySimulation refused;
	CHECK(!refused.initialize(no_door.scenario, makeDefinitions(2.0f), error));
	CHECK(error.rfind("/initialBuildings/0", 0) == 0);

	// A stop the walls of blockers cut off: the loader cannot know, the simulation refuses.
	Json cut_off = doc;
	cut_off["staticBlockers"] = Json::parse(R"([ { "id": "ridge", "min": [12, 0], "max": [12, 15] } ])");
	cut_off["debugRoute"]["stops"][0]["cell"] = Json::array({ 14, 3 });
	const ScenarioLoadResult loaded = ScenarioLoader().parse(cut_off, "scenario.json");
	CHECK(loaded);
	ColonySimulation unreachable;
	CHECK(!unreachable.initialize(loaded.scenario, makeDefinitions(2.0f), error));
	CHECK(error.rfind("/debugRoute/stops/0/cell", 0) == 0);
}
