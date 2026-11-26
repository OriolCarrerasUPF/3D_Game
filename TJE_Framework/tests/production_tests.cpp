// P05 production loop (headless): building records, placement rules, inventory,
// work-slot reservation/release, nearest-job selection, production from present
// workers, atomic processing, capacity limits and the AssignJob order.
// Scenarios are variations of data/arkfall/dev/production.json on the shipped map.

#include "test_harness.h"

#include "game/colony_simulation.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace
{
	using json_data::Json;
	constexpr float dt = 1.0f / 30.0f;

	Json loadJson(const char* path)
	{
		Json document;
		DataLoadError error;
		if (!json_data::loadJson(path, document, error))
			test::fail(__FILE__, __LINE__, error.describe());
		return document;
	}

	// Shipped colonists.json + buildings.json, optionally with edited building data.
	GameDefinitions makeDefinitions(const Json* buildings_override = nullptr)
	{
		GameDefinitions definitions;
		const ColonistCatalogLoadResult colonists = definitions.colonists.load("data/arkfall/colonists.json");
		if (!colonists)
			test::fail(__FILE__, __LINE__, colonists.error->describe());
		const BuildingCatalogLoadResult buildings = buildings_override
			? definitions.buildings.parse(*buildings_override, "buildings.json")
			: definitions.buildings.load("data/arkfall/buildings.json");
		if (!buildings)
			test::fail(__FILE__, __LINE__, buildings.error->describe());
		return definitions;
	}

	Json devScenario() { return loadJson("data/arkfall/dev/production.json"); }

	Json colonist(int id, const char* job, int x, int z)
	{
		return Json{ { "id", id }, { "job", job }, { "cell", Json::array({ x, z }) },
			{ "needs", { { "hunger", 100 }, { "thirst", 100 }, { "oxygen", 100 } } } };
	}

	Json building(const char* type, int x, int z, int quarter_turns = 0)
	{
		return Json{ { "type", type }, { "origin", Json::array({ x, z }) }, { "quarterTurns", quarter_turns } };
	}

	bool start(ColonySimulation& simulation, const Json& scenario, const GameDefinitions& definitions, std::string* error_out = nullptr)
	{
		const ScenarioLoadResult loaded = ScenarioLoader().parse(scenario, "scenario.json");
		if (!loaded)
		{
			if (error_out)
				*error_out = loaded.error->describe();
			else
				test::fail(__FILE__, __LINE__, loaded.error->describe());
			return false;
		}
		std::string error;
		const bool ok = simulation.initialize(loaded.scenario, definitions, error);
		if (error_out)
			*error_out = error;
		else if (!ok)
			test::fail(__FILE__, __LINE__, error);
		return ok;
	}

	void run(ColonySimulation& simulation, float seconds)
	{
		const int ticks = static_cast<int>(seconds * 30.0f + 0.5f);
		for (int i = 0; i < ticks; ++i)
			simulation.tick(dt);
	}

	const BuildingRecord* firstOfType(const ColonySimulation& simulation, BuildingType type)
	{
		for (const BuildingRecord& building : simulation.getBuildings())
			if (building.type == type)
				return &building;
		return nullptr;
	}

	// Reservation bookkeeping is consistent in both directions and never overstaffed.
	void checkReservations(const ColonySimulation& simulation)
	{
		std::set<ColonistId> owners;
		for (const BuildingRecord& building : simulation.getBuildings())
		{
			CHECK(simulation.countReservedWorkers(building) <= static_cast<int>(building.slots.size()));
			CHECK(simulation.countPresentWorkers(building) <= simulation.countReservedWorkers(building));
			const BuildingDefinition* def = simulation.findDefinition(building.type);
			for (std::size_t slot = 0; slot < building.slots.size(); ++slot)
			{
				const ColonistId id = building.slots[slot];
				if (id == INVALID_COLONIST)
					continue;
				CHECK(owners.insert(id).second); // one slot per colonist
				const ColonistRecord* owner = simulation.findColonist(id);
				CHECK(owner != nullptr);
				if (!owner)
					continue;
				CHECK_EQ(owner->reservation.building, building.id);
				CHECK_EQ(owner->reservation.slot, static_cast<int>(slot));
				CHECK(def && owner->job == def->operation_role);
			}
		}
		for (const ColonistRecord& c : simulation.getColonists())
		{
			if (c.reservation.active())
				CHECK(owners.count(c.id) == 1);
			if (c.state == ColonistState::Working)
				CHECK(c.reservation.active());
		}
	}
}

