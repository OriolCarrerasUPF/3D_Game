// P06 survival and utilities (headless): need decay and death, services (eat/drink at
// Storage entrances, suit refill in a powered airlock chamber), all-or-nothing power,
// water allocation order, indoor oxygen coverage, mine oxygen, Farmer production, the
// idle rule and deterministic replay. Scenarios are variations of
// data/arkfall/dev/survival.json: an airlock, a corridor with Storage, Oxygenator and
// Greenhouse domes, and an Iron Mine, Water Extractor and Solar Panel outside.

#include "test_harness.h"

#include "game/colony_simulation.h"

#include <algorithm>

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

	// Shipped buildings.json with one numeric field of one building changed.
	GameDefinitions definitionsWith(const char* id, const char* group, const char* key, double value)
	{
		Json buildings = loadJson("data/arkfall/buildings.json");
		for (Json& def : buildings["buildings"])
			if (def["id"] == id)
				def[group][key] = value;
		return makeDefinitions(&buildings);
	}

	Json survivalScenario() { return loadJson("data/arkfall/dev/survival.json"); }

	Json colonist(int id, const char* job, int x, int z, double hunger = 100, double thirst = 100, double oxygen = 100)
	{
		return Json{ { "id", id }, { "job", job }, { "cell", Json::array({ x, z }) },
			{ "needs", { { "hunger", hunger }, { "thirst", thirst }, { "oxygen", oxygen } } } };
	}

	// Removes every initial building of the given type.
	void removeBuildings(Json& scenario, const char* type)
	{
		Json kept = Json::array();
		for (const Json& building : scenario["initialBuildings"])
			if (building["type"] != type)
				kept.push_back(building);
		scenario["initialBuildings"] = kept;
	}

	bool start(ColonySimulation& simulation, const Json& scenario, const GameDefinitions& definitions)
	{
		const ScenarioLoadResult loaded = ScenarioLoader().parse(scenario, "survival.json");
		if (!loaded)
		{
			test::fail(__FILE__, __LINE__, loaded.error->describe());
			return false;
		}
		std::string error;
		if (!simulation.initialize(loaded.scenario, definitions, error))
		{
			test::fail(__FILE__, __LINE__, error);
			return false;
		}
		return true;
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

	bool contains(const std::vector<ColonistId>& ids, ColonistId id)
	{
		return std::find(ids.begin(), ids.end(), id) != ids.end();
	}

	bool contains(const std::vector<BuildingId>& ids, const BuildingRecord* building)
	{
		return building && std::find(ids.begin(), ids.end(), building->id) != ids.end();
	}
}

TEST_CASE("survival: shipped need rules are the C8 values")
{
	const GameDefinitions definitions = makeDefinitions();
	const NeedRules& rules = definitions.colonists.getNeedRules();
	CHECK_NEAR(rules.refill_threshold, 20.0, 1e-6);
	CHECK_NEAR(rules.hunger_drain, 0.10, 1e-6);
	CHECK_NEAR(rules.thirst_drain, 0.12, 1e-6);
	CHECK_NEAR(rules.oxygen_drain, 0.10, 1e-6);
	CHECK_NEAR(rules.drink_per_second * rules.water_per_thirst_point, 0.2, 1e-6); // 0.2 water/s per drinker
	CHECK_NEAR(rules.eat_seconds, 3.0, 1e-6);
	CHECK_EQ(rules.vegetables_per_meal, 1);
}

TEST_CASE("survival: needs decay at the C8 rates outside")
{
	Json scenario = survivalScenario();
	scenario["initialBuildings"] = Json::array();
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 17, 30) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	run(simulation, 10.0f);
	const ColonistRecord* c = simulation.findColonist(1);
	CHECK_NEAR(c->needs.hunger, 99.0, 1e-3);
	CHECK_NEAR(c->needs.thirst, 98.8, 1e-3);
	CHECK_NEAR(c->needs.oxygen, 99.0, 1e-3);   // no base: the suit drains
	CHECK(c->state == ColonistState::Idle);    // nowhere inside to wait: it stays put
	CHECK(c->last_cell == (Cell{ 17, 30 }));
}

