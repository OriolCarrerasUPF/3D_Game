// P05 building data: the shipped buildings.json, the strict BuildingCatalog loader
// (rejection cases from plan P05) and the shared quarter-turn footprint transform (C5).

#include "test_harness.h"

#include "game/asset_catalog.h"
#include "game/building_catalog.h"

#include <set>
#include <utility>

namespace
{
	using json_data::Json;

	Json shippedDocument()
	{
		Json document;
		DataLoadError error;
		if (!json_data::loadJson("data/arkfall/buildings.json", document, error))
			test::fail(__FILE__, __LINE__, error.describe());
		return document;
	}

	std::size_t indexOf(const Json& document, const std::string& id)
	{
		for (std::size_t i = 0; i < document["buildings"].size(); ++i)
			if (document["buildings"][i]["id"] == id)
				return i;
		test::fail(__FILE__, __LINE__, "no building '" + id + "' in buildings.json");
		return 0;
	}

	// Expects a load error whose JSON pointer contains `pointer_fragment`.
	void expectError(const Json& document, const std::string& pointer_fragment, const char* label)
	{
		BuildingCatalog catalog;
		const BuildingCatalogLoadResult result = catalog.parse(document, "buildings.json");
		if (result)
		{
			test::fail(__FILE__, __LINE__, std::string(label) + ": expected a load error");
			return;
		}
		if (result.error->json_pointer.find(pointer_fragment) == std::string::npos)
			test::fail(__FILE__, __LINE__, std::string(label) + ": unexpected error " + result.error->describe());
		if (!catalog.empty())
			test::fail(__FILE__, __LINE__, std::string(label) + ": a failed load must leave the catalog empty");
	}
}

TEST_CASE("buildings: shipped buildings.json has the 13 MVP types with the C8 values")
{
	BuildingCatalog catalog;
	const BuildingCatalogLoadResult result = catalog.load("data/arkfall/buildings.json");
	CHECK(result);
	if (!result)
	{
		test::fail(__FILE__, __LINE__, result.error->describe());
		return;
	}
	CHECK_EQ(catalog.getDefinitions().size(), building_type_count);
	for (std::size_t i = 0; i < building_type_count; ++i)
		CHECK(catalog.find(static_cast<BuildingType>(i)) != nullptr);

	const BuildingDefinition* storage = catalog.find(BuildingType::Storage);
	const BuildingDefinition* mine = catalog.find(BuildingType::IronMine);
	const BuildingDefinition* processor = catalog.find(BuildingType::IronProcessor);
	const BuildingDefinition* door = catalog.find(BuildingType::ExitDoor);
	const BuildingDefinition* beacon = catalog.find(BuildingType::HavenBeacon);
	const BuildingDefinition* greenhouse = catalog.find(BuildingType::Greenhouse);
	if (!storage || !mine || !processor || !door || !beacon || !greenhouse)
		return;
	CHECK_EQ(storage->storage_capacity, 100);
	CHECK(storage->offersService(ServiceType::Eat) && storage->offersService(ServiceType::Drink));
	CHECK(!storage->has_operation);
	CHECK(mine->has_operation && mine->operation_role == Job::Miner);
	CHECK_EQ(mine->operation_slots, 5);
	CHECK(mine->required_deposit && *mine->required_deposit == DepositType::Iron);
	CHECK(mine->recipe && mine->recipe->per_present_worker);
	CHECK_EQ(mine->recipe->outputs[ResourceType::IronOre], 1);
	CHECK_NEAR(mine->recipe->seconds, 5.0, 1e-6);
	CHECK(processor->recipe && !processor->recipe->per_present_worker);
	CHECK_EQ(processor->recipe->inputs[ResourceType::IronOre], 1);
	CHECK_EQ(processor->recipe->outputs[ResourceType::Steel], 1);
	CHECK_EQ(processor->operation_slots, 1);
	CHECK(door->placement == PlacementKind::DoorSocket && door->zone == BuildingZone::Boundary);
	CHECK(door->access_offsets.empty());
	CHECK_EQ(beacon->cost[ResourceType::Aurite], 300);
	CHECK_NEAR(beacon->power_demand, 0.0, 1e-6);
	CHECK_EQ(beacon->footprint_width, 4);
	CHECK(greenhouse->operation_role == Job::Farmer);
	CHECK_EQ(greenhouse->footprint_width, 3);
	for (const BuildingDefinition& def : catalog.getDefinitions())
	{
		CHECK(def.construction_role == Job::Engineer);
		CHECK_EQ(def.construction_slots, 1);
		CHECK(!def.visuals.empty());
	}

	// Every visual asset and material exists in the shipped asset catalog.
	AssetCatalog assets;
	CHECK(assets.load("data/arkfall/assets.json", "data/arkfall/materials.json"));
	const BuildingCatalogLoadResult visuals = catalog.validateVisuals(assets);
	CHECK(visuals);
	if (!visuals)
		test::fail(__FILE__, __LINE__, visuals.error->describe());
}