TEST_CASE("production: dev scenario places its buildings, blocks footprints and sets capacity")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	CHECK_EQ(simulation.getBuildings().size(), static_cast<std::size_t>(4));
	const NavigationGrid& grid = simulation.getGrid();
	const NavigationGrid bare = buildNavigationGrid(simulation.getScenario());
	CHECK(grid.getRevision() > bare.getRevision()); // footprints bumped the revision
	for (const BuildingRecord& building : simulation.getBuildings())
	{
		CHECK(building.isComplete());
		for (Cell cell : building.footprint_cells)
			CHECK(grid.isBlocked(cell));
		for (Cell cell : building.access_cells)
			CHECK(grid.isWalkable(cell));
	}
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	CHECK(mine && mine->deposit_index && *mine->deposit_index == 0u);
	CHECK(mine && mine->slots.size() == 5u);
	// Two complete Storages: 200 shared item units; the cargo capacity no longer applies.
	CHECK(!simulation.usesCargoCapacity());
	CHECK_EQ(simulation.getInventory().getCapacity(), 200);
	CHECK_EQ(simulation.getInventory().getTotal(), 60);
	// Names from the pool in colonist-ID order.
	CHECK_EQ(simulation.findColonist(1)->name, std::string("Ada Vance"));
	CHECK_EQ(simulation.findColonist(8)->name, std::string("Hugo Hale"));
}

TEST_CASE("production: a Miner reserves before walking and produces only while present")
{
	Json scenario = devScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 18, 31) });
	scenario["initialBuildings"] = Json::array({ building("iron_mine", 8, 12) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	CHECK(simulation.usesCargoCapacity());
	CHECK_EQ(simulation.getInventory().getCapacity(), 100);

	simulation.tick(dt);
	const ColonistRecord* miner = simulation.findColonist(1);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	CHECK(miner->state == ColonistState::MovingToWork);
	CHECK(miner->reservation.active());                 // the slot is taken before walking
	CHECK_EQ(simulation.countReservedWorkers(*mine), 1);
	CHECK_EQ(simulation.countPresentWorkers(*mine), 0);

	// While walking nothing is produced.
	float walked = dt;
	while (miner->state == ColonistState::MovingToWork && walked < 60.0f)
	{
		CHECK_EQ(simulation.getInventory().get(ResourceType::IronOre), 0);
		simulation.tick(dt);
		walked += dt;
	}
	CHECK(miner->state == ColonistState::Working);
	CHECK(mine->footprint_cells.size() == 4u);
	bool on_access_cell = false;
	for (Cell access : mine->access_cells)
		on_access_cell = on_access_cell || access == miner->last_cell;
	CHECK(on_access_cell);
	CHECK_EQ(simulation.countPresentWorkers(*mine), 1);
	CHECK_EQ(simulation.getInventory().get(ResourceType::IronOre), 0);

	// One present Miner: one ore per 5 s.
	run(simulation, 10.2f);
	CHECK_EQ(simulation.getInventory().get(ResourceType::IronOre), 2);
	CHECK_EQ(mine->cycles_completed, 2);
	CHECK(mine->status == BuildingStatus::Running);
	checkReservations(simulation);
}

TEST_CASE("production: present Miners add up (5 Miners -> 5 ore per 5 s)")
{
	Json scenario = devScenario();
	scenario["initialBuildings"] = Json::array({ building("iron_mine", 8, 12) });
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 7, 14), colonist(2, "miner", 7, 15),
		colonist(3, "miner", 6, 14), colonist(4, "miner", 6, 15), colonist(5, "miner", 5, 14) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	for (int i = 0; i < 30 * 30 && simulation.countPresentWorkers(*mine) < 5; ++i)
		simulation.tick(dt);
	CHECK_EQ(simulation.countPresentWorkers(*mine), 5);
	const int before = simulation.getInventory().get(ResourceType::IronOre);
	run(simulation, 10.0f);
	const int made = simulation.getInventory().get(ResourceType::IronOre) - before;
	CHECK(made >= 9 && made <= 11);
}

