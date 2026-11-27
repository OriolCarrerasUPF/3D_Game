// P03 data acceptance (updated for P05B: no base block, corridor strips): ScenarioLoader,
// scene/scenario blocker agreement and the colonist role catalog.

#include "test_harness.h"

#include "game/asset_catalog.h"
#include "game/colonist_catalog.h"
#include "game/scenario_loader.h"
#include "game/scene_parser.h"

namespace
{
	using json_data::Json;

	// Small valid scenario: 16x16 grid, no predefined base (P05B).
	Json validScenario()
	{
		return Json::parse(R"({
			"schemaVersion": 1,
			"grid": { "width": 16, "height": 16, "cellSize": 1.0, "originXZ": [-8, -8] },
			"deposits": [ { "id": "iron", "resource": "iron", "cell": [1, 1] } ],
			"staticBlockers": [ { "id": "crate", "min": [7, 7], "max": [8, 8] } ],
			"initialColonists": [ { "id": 1, "job": "miner", "cell": [2, 7],
				"needs": { "hunger": 100, "thirst": 90, "oxygen": 80 } } ],
			"initialInventory": { "steel": 10 },
			"cargoCapacity": 100,
			"initialBuildings": [],
			"debugRoute": { "colonistId": 1, "loop": true, "dwellSeconds": 0.5,
				"stops": [ { "label": "mine", "cell": [2, 1] }, { "label": "store", "cell": [9, 9] } ] }
		})");
	}

	ScenarioLoadResult parse(const Json& document)
	{
		return ScenarioLoader().parse(document, "scenario.json");
	}

	void expectError(const Json& document, const std::string& pointer_fragment, const char* label)
	{
		const ScenarioLoadResult result = parse(document);
		if (result)
		{
			test::fail(__FILE__, __LINE__, std::string(label) + ": expected a load error");
			return;
		}
		if (result.error->json_pointer.find(pointer_fragment) == std::string::npos)
			test::fail(__FILE__, __LINE__, std::string(label) + ": unexpected error " + result.error->describe());
	}
}

TEST_CASE("scenario: valid document loads into typed records")
{
	const ScenarioLoadResult result = parse(validScenario());
	CHECK(result);
	if (!result)
	{
		test::fail(__FILE__, __LINE__, result.error->describe());
		return;
	}
	const ScenarioDefinition& s = result.scenario;
	CHECK_EQ(s.grid.width, 16);
	CHECK_NEAR(s.grid.origin_x, -8.0, 1e-6);
	CHECK(s.deposits[0].type == DepositType::Iron);
	CHECK(s.initial_colonists[0].job == Job::Miner);
	CHECK_NEAR(s.initial_colonists[0].needs.oxygen, 80.0, 1e-6);
	CHECK(s.debug_route.has_value());

	const NavigationGrid grid = buildNavigationGrid(s);
	CHECK(grid.isBlocked(Cell{ 1, 1 }));
	CHECK(grid.isBlocked(Cell{ 8, 8 }));
	// Every cell starts outside: inside cells and doors come from buildings (P05B).
	CHECK(grid.getZone(Cell{ 5, 5 }) == Zone::Outside);
	CHECK_EQ(grid.getDoorCount(), static_cast<std::size_t>(0));
	CHECK(grid.canStep(Cell{ 4, 7 }, Cell{ 5, 7 }));
}

TEST_CASE("scenario: rejects bad schema, unknown fields and enums")
{
	Json doc = validScenario();
	doc["schemaVersion"] = 2;
	expectError(doc, "/schemaVersion", "schema");

	doc = validScenario();
	doc["extra"] = 1;
	expectError(doc, "/extra", "unknown field");

	doc = validScenario();
	doc["deposits"][0]["resource"] = "gold";
	expectError(doc, "/deposits/0/resource", "unknown resource");

	doc = validScenario();
	doc["initialColonists"][0]["job"] = "pilot";
	expectError(doc, "/initialColonists/0/job", "unknown job");

	doc = validScenario();
	doc["initialColonists"][0]["needs"]["hunger"] = 101;
	expectError(doc, "/initialColonists/0/needs/hunger", "need range");

	doc = validScenario();
	doc["grid"].erase("cellSize");
	expectError(doc, "/grid/cellSize", "missing field");
}

