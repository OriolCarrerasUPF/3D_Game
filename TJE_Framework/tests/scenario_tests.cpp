// P03 data acceptance: ScenarioLoader, scene/scenario blocker agreement and the
// colonist role catalog.

#include "test_harness.h"

#include "game/asset_catalog.h"
#include "game/colonist_catalog.h"
#include "game/scenario_loader.h"
#include "game/scene_parser.h"

namespace
{
	using json_data::Json;

	// Small valid scenario: 16x16 grid, base x=5..10 z=5..10, door (4,7)->(5,7).
	Json validScenario()
	{
		return Json::parse(R"({
			"schemaVersion": 1,
			"grid": { "width": 16, "height": 16, "cellSize": 1.0, "originXZ": [-8, -8] },
			"base": {
				"interior": { "min": [5, 5], "max": [10, 10] },
				"exitDoors": [ { "id": "door", "openAtStart": true,
					"crossings": [ { "outside": [4, 7], "inside": [5, 7] } ] } ]
			},
			"deposits": [ { "id": "iron", "resource": "iron", "cell": [1, 1] } ],
			"staticBlockers": [ { "id": "crate", "min": [7, 7], "max": [8, 8] } ],
			"initialColonists": [ { "id": 1, "job": "miner", "cell": [2, 7],
				"needs": { "hunger": 100, "thirst": 90, "oxygen": 80 } } ],
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
	CHECK(s.interior_min == (Cell{ 5, 5 }));
	CHECK_EQ(s.exit_doors.size(), static_cast<std::size_t>(1));
	CHECK(s.exit_doors[0].crossings[0].inside == (Cell{ 5, 7 }));
	CHECK(s.deposits[0].type == DepositType::Iron);
	CHECK(s.initial_colonists[0].job == Job::Miner);
	CHECK_NEAR(s.initial_colonists[0].needs.oxygen, 80.0, 1e-6);
	CHECK(s.debug_route.has_value());

	const NavigationGrid grid = buildNavigationGrid(s);
	CHECK(grid.isBlocked(Cell{ 1, 1 }));
	CHECK(grid.isBlocked(Cell{ 8, 8 }));
	CHECK(grid.getZone(Cell{ 5, 5 }) == Zone::Inside);
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

	doc = validScenario();
	doc["base"]["interior"]["min"] = Json::array({ 0, 5 });
	expectError(doc, "/base/interior", "no exterior margin");
}

TEST_CASE("scenario: rejects invalid door crossings")
{
	Json doc = validScenario();
	doc["base"]["exitDoors"][0]["crossings"][0]["outside"] = Json::array({ 3, 7 });
	expectError(doc, "/crossings/0", "not neighbours");

	doc = validScenario();
	doc["base"]["exitDoors"][0]["crossings"][0] = Json::parse(R"({ "outside": [5, 7], "inside": [4, 7] })");
	expectError(doc, "/crossings/0/outside", "zones swapped");

	doc = validScenario();
	doc["base"]["exitDoors"][0]["crossings"].push_back(Json::parse(R"({ "outside": [4, 9], "inside": [5, 9] })"));
	expectError(doc, "/crossings", "not contiguous");

	doc = validScenario();
	doc["base"]["exitDoors"][0]["crossings"].push_back(Json::parse(R"({ "outside": [7, 4], "inside": [7, 5] })"));
	expectError(doc, "/crossings/1", "different sides");

	doc = validScenario();
	doc["base"]["exitDoors"] = Json::array();
	expectError(doc, "/base/exitDoors", "no door");

	doc = validScenario();
	doc["staticBlockers"][0]["min"] = Json::array({ 5, 7 });
	doc["staticBlockers"][0]["max"] = Json::array({ 5, 7 });
	expectError(doc, "/base/exitDoors/0", "door cell blocked");
}

TEST_CASE("scenario: rejects impossible starts and unreachable routes")
{
	Json doc = validScenario();
	doc["initialColonists"][0]["cell"] = Json::array({ 1, 1 });
	expectError(doc, "/initialColonists/0/cell", "colonist on deposit");

	doc = validScenario();
	doc["debugRoute"]["stops"][0]["cell"] = Json::array({ 8, 8 });
	expectError(doc, "/debugRoute/stops/0/cell", "stop on blocker");

	doc = validScenario();
	doc["base"]["exitDoors"][0]["openAtStart"] = false;
	expectError(doc, "/debugRoute/stops/1/cell", "route through closed door");

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

	// The P03 route crosses the 2-cell west door: (23,31)->(24,31) and (23,32)->(24,32).
	const NavigationGrid grid = buildNavigationGrid(scenario.scenario);
	CHECK(grid.canStep(Cell{ 23, 31 }, Cell{ 24, 31 }));
	CHECK(grid.canStep(Cell{ 23, 32 }, Cell{ 24, 32 }));
	CHECK(!grid.canStep(Cell{ 23, 30 }, Cell{ 24, 30 }));
	CHECK(!grid.canStep(Cell{ 30, 23 }, Cell{ 30, 24 }));

	ColonistCatalog roles;
	const ColonistCatalogLoadResult role_result = roles.load("data/arkfall/colonists.json");
	CHECK(role_result);
	CHECK(roles.validateVisuals(catalog));
	CHECK(roles.findRole(Job::Engineer) && roles.findRole(Job::Miner) && roles.findRole(Job::Farmer));
}