TEST_CASE("survival: a need at zero kills, releases the slot and leaves the population")
{
	Json scenario = survivalScenario();
	removeBuildings(scenario, "storage");   // no Storage: the thirsty Miner has nowhere to drink
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 7, 12, 100, 3, 100) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	const ColonistRecord* miner = simulation.findColonist(1);
	simulation.tick(dt);
	// Urgent but no service: it keeps working (and says why), the need keeps draining.
	CHECK(miner->state == ColonistState::Working);
	CHECK(miner->need_alert.find("needs water") != std::string::npos);
	CHECK_EQ(simulation.countLiving(), 1);
	run(simulation, 30.0f);
	CHECK(miner->state == ColonistState::Dead);
	CHECK_EQ(miner->death_cause, std::string("thirst"));
	CHECK(miner->status.find("dead") != std::string::npos);
	CHECK(!miner->reservation.active());
	CHECK_EQ(simulation.countReservedWorkers(*mine), 0);
	CHECK_EQ(simulation.countLiving(), 0);
	// While it worked at the powered mine its suit never drained (only the first tick, before arriving).
	CHECK_NEAR(miner->needs.oxygen, 100.0, 0.01);
	// The dead never come back to work.
	run(simulation, 5.0f);
	CHECK(miner->state == ColonistState::Dead);
}

TEST_CASE("survival: no power means no mine production")
{
	Json scenario = survivalScenario();
	removeBuildings(scenario, "solar_panel");
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 7, 12) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	run(simulation, 20.0f);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	CHECK(!simulation.getUtilities().powered);
	CHECK(mine->status == BuildingStatus::NoPower);
	CHECK_EQ(mine->cycles_completed, 0);
	CHECK_EQ(simulation.getInventory().get(ResourceType::IronOre), 0);
	CHECK(!simulation.findColonist(1)->reservation.active());
	CHECK(simulation.findColonist(1)->status.find("no power") != std::string::npos);
}

TEST_CASE("survival: all-or-nothing power at the exact boundary")
{
	// Demand of dev/survival.json: door 1 + Oxygenator 2 + Greenhouse 2 + mine 4 + extractor 2 = 11.
	{
		ColonySimulation simulation;
		if (!start(simulation, survivalScenario(), definitionsWith("solar_panel", "power", "output", 11.0)))
			return;
		simulation.tick(dt);
		const UtilitySnapshot& u = simulation.getUtilities();
		CHECK_NEAR(u.power_demand, 11.0, 1e-4);
		CHECK(u.powered);
		CHECK(u.stopped.empty());
		for (const BuildingRecord& building : simulation.getBuildings())
			CHECK(building.powered);
	}
	{
		ColonySimulation simulation;
		if (!start(simulation, survivalScenario(), definitionsWith("solar_panel", "power", "output", 10.9)))
			return;
		simulation.tick(dt);
		const UtilitySnapshot& u = simulation.getUtilities();
		CHECK(!u.powered);
		CHECK_NEAR(u.water_produced, 0.0, 1e-6);   // the extractor is a consumer too
		CHECK_EQ(u.oxygen_capacity, 0);
		CHECK(contains(u.stopped, firstOfType(simulation, BuildingType::IronMine)));
		CHECK(contains(u.stopped, firstOfType(simulation, BuildingType::ExitDoor)));
		CHECK(firstOfType(simulation, BuildingType::SolarPanel)->powered);   // generators never stop
		CHECK(firstOfType(simulation, BuildingType::Storage)->powered);      // no demand
		// An unpowered door stays passable: only the suit refill stops.
		const BuildingRecord* door = firstOfType(simulation, BuildingType::ExitDoor);
		CHECK(simulation.getGrid().isDoorOpen(*door->door_index));
		CHECK(door->status_text.find("no suit refill") != std::string::npos);
	}
}