TEST_CASE("production: the spare Miner staffs the processor once ore exists (acceptance)")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	simulation.tick(dt);
	// Miners 1-5 reserve the five mine slots; Miner 6 cannot: the processor has no ore yet.
	const ColonistRecord* spare = simulation.findColonist(6);
	CHECK(!spare->reservation.active());
	CHECK(spare->status.find("missing Iron ore") != std::string::npos);
	const ColonistRecord* engineer = simulation.findColonist(7);
	CHECK(engineer->status.find("no workplace for Engineers") != std::string::npos);

	const BuildingRecord* processor = firstOfType(simulation, BuildingType::IronProcessor);
	bool staffed = false;
	for (int i = 0; i < 30 * 90 && !staffed; ++i)
	{
		simulation.tick(dt);
		checkReservations(simulation);
		staffed = spare->state == ColonistState::Working && spare->reservation.building == processor->id;
	}
	CHECK(staffed);
	run(simulation, 20.0f);
	CHECK(processor->cycles_completed >= 3);
	CHECK(simulation.getInventory().get(ResourceType::Steel) > 40);
}

TEST_CASE("production: two Miners competing for the last slot")
{
	Json buildings = loadJson("data/arkfall/buildings.json");
	for (Json& def : buildings["buildings"])
		if (def["id"] == "iron_mine")
			def["operation"]["slots"] = 1;
	const GameDefinitions definitions = makeDefinitions(&buildings);

	Json scenario = devScenario();
	scenario["initialBuildings"] = Json::array({ building("iron_mine", 8, 12) });
	// Colonist 2 is listed first in the file and is even closer; colonists still choose in ID order.
	scenario["initialColonists"] = Json::array({ colonist(2, "miner", 4, 12), colonist(1, "miner", 7, 8) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, definitions))
		return;
	simulation.tick(dt);
	const ColonistRecord* first = simulation.findColonist(1);
	const ColonistRecord* second = simulation.findColonist(2);
	// Colonists choose in ID order: colonist 1 wins, colonist 2 finds the slot taken.
	CHECK(first->reservation.active());
	CHECK(!second->reservation.active());
	CHECK(second->status.find("every Miner slot is taken") != std::string::npos);
	for (int i = 0; i < 30 * 30; ++i)
	{
		simulation.tick(dt);
		checkReservations(simulation);
	}
	CHECK(first->state == ColonistState::Working);
	CHECK(second->state == ColonistState::Idle);
	CHECK_EQ(simulation.countPresentWorkers(*firstOfType(simulation, BuildingType::IronMine)), 1);
}

TEST_CASE("production: repeated role changes always release the slot")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	run(simulation, 25.0f);
	const ColonistRecord* miner = simulation.findColonist(1);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	CHECK(miner->state == ColonistState::Working);
	const int reserved = simulation.countReservedWorkers(*mine);

	std::string reason;
	CHECK(simulation.assignJob(1, Job::Engineer, reason));
	CHECK(!miner->reservation.active());
	CHECK(miner->state == ColonistState::Idle);
	CHECK_EQ(simulation.countReservedWorkers(*mine), reserved - 1);
	checkReservations(simulation);

	for (int round = 0; round < 12; ++round)
	{
		const Job next = round % 2 == 0 ? Job::Miner : Job::Engineer;
		CHECK(simulation.assignJob(1, next, reason));
		// Some changes land mid-walk, some while working.
		run(simulation, 0.4f + 1.7f * static_cast<float>(round % 3));
		checkReservations(simulation);
		if (next == Job::Engineer)
			CHECK(!miner->reservation.active());
	}
	CHECK(simulation.assignJob(1, Job::Miner, reason));
	run(simulation, 40.0f);
	checkReservations(simulation);
	CHECK(miner->state == ColonistState::Working);
}