TEST_CASE("buildings: the quarter-turn transform keeps footprints and access cells consistent")
{
	BuildingCatalog catalog;
	if (!catalog.load("data/arkfall/buildings.json"))
		return;
	for (const BuildingDefinition& def : catalog.getDefinitions())
	{
		if (def.placement != PlacementKind::Footprint)
			continue;
		for (int q = 0; q < 4; ++q)
		{
			int w = 0, h = 0;
			footprint::rotatedSize(def.footprint_width, def.footprint_height, q, w, h);
			CHECK_EQ(w * h, def.footprint_width * def.footprint_height);
			CHECK_EQ(w, q % 2 ? def.footprint_height : def.footprint_width);
			// Footprint cells map one-to-one onto the rotated rectangle.
			std::set<std::pair<int, int>> cells;
			for (int dz = 0; dz < def.footprint_height; ++dz)
				for (int dx = 0; dx < def.footprint_width; ++dx)
				{
					const Cell c = footprint::rotateOffset(Cell{ dx, dz }, def.footprint_width, def.footprint_height, q);
					CHECK(c.x >= 0 && c.x < w && c.z >= 0 && c.z < h);
					cells.insert({ c.x, c.z });
				}
			CHECK_EQ(cells.size(), static_cast<std::size_t>(w * h));
			// Access offsets stay outside and edge-adjacent after the same transform.
			for (Cell offset : def.access_offsets)
				CHECK(footprint::isAdjacentOutside(footprint::rotateOffset(offset, def.footprint_width, def.footprint_height, q), w, h));
		}
	}

	// q = 1 maps a +X step to a +Z step (world_transform: +90 deg maps +X to +Z).
	const Cell a = footprint::rotateOffset(Cell{ 0, 0 }, 3, 2, 1);
	const Cell b = footprint::rotateOffset(Cell{ 1, 0 }, 3, 2, 1);
	CHECK_EQ(b.x - a.x, 0);
	CHECK_EQ(b.z - a.z, 1);
	// Four quarter turns (sizes swapping each time) return every offset to itself.
	for (Cell start : { Cell{ 0, 0 }, Cell{ 2, 1 }, Cell{ -1, 0 }, Cell{ 1, 2 } })
	{
		Cell c = start;
		int w = 3, h = 2;
		for (int turn = 0; turn < 4; ++turn)
		{
			c = footprint::rotateOffset(c, w, h, 1);
			std::swap(w, h);
		}
		CHECK(c == start);
	}
	CHECK_EQ(footprint::normalizeQuarterTurns(-1), 3);
	CHECK_EQ(footprint::normalizeQuarterTurns(5), 1);
	CHECK(!footprint::isAdjacentOutside(Cell{ -1, -1 }, 2, 2)); // diagonal corner
	CHECK(!footprint::isAdjacentOutside(Cell{ 1, 1 }, 2, 2));   // inside
	CHECK(footprint::isAdjacentOutside(Cell{ 2, 1 }, 2, 2));
}