TEST_CASE("survival: water goes to Oxygenators before Greenhouses, full flow only")
{
	{
		ColonySimulation simulation;
		if (!start(simulation, survivalScenario(), definitionsWith("water_extractor", "water", "output", 0.06)))
			return;
		simulation.tick(dt);
		const BuildingRecord* oxygenator = firstOfType(simulation, BuildingType::Oxygenator);
		const BuildingRecord* greenhouse = firstOfType(simulation, BuildingType::Greenhouse);
		CHECK(oxygenator->water_supplied && oxygenator->operational);
		CHECK(!greenhouse->water_supplied && !greenhouse->operational); // 0.04 left < 0.05
		CHECK(contains(simulation.getUtilities().stopped, greenhouse));
		CHECK_NEAR(simulation.getUtilities().water_for_buildings, 0.02, 1e-5);
	}
	{
		ColonySimulation simulation;
		if (!start(simulation, survivalScenario(), definitionsWith("water_extractor", "water", "output", 0.07)))
			return;
		simulation.tick(dt);
		CHECK(firstOfType(simulation, BuildingType::Oxygenator)->operational);
		CHECK(firstOfType(simulation, BuildingType::Greenhouse)->operational);
	}
}

TEST_CASE("survival: drinkers get water first, lowest Thirst first")
{
	Json scenario = survivalScenario();
	// Colonist 2 is thirstier; both stand on Storage entrances in the corridor.
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 26, 31, 100, 15, 100),
		colonist(2, "engineer", 27, 31, 100, 10, 100) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, definitionsWith("water_extractor", "water", "output", 0.21)))
		return;
	const BuildingRecord* storage = firstOfType(simulation, BuildingType::Storage);
	const BuildingRecord* oxygenator = firstOfType(simulation, BuildingType::Oxygenator);
	simulation.tick(dt);   // both arrive at once: already on the entrance
	const ColonistRecord* a = simulation.findColonist(1);
	const ColonistRecord* b = simulation.findColonist(2);
	CHECK(a->state == ColonistState::Refilling);
	CHECK(b->state == ColonistState::Refilling);
	// Drinking colonists are inside the Storage dome, hidden and listed.
	CHECK_EQ(a->current_dome, storage->id);
	CHECK(contains(simulation.occupants(storage->id), 1u));
	CHECK(contains(simulation.occupants(storage->id), 2u));
	simulation.tick(dt);
	const UtilitySnapshot& u = simulation.getUtilities();
	CHECK_EQ(u.drinkers_served, 1);
	CHECK_EQ(u.drinkers_waiting, 1);
	CHECK(b->water_allocated);
	CHECK(!a->water_allocated);
	CHECK(a->status.find("waiting for water") != std::string::npos);
	// 0.01 left: the Oxygenator (0.02) stops while someone drinks.
	CHECK(!oxygenator->water_supplied);
	// Once full, Thirst drains again at 0.12/s, so allow a little below 100.
	run(simulation, 10.0f);
	CHECK(b->needs.thirst > 98.0f);
	CHECK(b->state != ColonistState::Refilling);
	run(simulation, 10.0f);
	CHECK(a->needs.thirst > 97.0f);
	CHECK(a->state != ColonistState::Refilling);
	run(simulation, 1.0f);
	CHECK(oxygenator->water_supplied);   // nobody drinks any more
}