TEST_CASE("production: processing runs while the inventory is exactly full")
{
	Json scenario = devScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 26, 33) });
	scenario["initialBuildings"] = Json::array({ building("iron_mine", 8, 12), building("iron_processor", 28, 33) });
	scenario["cargoCapacity"] = 20;
	scenario["initialInventory"] = Json{ { "ironOre", 3 }, { "steel", 17 } };
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	CHECK_EQ(simulation.getInventory().getTotal(), simulation.getInventory().getCapacity());
	run(simulation, 30.0f);
	const Inventory& inventory = simulation.getInventory();
	// 1 ore -> 1 Steel has net change 0, so it runs at exactly full; ore runs out after 3.
	CHECK_EQ(inventory.get(ResourceType::IronOre), 0);
	CHECK_EQ(inventory.get(ResourceType::Steel), 20);
	CHECK_EQ(inventory.getTotal(), 20);
	const BuildingRecord* processor = firstOfType(simulation, BuildingType::IronProcessor);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	CHECK_EQ(processor->cycles_completed, 3);
	CHECK(processor->status == BuildingStatus::MissingInput);
	// The mine can never be worked: its output would not fit.
	CHECK(mine->status == BuildingStatus::StorageFull);
	CHECK_EQ(mine->cycles_completed, 0);
	// The input ran out, so the worker's job was invalidated and the slot released.
	CHECK(!simulation.findColonist(1)->reservation.active());
	CHECK(simulation.findColonist(1)->status.find("full") != std::string::npos);
	checkReservations(simulation);
}

TEST_CASE("production: full Storage stops the mine and never deletes items")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	const BuildingRecord* processor = firstOfType(simulation, BuildingType::IronProcessor);
	const Inventory& inventory = simulation.getInventory();
	for (int i = 0; i < 30 * 300; ++i)
	{
		simulation.tick(dt);
		CHECK(inventory.getTotal() <= inventory.getCapacity());
		// Conservation: every item is accounted for by completed cycles.
		CHECK_EQ(inventory.get(ResourceType::IronOre), mine->cycles_completed - processor->cycles_completed);
		CHECK_EQ(inventory.get(ResourceType::Steel), 40 + processor->cycles_completed);
		CHECK_EQ(inventory.getTotal(), 60 + mine->cycles_completed);
	}
	CHECK_EQ(inventory.getTotal(), 200);
	CHECK(mine->status == BuildingStatus::StorageFull);
	CHECK_EQ(simulation.countReservedWorkers(*mine), 0);
	CHECK(mine->production_progress <= 5.0f + 1e-4f); // at most one ready cycle retained
	for (ColonistId id : { 1u, 2u, 3u, 4u, 5u })
	{
		CHECK(simulation.findColonist(id)->state == ColonistState::Idle);
		CHECK(simulation.findColonist(id)->status.find("Storage full") != std::string::npos);
	}
	checkReservations(simulation);
}

TEST_CASE("production: an Exit Door building completes onto its socket and opens it")
{
	Json scenario = devScenario();
	scenario["base"]["exitDoors"][0]["openAtStart"] = false;
	scenario["initialBuildings"] = Json::array({ Json{ { "type", "exit_door" }, { "socket", "west_door" } } });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	CHECK(simulation.getGrid().isDoorOpen(0));
	const BuildingRecord* door = firstOfType(simulation, BuildingType::ExitDoor);
	CHECK(door && door->door_index && *door->door_index == 0u);
	CHECK(door && door->footprint_cells.empty());
	CHECK(door && door->access_cells.size() == 4u); // 2 crossings: outside + inside cells
	CHECK(!simulation.validateDoorPlacement(0).valid); // the socket is taken
	CHECK_EQ(simulation.validateDoorPlacement(0).code, std::string("socket_taken"));

	// A door socket given twice is rejected by the loader.
	scenario["initialBuildings"].push_back(scenario["initialBuildings"][0]);
	std::string error;
	ColonySimulation twice;
	CHECK(!start(twice, scenario, makeDefinitions(), &error));
	CHECK(error.find("/initialBuildings/1/socket") != std::string::npos);
}