TEST_CASE("scenario: rejects out-of-grid and duplicate data")
{
	Json doc = validScenario();
	doc["deposits"][0]["cell"] = Json::array({ 16, 1 });
	expectError(doc, "/deposits/0/cell/0", "out of grid");

	doc = validScenario();
	doc["staticBlockers"][0]["id"] = "iron";
	expectError(doc, "/staticBlockers/0/id", "duplicate id");

	doc = validScenario();
	doc["initialColonists"].push_back(doc["initialColonists"][0]);
	expectError(doc, "/initialColonists/1/id", "duplicate colonist");

	// The P03 base block is gone: it is now an unknown field.
	doc = validScenario();
	doc["base"] = Json::object();
	expectError(doc, "/base", "removed base block");
}

TEST_CASE("scenario: rejects impossible starts")
{
	Json doc = validScenario();
	doc["initialColonists"][0]["cell"] = Json::array({ 1, 1 });
	expectError(doc, "/initialColonists/0/cell", "colonist on deposit");

	doc = validScenario();
	doc["debugRoute"]["stops"][0]["cell"] = Json::array({ 8, 8 });
	expectError(doc, "/debugRoute/stops/0/cell", "stop on blocker");

	doc = validScenario();
	doc["debugRoute"]["colonistId"] = 7;
	expectError(doc, "/debugRoute/colonistId", "unknown colonist");

	// The debug route is optional.
	doc = validScenario();
	doc.erase("debugRoute");
	CHECK(parse(doc));
}

TEST_CASE("scenario: scene blockers must match scenario blockers")
{
	const ScenarioLoadResult result = parse(validScenario());
	CHECK(result);
	if (!result)
		return;
	auto instance = [](const char* id, float x, float z, bool blocks) {
		StaticSceneInstance item;
		item.id = id;
		item.position = Vector3(x, 0.0f, z);
		item.blocks_navigation = blocks;
		return item;
	};
	// Cell (1,1) centre = (-6.5, -6.5); blocker (7..8, 7..8) centre = (0, 0).
	std::vector<StaticSceneInstance> scene{ instance("iron_rock", -6.5f, -6.5f, true), instance("crate", 0.0f, 0.0f, true),
		instance("wall", -3.0f, -0.5f, false) };
	DataLoadError error;
	CHECK(ScenarioLoader::validateSceneBlockers(result.scenario, scene, "scene.json", error));

	std::vector<StaticSceneInstance> stray = scene;
	stray.push_back(instance("stray_wall", -3.0f, -0.5f, true));
	CHECK(!ScenarioLoader::validateSceneBlockers(result.scenario, stray, "scene.json", error));
	CHECK_EQ(error.json_pointer, std::string("/instances/stray_wall"));

	std::vector<StaticSceneInstance> invisible{ scene[0], scene[2] };
	CHECK(!ScenarioLoader::validateSceneBlockers(result.scenario, invisible, "scene.json", error));
	CHECK(error.message.find("'crate'") != std::string::npos);
}

TEST_CASE("scenario: shipped scenario.json, scene.json and colonists.json agree")
{
	const ScenarioLoadResult scenario = ScenarioLoader().load("data/arkfall/scenario.json");
	CHECK(scenario);
	if (!scenario)
	{
		test::fail(__FILE__, __LINE__, scenario.error->describe());
		return;
	}
	AssetCatalog catalog;
	CHECK(catalog.load("data/arkfall/assets.json", "data/arkfall/materials.json"));
	const SceneLoadResult scene = SceneParser().parse("data/arkfall/scene.json", catalog);
	CHECK(scene);
	DataLoadError error;
	CHECK(ScenarioLoader::validateSceneBlockers(scenario.scenario, scene.instances, "data/arkfall/scene.json", error));

	// P05B: the shipped start has no base, no door and no debug route.
	const NavigationGrid grid = buildNavigationGrid(scenario.scenario);
	CHECK_EQ(grid.getDoorCount(), static_cast<std::size_t>(0));
	CHECK(grid.getZone(Cell{ 32, 32 }) == Zone::Outside);
	CHECK(!scenario.scenario.debug_route.has_value());
	CHECK(scenario.scenario.initial_buildings.empty());

	ColonistCatalog roles;
	const ColonistCatalogLoadResult role_result = roles.load("data/arkfall/colonists.json");
	CHECK(role_result);
	CHECK(roles.validateVisuals(catalog));
	CHECK(roles.findRole(Job::Engineer) && roles.findRole(Job::Miner) && roles.findRole(Job::Farmer));
}