TEST_CASE("buildings: rejects a missing type, duplicate and unknown ids")
{
	Json doc = shippedDocument();
	doc["buildings"].erase(indexOf(doc, "greenhouse"));
	expectError(doc, "/buildings", "missing type");

	doc = shippedDocument();
	doc["buildings"].push_back(doc["buildings"][0]);
	expectError(doc, "/buildings/13/id", "duplicate id");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["id"] = "silo";
	expectError(doc, "/id", "unknown type id");

	doc = shippedDocument();
	doc["schemaVersion"] = 2;
	expectError(doc, "/schemaVersion", "schema");
}

TEST_CASE("buildings: rejects unknown roles, deposits, items and fields")
{
	Json doc = shippedDocument();
	doc["buildings"][indexOf(doc, "iron_mine")]["operation"]["role"] = "pilot";
	expectError(doc, "/operation/role", "unknown role");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "iron_mine")]["requiredDeposit"] = "gold";
	expectError(doc, "/requiredDeposit", "unknown deposit");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["requiredDeposit"] = "iron";
	expectError(doc, "/requiredDeposit", "deposit on an interior building");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["cost"]["gold"] = 1;
	expectError(doc, "/cost/gold", "unknown item");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["cost"]["steel"] = -5;
	expectError(doc, "/cost/steel", "negative cost");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["colour"] = "red";
	expectError(doc, "/colour", "unknown field");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")].erase("services");
	expectError(doc, "/services", "missing key (absent features must be explicit)");
}

TEST_CASE("buildings: rejects invalid footprints and access offsets")
{
	Json doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["footprint"] = Json::array({ 0, 2 });
	expectError(doc, "/footprint/0", "zero width");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["footprint"] = Json::array({ 2 });
	expectError(doc, "/footprint", "one component");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["footprint"] = nullptr;
	expectError(doc, "/footprint", "footprint building without footprint");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "exit_door")]["footprint"] = Json::array({ 1, 1 });
	expectError(doc, "/footprint", "door socket with a footprint");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["accessOffsets"][0] = Json::array({ 1, 1 });
	expectError(doc, "/accessOffsets/0", "access inside the footprint");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["accessOffsets"][0] = Json::array({ -1, -1 });
	expectError(doc, "/accessOffsets/0", "diagonal access");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["accessOffsets"][1] = doc["buildings"][indexOf(doc, "storage")]["accessOffsets"][0];
	expectError(doc, "/accessOffsets/1", "duplicate access");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["accessOffsets"] = Json::array();
	expectError(doc, "/accessOffsets", "no access cell");
}

TEST_CASE("buildings: rejects recipes without workers and unknown visual assets")
{
	Json doc = shippedDocument();
	doc["buildings"][indexOf(doc, "iron_processor")]["operation"] = nullptr;
	expectError(doc, "/recipe", "recipe without operation");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "iron_processor")]["recipe"] = nullptr;
	expectError(doc, "/operation", "operation without recipe");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "iron_mine")]["recipe"]["outputs"] = Json::object();
	expectError(doc, "/recipe/outputs", "recipe producing nothing");

	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["visuals"] = Json::array();
	expectError(doc, "/visuals", "no visual");

	// Unknown visual asset: parses, then fails against the asset catalog.
	doc = shippedDocument();
	doc["buildings"][indexOf(doc, "storage")]["visuals"][0]["assetId"] = "no_such_model";
	BuildingCatalog catalog;
	CHECK(catalog.parse(doc, "buildings.json"));
	AssetCatalog assets;
	CHECK(assets.load("data/arkfall/assets.json", "data/arkfall/materials.json"));
	const BuildingCatalogLoadResult visuals = catalog.validateVisuals(assets);
	CHECK(!visuals);
	if (!visuals)
		CHECK(visuals.error->json_pointer.find("/buildings/storage/visuals/0/assetId") != std::string::npos);
}