TEST_CASE("production: placement rules for footprints, zones, deposits and access")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	auto code = [&simulation](BuildingType type, int x, int z, int q = 0) {
		return simulation.validatePlacement(type, Cell{ x, z }, q).code;
	};
	CHECK_EQ(code(BuildingType::CopperMine, 55, 12), std::string("ok"));
	CHECK_EQ(code(BuildingType::CopperMine, 54, 11), std::string("ok"));      // deposit in another footprint cell
	CHECK_EQ(code(BuildingType::IronMine, 20, 10), std::string("needs_deposit"));
	CHECK_EQ(code(BuildingType::IronMine, 55, 12), std::string("wrong_deposit"));
	CHECK_EQ(code(BuildingType::IronMine, 7, 11), std::string("deposit_taken"));
	CHECK_EQ(code(BuildingType::SolarPanel, 55, 12), std::string("deposit_in_the_way"));
	CHECK_EQ(code(BuildingType::Storage, 10, 30), std::string("wrong_zone"));
	CHECK_EQ(code(BuildingType::SolarPanel, 30, 30), std::string("wrong_zone"));
	CHECK_EQ(code(BuildingType::Storage, 31, 27), std::string("occupied"));
	CHECK_EQ(code(BuildingType::Greenhouse, 24, 30, 1), std::string("door_cells")); // covers (24,31),(24,32)
	CHECK_EQ(code(BuildingType::SolarPanel, 17, 29), std::string("spawn_cell"));
	CHECK_EQ(code(BuildingType::LandingPad, 61, 61), std::string("out_of_bounds"));
	CHECK_EQ(code(BuildingType::Storage, 32, 25), std::string("blocks_access")); // covers Storage #2's (32,26)
	CHECK_EQ(code(BuildingType::ExitDoor, 30, 30), std::string("wrong_placement"));
	CHECK_EQ(code(BuildingType::Storage, 30, 36), std::string("ok"));

	// Rotation: the same transform drives cells and access cells (C5).
	const PlacementResult rotated = simulation.validatePlacement(BuildingType::Greenhouse, Cell{ 30, 35 }, 1);
	CHECK(rotated.valid);
	CHECK_EQ(rotated.footprint_cells.size(), static_cast<std::size_t>(9));
	const BuildingDefinition* greenhouse = simulation.findDefinition(BuildingType::Greenhouse);
	for (std::size_t i = 0; i < greenhouse->access_offsets.size(); ++i)
	{
		const Cell offset = footprint::rotateOffset(greenhouse->access_offsets[i], 3, 3, 1);
		CHECK(rotated.access_cells[i] == (Cell{ 30 + offset.x, 35 + offset.z }));
	}
}