TEST_CASE("survival: indoor oxygen covers 25 colonists, not 26")
{
	for (int count : { 25, 26 })
	{
		Json scenario = survivalScenario();
		scenario["initialBuildings"][2]["to"] = Json::array({ 44, 31 });   // a longer corridor
		Json colonists = Json::array();
		for (int i = 0; i < count; ++i)
			colonists.push_back(colonist(i + 1, "engineer", 25 + i / 2, 30 + (i % 2)));
		scenario["initialColonists"] = colonists;
		ColonySimulation simulation;
		if (!start(simulation, scenario, makeDefinitions()))
			return;
		run(simulation, 10.0f);
		const UtilitySnapshot& u = simulation.getUtilities();
		CHECK_EQ(u.indoor_population, count);
		CHECK_EQ(u.oxygen_capacity, 25);
		CHECK_EQ(u.oxygen_covered, count <= 25);
		// Everyone indoors drains when the capacity is short, in domes and corridors alike.
		for (const ColonistRecord& c : simulation.getColonists())
			CHECK_NEAR(c.needs.oxygen, count <= 25 ? 100.0 : 99.0, 0.05);
	}
}

TEST_CASE("survival: powered working Miners keep mine oxygen")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 17, 30) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const ColonistRecord* miner = simulation.findColonist(1);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	for (int i = 0; i < 30 * 60 && miner->state != ColonistState::Working; ++i)
	{
		simulation.tick(dt);
		if (miner->state != ColonistState::Working)
			CHECK_EQ(miner->current_dome, INVALID_BUILDING);   // visible while walking
	}
	CHECK(miner->state == ColonistState::Working);
	// Working at an exterior building hides the Miner inside it, listed in its popup.
	CHECK_EQ(miner->current_dome, mine->id);
	CHECK(contains(simulation.occupants(mine->id), 1u));
	CHECK(miner->needs.oxygen < 100.0f);   // the walk out drained the suit
	const float oxygen = miner->needs.oxygen;
	run(simulation, 30.0f);
	CHECK_NEAR(miner->needs.oxygen, oxygen, 1e-4);
	CHECK_NEAR(simulation.currentDrain(*miner, NeedType::Oxygen), 0.0, 1e-6);
	// The mine's local supply never becomes base capacity.
	CHECK_EQ(simulation.getUtilities().oxygen_capacity, 25);
}

TEST_CASE("survival: a thirsty Miner leaves work, drinks hidden in the Storage and returns")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 7, 12, 100, 24, 100) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const BuildingRecord* storage = firstOfType(simulation, BuildingType::Storage);
	const BuildingRecord* mine = firstOfType(simulation, BuildingType::IronMine);
	const ColonistRecord* miner = simulation.findColonist(1);
	bool worked = false, interrupted = false, hidden_drinking = false, back_to_work = false;
	for (int i = 0; i < 30 * 200 && !back_to_work; ++i)
	{
		simulation.tick(dt);
		if (miner->state == ColonistState::Working && !interrupted)
			worked = true;
		if (worked && miner->state == ColonistState::MovingToService)
		{
			interrupted = true;
			CHECK(!miner->reservation.active());          // released before the service
			CHECK_EQ(miner->current_dome, INVALID_BUILDING); // out of the mine, visible again
			CHECK_EQ(simulation.countReservedWorkers(*mine), 0);
			CHECK(miner->needs.thirst <= 20.0f + 1e-3f);
		}
		if (miner->state == ColonistState::Refilling && miner->current_dome == storage->id &&
			contains(simulation.occupants(storage->id), 1u))
			hidden_drinking = true;
		if (hidden_drinking && miner->state == ColonistState::Working)
			back_to_work = true;
	}
	CHECK(worked);
	CHECK(interrupted);
	CHECK(hidden_drinking);
	CHECK(back_to_work);
	CHECK(miner->needs.thirst > 90.0f);
	CHECK(miner->state != ColonistState::Dead);
}

