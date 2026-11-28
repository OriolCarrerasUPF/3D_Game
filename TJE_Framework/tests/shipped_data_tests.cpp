// Validates the JSON files actually shipped in data/arkfall (run from TJE_Framework/).
// Since P05B the scene holds only terrain and deposits (doors, corridors and domes are
// runtime building views); every navigation blocker in scene.json matches a blocker
// cell in scenario.json (and vice versa), and every dev scenario starts.

#include "test_harness.h"

#include "game/asset_catalog.h"
#include "game/colony_simulation.h"
#include "game/colony_ui.h"
#include "game/json_data.h"
#include "game/scene_parser.h"

#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace
{
	using json_data::Json;
	using GridKey = std::pair<int, int>;

	struct ShippedData
	{
		AssetCatalog catalog;
		SceneLoadResult scene;
		Json scenario;
		bool ok = false;
	};

	ShippedData& shipped()
	{
		static ShippedData data;
		static bool loaded = false;
		if (!loaded)
		{
			loaded = true;
			const AssetCatalogLoadResult catalog_result = data.catalog.load("data/arkfall/assets.json", "data/arkfall/materials.json");
			if (!catalog_result)
			{
				test::fail(__FILE__, __LINE__, "assets/materials: " + catalog_result.error->describe());
				return data;
			}
			data.scene = SceneParser().parse("data/arkfall/scene.json", data.catalog);
			if (!data.scene)
			{
				test::fail(__FILE__, __LINE__, "scene: " + data.scene.error->describe());
				return data;
			}
			DataLoadError error;
			if (!json_data::loadJson("data/arkfall/scenario.json", data.scenario, error))
			{
				test::fail(__FILE__, __LINE__, "scenario: " + error.describe());
				return data;
			}
			data.ok = true;
		}
		return data;
	}

	GridKey worldToCell(const Json& scenario, float x, float z)
	{
		const float origin_x = scenario["grid"]["originXZ"][0].get<float>();
		const float origin_z = scenario["grid"]["originXZ"][1].get<float>();
		const float size = scenario["grid"]["cellSize"].get<float>();
		return { static_cast<int>(std::floor((x - origin_x) / size)), static_cast<int>(std::floor((z - origin_z) / size)) };
	}
}

TEST_CASE("shipped data: catalog, materials and scene load")
{
	ShippedData& data = shipped();
	CHECK(data.ok);
	if (!data.ok)
		return;
	CHECK(data.scene.instances.size() > 4096u);
	const AssetDefinition* terrain = data.catalog.findAsset("terrain_tile");
	// The corridor floor is the walkable 0.1 m surface of the base (plan C5.2).
	const AssetDefinition* floor = data.catalog.findAsset("corridor_floor");
	CHECK(terrain && terrain->walkable_surface);
	CHECK(floor && floor->walkable_surface);
	CHECK(floor && std::fabs(floor->picking_bounds_max.y - 0.1f) < 1e-4f);
}

TEST_CASE("shipped data: the scene has no fixed base and no static colonists")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	for (const StaticSceneInstance& instance : data.scene.instances)
	{
		CHECK(instance.asset_id != "astronaut");
		CHECK(instance.asset_id != "base_boundary" && instance.asset_id != "interior_floor" && instance.asset_id != "exit_gate");
	}
	CHECK(!data.scenario.contains("base"));
	CHECK(!data.scenario.contains("debugRoute"));
}

TEST_CASE("shipped data: every scene blocker matches a scenario blocker")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	const Json& scenario = data.scenario;
	std::map<GridKey, std::string> blocked;
	for (const Json& deposit : scenario["deposits"])
		blocked[{ deposit["cell"][0].get<int>(), deposit["cell"][1].get<int>() }] = deposit["id"].get<std::string>();
	for (const Json& blocker : scenario["staticBlockers"])
		for (int x = blocker["min"][0].get<int>(); x <= blocker["max"][0].get<int>(); ++x)
			for (int z = blocker["min"][1].get<int>(); z <= blocker["max"][1].get<int>(); ++z)
				blocked[{ x, z }] = blocker["id"].get<std::string>();

	std::set<std::string> blockers_with_visual;
	for (const StaticSceneInstance& instance : data.scene.instances)
	{
		if (!instance.blocks_navigation)
			continue;
		const GridKey cell = worldToCell(scenario, instance.position.x, instance.position.z);
		const auto it = blocked.find(cell);
		CHECK(it != blocked.end());
		if (it != blocked.end())
			blockers_with_visual.insert(it->second);
	}
	// No invisible blockers: each deposit/blocker has a blocking visual.
	std::set<std::string> all_blockers;
	for (const auto& entry : blocked)
		all_blockers.insert(entry.second);
	CHECK(blockers_with_visual == all_blockers);
}

