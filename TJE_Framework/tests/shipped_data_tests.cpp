// Validates the JSON files actually shipped in data/arkfall (run from TJE_Framework/).
// These are regression checks for the pre-P03 fixes: walls are visual only, the gate
// matches its door crossings, no static colonist, and every navigation blocker in
// scene.json matches a blocker cell in scenario.json (and vice versa).

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
	const AssetDefinition* floor = data.catalog.findAsset("interior_floor");
	CHECK(terrain && terrain->walkable_surface);
	CHECK(floor && floor->walkable_surface);
	CHECK(floor && std::fabs(floor->picking_bounds_max.y - 0.1f) < 1e-4f);
}

TEST_CASE("shipped data: wall pieces are visual only and colonists are not static")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	int walls = 0;
	for (const StaticSceneInstance& instance : data.scene.instances)
	{
		if (instance.asset_id == "base_boundary")
		{
			++walls;
			CHECK(!instance.blocks_navigation);
		}
		CHECK(instance.asset_id != "astronaut");
	}
	CHECK(walls > 0);
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

TEST_CASE("shipped data: exit gate width matches its door crossings")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	const AssetDefinition* gate = data.catalog.findAsset("exit_gate");
	CHECK(gate != nullptr);
	if (!gate)
		return;
	const float gate_width_at_scale_1 = gate->picking_bounds_max.x - gate->picking_bounds_min.x;
	for (const Json& door : data.scenario["base"]["exitDoors"])
	{
		const std::string id = "exit_door_" + door["id"].get<std::string>();
		const StaticSceneInstance* visual = nullptr;
		for (const StaticSceneInstance& instance : data.scene.instances)
			if (instance.id == id)
				visual = &instance;
		CHECK(visual != nullptr);
		if (!visual)
			continue;
		const float door_width = static_cast<float>(door["crossings"].size()) * data.scenario["grid"]["cellSize"].get<float>();
		CHECK_NEAR(visual->scale.x * gate_width_at_scale_1, door_width, 1e-3);

		// No wall piece may remain on a crossing edge (the gap is as wide as the door).
		for (const Json& crossing : door["crossings"])
		{
			const GridKey inside{ crossing["inside"][0].get<int>(), crossing["inside"][1].get<int>() };
			const GridKey outside{ crossing["outside"][0].get<int>(), crossing["outside"][1].get<int>() };
			// Wall rotation that puts the slab on the inside cell's edge toward `outside`
			// (same convention as tools/generate_arkfall_scene.py).
			float edge_rotation = 0.0f;                                   // west
			if (outside.first > inside.first) edge_rotation = 180.0f;     // east
			else if (outside.second < inside.second) edge_rotation = 90.0f;  // south
			else if (outside.second > inside.second) edge_rotation = -90.0f; // north
			for (const StaticSceneInstance& instance : data.scene.instances)
			{
				if (instance.asset_id != "base_boundary")
					continue;
				const GridKey wall_cell = worldToCell(data.scenario, instance.position.x, instance.position.z);
				const bool on_crossing_edge = wall_cell == inside &&
					std::fabs(instance.rotation_y_degrees - edge_rotation) < 0.01f;
				CHECK(!on_crossing_edge);
			}
		}
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

TEST_CASE("shipped data: dev/production.json starts on the shipped map")
{
	ShippedData& data = shipped();
	if (!data.ok)
		return;
	const ScenarioLoadResult dev = ScenarioLoader().load("data/arkfall/dev/production.json");
	CHECK(dev);
	if (!dev)
	{
		test::fail(__FILE__, __LINE__, dev.error->describe());
		return;
	}
	// Dev scenarios reuse scene.json, so their blockers must agree with it.
	DataLoadError error;
	CHECK(ScenarioLoader::validateSceneBlockers(dev.scenario, data.scene.instances, "data/arkfall/scene.json", error));
	GameDefinitions definitions;
	CHECK(definitions.colonists.load("data/arkfall/colonists.json"));
	CHECK(definitions.buildings.load("data/arkfall/buildings.json"));
	ColonySimulation simulation;
	std::string init_error;
	CHECK(simulation.initialize(dev.scenario, definitions, init_error));
	if (!init_error.empty())
		test::fail(__FILE__, __LINE__, init_error);
}