TEST_CASE("survival: eating takes one vegetable and the eating time")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 26, 31, 15, 100, 100) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const ColonistRecord* c = simulation.findColonist(1);
	simulation.tick(dt);
	CHECK(c->state == ColonistState::Refilling);
	CHECK(c->service_need == NeedType::Hunger);
	CHECK_EQ(simulation.getInventory().get(ResourceType::Vegetables), 4);   // taken when eating starts
	run(simulation, 2.5f);
	CHECK(c->state == ColonistState::Refilling);
	CHECK(c->needs.hunger < 20.0f);
	run(simulation, 0.6f);
	CHECK_NEAR(c->needs.hunger, 100.0, 0.1);   // full, then drains again
	CHECK(c->state != ColonistState::Refilling);
	CHECK_EQ(simulation.getInventory().get(ResourceType::Vegetables), 4);
}

TEST_CASE("survival: unavailable services never refill needs")
{
	Json scenario = survivalScenario();
	removeBuildings(scenario, "water_extractor");
	scenario["initialInventory"] = Json{ { "steel", 20 } };   // no vegetables
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 26, 31, 15, 15, 100) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	run(simulation, 10.0f);
	const ColonistRecord* c = simulation.findColonist(1);
	CHECK(!c->in_service);
	CHECK(c->state != ColonistState::Refilling);
	CHECK(c->need_alert.find("no water") != std::string::npos);
	CHECK(c->need_alert.find("no vegetables") != std::string::npos);
	CHECK_NEAR(c->needs.thirst, 15.0 - 1.2, 1e-3);
	CHECK_NEAR(c->needs.hunger, 15.0 - 1.0, 1e-3);
}

TEST_CASE("survival: the suit is refilled in a powered airlock chamber with base oxygen")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "miner", 17, 30, 100, 100, 15) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	const BuildingRecord* door = firstOfType(simulation, BuildingType::ExitDoor);
	const ColonistRecord* c = simulation.findColonist(1);
	simulation.tick(dt);
	CHECK(c->in_service && c->service_need == NeedType::Oxygen);
	CHECK_EQ(c->service_building, door->id);
	bool refilled_in_chamber = false;
	for (int i = 0; i < 30 * 30 && c->in_service; ++i)
	{
		simulation.tick(dt);
		if (c->state == ColonistState::Refilling)
		{
			refilled_in_chamber = std::find(door->inside_cells.begin(), door->inside_cells.end(), c->last_cell) != door->inside_cells.end();
			CHECK_EQ(c->current_dome, INVALID_BUILDING);   // the airlock is not a dome: visible
		}
	}
	CHECK(refilled_in_chamber);
	CHECK_NEAR(c->needs.oxygen, 100.0, 1e-3);
	CHECK(door->status_text.find("refills suits") != std::string::npos);

	// Without a working Oxygenator the base has no oxygen and the door refills nobody.
	Json no_air = survivalScenario();
	removeBuildings(no_air, "oxygenator");
	no_air["initialColonists"] = Json::array({ colonist(1, "miner", 17, 30, 100, 100, 15) });
	ColonySimulation dry;
	if (!start(dry, no_air, makeDefinitions()))
		return;
	run(dry, 5.0f);
	const ColonistRecord* d = dry.findColonist(1);
	CHECK(!d->in_service);
	CHECK(d->need_alert.find("no working Oxygenator") != std::string::npos);
	CHECK(d->needs.oxygen < 15.0f);
	CHECK(!d->reservation.active());   // the suit cannot last an outdoor job
}

TEST_CASE("survival: several urgent needs - shortest time to zero, then Oxygen, Thirst, Hunger")
{
	// Outside: Oxygen 10 empties in 100 s, Thirst 15 in 125 s: Oxygen first.
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 17, 30, 100, 15, 10) });
	ColonySimulation outside;
	if (!start(outside, scenario, makeDefinitions()))
		return;
	outside.tick(dt);
	CHECK(outside.findColonist(1)->service_need == NeedType::Oxygen);

	// Inside a covered base the suit does not drain, so Thirst comes first.
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 26, 31, 100, 15, 15) });
	ColonySimulation inside;
	if (!start(inside, scenario, makeDefinitions()))
		return;
	inside.tick(dt);
	CHECK(inside.findColonist(1)->service_need == NeedType::Thirst);
	// After drinking, the Oxygen need is served next (refill in the chamber).
	for (int i = 0; i < 30 * 30 && inside.findColonist(1)->service_need != NeedType::Oxygen; ++i)
		inside.tick(dt);
	CHECK(inside.findColonist(1)->service_need == NeedType::Oxygen);
}