TEST_CASE("shipped data: the base modules fit the astronaut and the grid (plan C5.2)")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	const AssetDefinition* astronaut = data.catalog.findAsset("astronaut");
	const AssetDefinition* airlock = data.catalog.findAsset("airlock");
	const AssetDefinition* wall = data.catalog.findAsset("corridor_wall_frame");
	const AssetDefinition* arch = data.catalog.findAsset("corridor_arch");
	const AssetDefinition* junction = data.catalog.findAsset("corridor_junction");
	CHECK(astronaut && airlock && wall && arch && junction);
	if (!astronaut || !airlock || !wall || !arch || !junction)
		return;
	const float astronaut_width = astronaut->picking_bounds_max.x - astronaut->picking_bounds_min.x;
	const float astronaut_height = astronaut->picking_bounds_max.y - astronaut->picking_bounds_min.y;
	// A 2-cell corridor: 2 m minus two wall thicknesses is wider than the astronaut, and
	// the rail is higher than the astronaut standing on the 0.1 m floor.
	const float wall_thickness = wall->picking_bounds_max.x - wall->picking_bounds_min.x;
	CHECK(2.0f - 2.0f * wall_thickness > astronaut_width + 0.4f);
	CHECK(wall->picking_bounds_max.y > astronaut_height + 0.1f + 0.4f);
	CHECK_NEAR(arch->picking_bounds_max.x - arch->picking_bounds_min.x, 2.0, 1e-3);
	// The airlock fills the 2 x 3 Exit Door footprint.
	GameDefinitions definitions;
	CHECK(definitions.buildings.load("data/arkfall/buildings.json"));
	const BuildingDefinition* door = definitions.buildings.find(BuildingType::ExitDoor);
	CHECK(door != nullptr);
	if (door)
	{
		CHECK_NEAR(airlock->picking_bounds_max.x - airlock->picking_bounds_min.x, door->footprint_width, 1e-3);
		CHECK(airlock->picking_bounds_max.z - airlock->picking_bounds_min.z <= door->footprint_height + 1e-3f);
	}
}

TEST_CASE("shipped data: materials.json defines every HUD and marker colour")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	for (const std::string& id : ColonyUI::requiredMaterialIds())
	{
		if (!data.catalog.findMaterial(id))
			test::fail(__FILE__, __LINE__, "materials.json is missing '" + id + "'");
	}
}

TEST_CASE("shipped data: P05 start state, name pool and building visuals")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	// Shipped start: no prebuilt base, F3 cargo in the 100-unit cargo hold, no placeholders.
	CHECK(data.scenario["initialBuildings"].empty());
	CHECK(data.scenario["staticBlockers"].empty());
	CHECK_EQ(data.scenario["cargoCapacity"].get<int>(), 100);
	CHECK_EQ(data.scenario["initialInventory"]["steel"].get<int>(), 60);
	CHECK_EQ(data.scenario["initialInventory"]["components"].get<int>(), 20);
	CHECK_EQ(data.scenario["initialInventory"]["vegetables"].get<int>(), 20);
	for (const StaticSceneInstance& instance : data.scene.instances)
		CHECK(instance.asset_id != "oxygen_machine" && instance.asset_id != "storage_barrels");

	GameDefinitions definitions;
	CHECK(definitions.colonists.load("data/arkfall/colonists.json"));
	// 12 starting colonists + 188 Ark passengers (P09) need 200 unique names.
	CHECK(definitions.colonists.getNames().size() >= 200u);
	CHECK(definitions.buildings.load("data/arkfall/buildings.json"));
	CHECK(definitions.buildings.validateVisuals(data.catalog));
}

TEST_CASE("shipped data: every dev scenario starts on the shipped map")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	GameDefinitions definitions;
	CHECK(definitions.colonists.load("data/arkfall/colonists.json"));
	CHECK(definitions.buildings.load("data/arkfall/buildings.json"));
	for (const char* path : { "data/arkfall/dev/production.json", "data/arkfall/dev/p03_navigation.json",
			"data/arkfall/dev/base_layout.json", "data/arkfall/dev/survival.json",
			"data/arkfall/dev/dome_chain.json" })
	{
		const ScenarioLoadResult dev = ScenarioLoader().load(path);
		CHECK(dev);
		if (!dev)
		{
			test::fail(__FILE__, __LINE__, dev.error->describe());
			continue;
		}
		// Dev scenarios reuse scene.json, so their blockers must agree with it.
		DataLoadError error;
		CHECK(ScenarioLoader::validateSceneBlockers(dev.scenario, data.scene.instances, "data/arkfall/scene.json", error));
		ColonySimulation simulation;
		std::string init_error;
		CHECK(simulation.initialize(dev.scenario, definitions, init_error));
		if (!init_error.empty())
			test::fail(__FILE__, __LINE__, std::string(path) + ": " + init_error);
	}
}