TEST_CASE("production: bad initial buildings and inventory report their JSON pointer")
{
	std::string error;
	Json scenario = devScenario();
	scenario["initialBuildings"][0]["type"] = "silo";
	ColonySimulation a;
	CHECK(!start(a, scenario, makeDefinitions(), &error));
	CHECK(error.rfind("/initialBuildings/0/type", 0) == 0);
	CHECK(!a.isInitialized());

	scenario = devScenario();
	scenario["initialBuildings"].push_back(building("storage", 31, 27));
	ColonySimulation b;
	CHECK(!start(b, scenario, makeDefinitions(), &error));
	CHECK(error.rfind("/initialBuildings/4:", 0) == 0);
	CHECK(b.getBuildings().empty()); // nothing is left half-initialised

	scenario = devScenario();
	scenario["initialBuildings"] = Json::array();      // back to the 100-unit cargo hold
	scenario["initialInventory"] = Json{ { "steel", 101 } };
	ColonySimulation c;
	CHECK(!start(c, scenario, makeDefinitions(), &error));
	CHECK(error.rfind("/initialInventory", 0) == 0);

	scenario = devScenario();
	scenario["initialBuildings"][0] = Json{ { "type", "iron_mine" }, { "socket", "west_door" } };
	ColonySimulation d;
	CHECK(!start(d, scenario, makeDefinitions(), &error));
	CHECK(error.find("footprint building") != std::string::npos);
}

TEST_CASE("production: names come from the pool in ID order and must be enough")
{
	Json scenario = devScenario();
	scenario["initialColonists"] = Json::array({ colonist(9, "miner", 17, 30), colonist(3, "farmer", 18, 30) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	CHECK_EQ(simulation.getColonists().front().id, 3u); // stored in ID order
	CHECK_EQ(simulation.findColonist(3)->name, std::string("Ada Vance"));
	CHECK_EQ(simulation.findColonist(9)->name, std::string("Bram Okafor"));

	const std::filesystem::path path = std::filesystem::temp_directory_path() / "arkfall_p05_names.json";
	{
		std::ofstream out(path);
		out << R"({ "schemaVersion": 1, "roles": [
			{ "job": "engineer", "assetId": "astronaut", "materialId": "crew", "moveSpeed": 1.8 },
			{ "job": "miner", "assetId": "astronaut", "materialId": "crew", "moveSpeed": 1.8 },
			{ "job": "farmer", "assetId": "astronaut", "materialId": "crew", "moveSpeed": 1.8 } ],
			"names": [ "Solo" ] })";
	}
	GameDefinitions definitions = makeDefinitions();
	CHECK(definitions.colonists.load(path));
	std::error_code ec;
	std::filesystem::remove(path, ec);
	std::string error;
	ColonySimulation few;
	CHECK(!start(few, scenario, definitions, &error));
	CHECK(error.find("name pool") != std::string::npos);
}

TEST_CASE("production: AssignJob returns structured results")
{
	ColonySimulation simulation;
	if (!start(simulation, devScenario(), makeDefinitions()))
		return;
	CommandResult result = simulation.apply(GameCommand::assignJob(42, Job::Miner));
	CHECK(!result.accepted);
	CHECK_EQ(result.code, std::string("unknown_colonist"));
	result = simulation.apply(GameCommand::assignJob(1, Job::Miner));
	CHECK(!result.accepted);
	CHECK_EQ(result.code, std::string("same_job"));
	result = simulation.apply(GameCommand::assignJob(8, Job::Miner));
	CHECK(result.accepted);
	CHECK_EQ(result.code, std::string("ok"));
	CHECK(result.detail.find("Hugo Hale is now a Miner") != std::string::npos);
	CHECK(simulation.findColonist(8)->job == Job::Miner);
}

TEST_CASE("production: same inputs give the same economy (deterministic replay)")
{
	ColonySimulation a, b;
	if (!start(a, devScenario(), makeDefinitions()) || !start(b, devScenario(), makeDefinitions()))
		return;
	for (int i = 0; i < 30 * 120; ++i)
	{
		if (i == 30 * 40)
		{
			a.apply(GameCommand::assignJob(3, Job::Farmer));
			b.apply(GameCommand::assignJob(3, Job::Farmer));
		}
		a.tick(dt);
		b.tick(dt);
	}
	CHECK(a.getInventory().getAmounts() == b.getInventory().getAmounts());
	for (std::size_t i = 0; i < a.getColonists().size(); ++i)
	{
		CHECK(a.getColonists()[i].last_cell == b.getColonists()[i].last_cell);
		CHECK(a.getColonists()[i].state == b.getColonists()[i].state);
	}
}