TEST_CASE("scenario: P05 start state (inventory, cargo capacity, initial buildings)")
{
	Json doc = validScenario();
	doc["initialInventory"] = Json::parse(R"({ "steel": 60, "components": 20, "vegetables": 20 })");
	doc["initialBuildings"] = Json::parse(R"([ { "type": "storage", "origin": [6, 6], "quarterTurns": 1 },
		{ "type": "exit_door", "origin": [2, 10], "quarterTurns": 3 },
		{ "type": "corridor", "from": [5, 11], "to": [8, 10] } ])");
	const ScenarioLoadResult result = parse(doc);
	CHECK(result);
	if (!result)
	{
		test::fail(__FILE__, __LINE__, result.error->describe());
		return;
	}
	const ScenarioDefinition& s = result.scenario;
	CHECK_EQ(s.initial_inventory[ResourceType::Steel], 60);
	CHECK_EQ(s.initial_inventory[ResourceType::Aurite], 0);
	CHECK_EQ(s.cargo_capacity, 100);
	CHECK_EQ(s.initial_buildings.size(), static_cast<std::size_t>(3));
	CHECK(!s.initial_buildings[0].is_strip);
	CHECK(s.initial_buildings[0].origin == (Cell{ 6, 6 }));
	CHECK_EQ(s.initial_buildings[0].quarter_turns, 1);
	CHECK(!s.initial_buildings[1].is_strip);
	CHECK_EQ(s.initial_buildings[1].quarter_turns, 3);
	CHECK(s.initial_buildings[2].is_strip);
	CHECK(s.initial_buildings[2].strip_from == (Cell{ 5, 11 }));
	// A strip expands row by row (2 cells across) from `from` towards `to`.
	const std::vector<Cell> cells = corridorStripCells(s.initial_buildings[2].strip_from, s.initial_buildings[2].strip_to);
	CHECK_EQ(cells.size(), static_cast<std::size_t>(8));
	CHECK(cells[0] == (Cell{ 5, 11 }));
	CHECK(cells[1] == (Cell{ 5, 10 }));
	CHECK(cells[2] == (Cell{ 6, 11 }));
	CHECK(cells[7] == (Cell{ 8, 10 }));
	const std::vector<Cell> along_z = corridorStripCells(Cell{ 3, 2 }, Cell{ 4, 4 });
	CHECK_EQ(along_z.size(), static_cast<std::size_t>(6));
	CHECK(along_z[0] == (Cell{ 3, 2 }) && along_z[1] == (Cell{ 4, 2 }) && along_z[2] == (Cell{ 3, 3 }));
}

TEST_CASE("scenario: rejects bad start state fields")
{
	Json doc = validScenario();
	doc.erase("initialBuildings");
	expectError(doc, "/initialBuildings", "missing initialBuildings");

	doc = validScenario();
	doc["initialInventory"]["gold"] = 1;
	expectError(doc, "/initialInventory/gold", "unknown item");

	doc = validScenario();
	doc["initialInventory"]["steel"] = -1;
	expectError(doc, "/initialInventory/steel", "negative item");

	doc = validScenario();
	doc["cargoCapacity"] = -5;
	expectError(doc, "/cargoCapacity", "negative capacity");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "storage", "origin": [6, 6], "quarterTurns": 4 } ])");
	expectError(doc, "/initialBuildings/0/quarterTurns", "quarter turns out of range");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "storage", "origin": [6, 6] } ])");
	expectError(doc, "/initialBuildings/0/quarterTurns", "origin without quarterTurns");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [6, 6], "to": [8, 7], "origin": [6, 6], "quarterTurns": 0 } ])");
	expectError(doc, "/initialBuildings/0", "both origin and strip");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [6, 6] } ])");
	expectError(doc, "/initialBuildings/0/to", "strip without to");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [6, 6], "to": [9, 8] } ])");
	expectError(doc, "/initialBuildings/0", "strip 3 cells wide");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [6, 6], "to": [9, 6] } ])");
	expectError(doc, "/initialBuildings/0", "strip 1 cell wide");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "storage", "from": [6, 6], "to": [9, 7] } ])");
	expectError(doc, "/initialBuildings/0/type", "strip of a non-corridor");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "corridor", "from": [6, 6], "to": [16, 7] } ])");
	expectError(doc, "/initialBuildings/0/to/0", "strip out of grid");

	doc = validScenario();
	doc["initialBuildings"] = Json::parse(R"([ { "type": "storage", "origin": [16, 6], "quarterTurns": 0 } ])");
	expectError(doc, "/initialBuildings/0/origin/0", "origin out of grid");
}