TEST_CASE("survival: no Farmer means no vegetables; a Farmer grows them")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 18, 31) });
	ColonySimulation none;
	if (!start(none, scenario, makeDefinitions()))
		return;
	run(none, 60.0f);
	CHECK_EQ(firstOfType(none, BuildingType::Greenhouse)->cycles_completed, 0);
	CHECK(firstOfType(none, BuildingType::Greenhouse)->status == BuildingStatus::NoWorkers);
	CHECK_EQ(none.getInventory().get(ResourceType::Vegetables), 5);

	scenario["initialColonists"] = Json::array({ colonist(1, "farmer", 18, 31) });
	ColonySimulation farm;
	if (!start(farm, scenario, makeDefinitions()))
		return;
	run(farm, 60.0f);
	const BuildingRecord* greenhouse = firstOfType(farm, BuildingType::Greenhouse);
	CHECK(greenhouse->cycles_completed >= 3);
	CHECK_EQ(farm.getInventory().get(ResourceType::Vegetables), 5 + greenhouse->cycles_completed);
	CHECK_EQ(farm.findColonist(1)->current_dome, greenhouse->id);   // works hidden in the dome
}

TEST_CASE("survival: idle colonists wait inside a dome, hidden and listed")
{
	Json scenario = survivalScenario();
	scenario["initialColonists"] = Json::array({ colonist(1, "engineer", 18, 31) });
	ColonySimulation simulation;
	if (!start(simulation, scenario, makeDefinitions()))
		return;
	run(simulation, 40.0f);
	const ColonistRecord* c = simulation.findColonist(1);
	CHECK(c->state == ColonistState::Idle);
	CHECK(c->current_dome != INVALID_BUILDING);
	const BuildingRecord* dome = simulation.findBuilding(c->current_dome);
	CHECK(dome && simulation.findDefinition(dome->type)->isDome());
	CHECK(dome && contains(simulation.occupants(dome->id), 1u));
	CHECK(c->status.find("waiting in") != std::string::npos);
	// Inside with oxygen coverage the suit no longer drains.
	const float oxygen = c->needs.oxygen;
	run(simulation, 10.0f);
	CHECK_NEAR(c->needs.oxygen, oxygen, 1e-4);
}

TEST_CASE("survival: same inputs give the same colony (deterministic replay)")
{
	ColonySimulation a, b;
	if (!start(a, survivalScenario(), makeDefinitions()) || !start(b, survivalScenario(), makeDefinitions()))
		return;
	for (int tick = 0; tick < 30 * 120; ++tick)
	{
		a.tick(dt);
		b.tick(dt);
	}
	for (std::size_t i = 0; i < a.getColonists().size(); ++i)
	{
		const ColonistRecord& ca = a.getColonists()[i];
		const ColonistRecord& cb = b.getColonists()[i];
		CHECK(ca.last_cell == cb.last_cell);
		CHECK(ca.state == cb.state);
		CHECK_NEAR(ca.needs.hunger, cb.needs.hunger, 0.0);
		CHECK_NEAR(ca.needs.thirst, cb.needs.thirst, 0.0);
		CHECK_NEAR(ca.needs.oxygen, cb.needs.oxygen, 0.0);
	}
	CHECK(a.getInventory().getAmounts() == b.getInventory().getAmounts());
	// Nobody in the dev scenario dies in the first two minutes: every need gets served.
	CHECK_EQ(a.countLiving(), 4);
}
