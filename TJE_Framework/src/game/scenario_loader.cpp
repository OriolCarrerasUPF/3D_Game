#include "scenario_loader.h"

#include "building_catalog.h"
#include "scene_parser.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace
{
	using json_data::Json;
	using json_data::childPointer;
	using json_data::makeError;

	std::string describeCell(Cell cell)
	{
		return "(" + std::to_string(cell.x) + ", " + std::to_string(cell.z) + ")";
	}

	// Helper that keeps the file path and stops at the first error.
	class Reader
	{
	public:
		Reader(const std::filesystem::path& file, DataLoadError& error) : file(file), error(error) {}

		bool keys(const Json& value, const std::set<std::string>& required, const std::string& pointer,
			const std::set<std::string>& optional = {})
		{
			return json_data::validateKeys(value, required, optional, file, pointer, error);
		}
		bool cell(const Json& value, Cell& out, const GridDefinition& grid, const std::string& pointer)
		{
			return json_data::readCell(value, out, grid.width, grid.height, file, pointer, error);
		}
		bool fail(const std::string& pointer, const std::string& message)
		{
			error = makeError(file, pointer, message);
			return false;
		}

		const std::filesystem::path& file;
		DataLoadError& error;
	};

	bool parseGrid(Reader& r, const Json& value, GridDefinition& grid)
	{
		Vector2 origin;
		if (!r.keys(value, {"width", "height", "cellSize", "originXZ"}, "/grid") ||
			!json_data::readInt(value["width"], grid.width, 3, ScenarioLoader::max_grid_size, r.file, "/grid/width", r.error) ||
			!json_data::readInt(value["height"], grid.height, 3, ScenarioLoader::max_grid_size, r.file, "/grid/height", r.error) ||
			!json_data::readFloatInRange(value["cellSize"], 0.01f, 100.0f, grid.cell_size, r.file, "/grid/cellSize", r.error) ||
			!json_data::readVector2(value["originXZ"], origin, r.file, "/grid/originXZ", r.error))
			return false;
		grid.origin_x = origin.x;
		grid.origin_z = origin.y;
		return true;
	}

	bool parseDeposits(Reader& r, const Json& value, ScenarioDefinition& scenario, std::set<std::string>& ids)
	{
		static const std::map<std::string, DepositType> resource_names{ { "iron", DepositType::Iron }, { "copper", DepositType::Copper } };
		if (!json_data::requireArray(value, 0, r.file, "/deposits", r.error))
			return false;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const std::string pointer = childPointer("/deposits", i);
			DepositDefinition deposit;
			if (!r.keys(value[i], {"id", "resource", "cell"}, pointer) ||
				!json_data::readString(value[i]["id"], deposit.id, r.file, childPointer(pointer, "id"), r.error) ||
				!json_data::readEnum(value[i]["resource"], resource_names, deposit.type, r.file, childPointer(pointer, "resource"), r.error) ||
				!r.cell(value[i]["cell"], deposit.cell, scenario.grid, childPointer(pointer, "cell")))
				return false;
			if (!ids.insert(deposit.id).second)
				return r.fail(childPointer(pointer, "id"), "duplicate id '" + deposit.id + "'");
			scenario.deposits.push_back(deposit);
		}
		return true;
	}

	bool parseBlockers(Reader& r, const Json& value, ScenarioDefinition& scenario, std::set<std::string>& ids)
	{
		if (!json_data::requireArray(value, 0, r.file, "/staticBlockers", r.error))
			return false;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const std::string pointer = childPointer("/staticBlockers", i);
			StaticBlockerDefinition blocker;
			if (!r.keys(value[i], {"id", "min", "max"}, pointer) ||
				!json_data::readString(value[i]["id"], blocker.id, r.file, childPointer(pointer, "id"), r.error) ||
				!r.cell(value[i]["min"], blocker.min, scenario.grid, childPointer(pointer, "min")) ||
				!r.cell(value[i]["max"], blocker.max, scenario.grid, childPointer(pointer, "max")))
				return false;
			if (!ids.insert(blocker.id).second)
				return r.fail(childPointer(pointer, "id"), "duplicate id '" + blocker.id + "'");
			if (blocker.min.x > blocker.max.x || blocker.min.z > blocker.max.z)
				return r.fail(pointer, "min must not exceed max");
			scenario.static_blockers.push_back(blocker);
		}
		return true;
	}

	bool readNeeds(Reader& r, const Json& value, NeedValues& needs, const std::string& pointer)
	{
		return r.keys(value, {"hunger", "thirst", "oxygen"}, pointer) &&
			json_data::readFloatInRange(value["hunger"], 0.0f, 100.0f, needs.hunger, r.file, childPointer(pointer, "hunger"), r.error) &&
			json_data::readFloatInRange(value["thirst"], 0.0f, 100.0f, needs.thirst, r.file, childPointer(pointer, "thirst"), r.error) &&
			json_data::readFloatInRange(value["oxygen"], 0.0f, 100.0f, needs.oxygen, r.file, childPointer(pointer, "oxygen"), r.error);
	}

	bool parseColonists(Reader& r, const Json& value, ScenarioDefinition& scenario)
	{
		static const std::map<std::string, Job> job_names{ { "engineer", Job::Engineer }, { "miner", Job::Miner }, { "farmer", Job::Farmer } };
		if (!json_data::requireArray(value, 1, r.file, "/initialColonists", r.error))
			return false;
		std::set<ColonistId> ids;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const std::string pointer = childPointer("/initialColonists", i);
			InitialColonistDefinition colonist;
			int id = 0;
			if (!r.keys(value[i], {"id", "job", "cell", "needs"}, pointer) ||
				!json_data::readInt(value[i]["id"], id, 1, 1000000, r.file, childPointer(pointer, "id"), r.error) ||
				!json_data::readEnum(value[i]["job"], job_names, colonist.job, r.file, childPointer(pointer, "job"), r.error) ||
				!r.cell(value[i]["cell"], colonist.cell, scenario.grid, childPointer(pointer, "cell")) ||
				!readNeeds(r, value[i]["needs"], colonist.needs, childPointer(pointer, "needs")))
				return false;
			colonist.id = static_cast<ColonistId>(id);
			if (!ids.insert(colonist.id).second)
				return r.fail(childPointer(pointer, "id"), "duplicate colonist id " + std::to_string(id));
			scenario.initial_colonists.push_back(colonist);
		}
		return true;
	}

	bool parseStartState(Reader& r, const Json& document, ScenarioDefinition& scenario)
	{
		if (!BuildingCatalog::readResourceAmounts(document["initialInventory"], ScenarioLoader::max_item_amount,
				scenario.initial_inventory, r.file, "/initialInventory", r.error) ||
			!json_data::readInt(document["cargoCapacity"], scenario.cargo_capacity, 0, ScenarioLoader::max_item_amount,
				r.file, "/cargoCapacity", r.error) ||
			!json_data::requireArray(document["initialBuildings"], 0, r.file, "/initialBuildings", r.error))
			return false;

		for (std::size_t i = 0; i < document["initialBuildings"].size(); ++i)
		{
			const std::string pointer = childPointer("/initialBuildings", i);
			const Json& item = document["initialBuildings"][i];
			InitialBuildingDefinition building;
			if (!r.keys(item, {"type"}, pointer, {"origin", "quarterTurns", "from", "to"}) ||
				!json_data::readString(item["type"], building.type_id, r.file, childPointer(pointer, "type"), r.error))
				return false;
			const bool has_origin = item.contains("origin") || item.contains("quarterTurns");
			building.is_strip = item.contains("from") || item.contains("to");
			if (has_origin == building.is_strip)
				return r.fail(pointer, "give either origin + quarterTurns, or from + to (corridor strip)");
			if (building.is_strip)
			{
				if (building.type_id != "corridor")
					return r.fail(childPointer(pointer, "type"), "only a corridor is placed as a from/to strip");
				if (!item.contains("from"))
					return r.fail(childPointer(pointer, "from"), "required field is missing");
				if (!item.contains("to"))
					return r.fail(childPointer(pointer, "to"), "required field is missing");
				if (!r.cell(item["from"], building.strip_from, scenario.grid, childPointer(pointer, "from")) ||
					!r.cell(item["to"], building.strip_to, scenario.grid, childPointer(pointer, "to")))
					return false;
				const int span_x = std::abs(building.strip_to.x - building.strip_from.x) + 1;
				const int span_z = std::abs(building.strip_to.z - building.strip_from.z) + 1;
				if (span_x != 2 && span_z != 2)
					return r.fail(pointer, "a corridor strip is straight and exactly 2 cells wide");
			}
			else
			{
				if (!item.contains("origin"))
					return r.fail(childPointer(pointer, "origin"), "required field is missing");
				if (!item.contains("quarterTurns"))
					return r.fail(childPointer(pointer, "quarterTurns"), "required field is missing");
				if (!r.cell(item["origin"], building.origin, scenario.grid, childPointer(pointer, "origin")) ||
					!json_data::readInt(item["quarterTurns"], building.quarter_turns, 0, 3, r.file, childPointer(pointer, "quarterTurns"), r.error))
					return false;
			}
			scenario.initial_buildings.push_back(building);
		}
		return true;
	}

	bool parseDebugRoute(Reader& r, const Json& value, ScenarioDefinition& scenario)
	{
		DebugRouteDefinition route;
		int colonist_id = 0;
		if (!r.keys(value, {"colonistId", "loop", "dwellSeconds", "stops"}, "/debugRoute") ||
			!json_data::readInt(value["colonistId"], colonist_id, 1, 1000000, r.file, "/debugRoute/colonistId", r.error) ||
			!json_data::readBool(value["loop"], route.loop, r.file, "/debugRoute/loop", r.error) ||
			!json_data::readFloatInRange(value["dwellSeconds"], 0.0f, 60.0f, route.dwell_seconds, r.file, "/debugRoute/dwellSeconds", r.error) ||
			!json_data::requireArray(value["stops"], 1, r.file, "/debugRoute/stops", r.error))
			return false;
		route.colonist_id = static_cast<ColonistId>(colonist_id);
		for (std::size_t i = 0; i < value["stops"].size(); ++i)
		{
			const std::string pointer = childPointer("/debugRoute/stops", i);
			RouteStopDefinition stop;
			if (!r.keys(value["stops"][i], {"label", "cell"}, pointer) ||
				!json_data::readString(value["stops"][i]["label"], stop.label, r.file, childPointer(pointer, "label"), r.error) ||
				!r.cell(value["stops"][i]["cell"], stop.cell, scenario.grid, childPointer(pointer, "cell")))
				return false;
			route.stops.push_back(stop);
		}
		scenario.debug_route = route;
		return true;
	}

	// Cross-field rules that need the whole scenario (overlaps, reachability).
	bool validateLayout(Reader& r, const ScenarioDefinition& scenario)
	{
		std::map<std::pair<int, int>, std::string> occupied;
		for (std::size_t i = 0; i < scenario.deposits.size(); ++i)
		{
			const DepositDefinition& deposit = scenario.deposits[i];
			occupied[{ deposit.cell.x, deposit.cell.z }] = deposit.id;
		}
		for (std::size_t i = 0; i < scenario.static_blockers.size(); ++i)
		{
			const StaticBlockerDefinition& blocker = scenario.static_blockers[i];
			for (int z = blocker.min.z; z <= blocker.max.z; ++z)
				for (int x = blocker.min.x; x <= blocker.max.x; ++x)
				{
					const auto inserted = occupied.emplace(std::make_pair(x, z), blocker.id);
					if (!inserted.second)
						return r.fail(childPointer("/staticBlockers", i), "overlaps '" + inserted.first->second + "' at " + describeCell(Cell{ x, z }));
				}
		}
		for (std::size_t i = 0; i < scenario.initial_colonists.size(); ++i)
		{
			const Cell cell = scenario.initial_colonists[i].cell;
			if (occupied.count({ cell.x, cell.z }))
				return r.fail(childPointer(childPointer("/initialColonists", i), "cell"), "colonist starts on a blocked cell " + describeCell(cell));
		}

		if (!scenario.debug_route)
			return true;
		const DebugRouteDefinition& route = *scenario.debug_route;
		const InitialColonistDefinition* colonist = nullptr;
		for (const InitialColonistDefinition& candidate : scenario.initial_colonists)
			if (candidate.id == route.colonist_id)
				colonist = &candidate;
		if (!colonist)
			return r.fail("/debugRoute/colonistId", "unknown colonist id " + std::to_string(route.colonist_id));

		// Stops must be free cells. Whether every leg is walkable depends on the doors and
		// corridors of initialBuildings, so ColonySimulation::initialize checks the routes.
		for (std::size_t i = 0; i < route.stops.size(); ++i)
		{
			const Cell to = route.stops[i].cell;
			if (occupied.count({ to.x, to.z }))
				return r.fail(childPointer(childPointer("/debugRoute/stops", i), "cell"), "stop " + describeCell(to) + " is blocked");
		}
		return true;
	}
}

std::vector<Cell> corridorStripCells(Cell from, Cell to)
{
	// The long axis is the one that is not 2 cells wide (a 2 x 2 strip runs along z).
	const int span_x = std::abs(to.x - from.x) + 1;
	const bool along_x = span_x != 2;
	const int step_x = to.x >= from.x ? 1 : -1;
	const int step_z = to.z >= from.z ? 1 : -1;
	std::vector<Cell> cells;
	if (along_x)
	{
		for (int x = from.x;; x += step_x)
		{
			for (int z = from.z;; z += step_z)
			{
				cells.push_back(Cell{ x, z });
				if (z == to.z)
					break;
			}
			if (x == to.x)
				break;
		}
	}
	else
	{
		for (int z = from.z;; z += step_z)
		{
			for (int x = from.x;; x += step_x)
			{
				cells.push_back(Cell{ x, z });
				if (x == to.x)
					break;
			}
			if (z == to.z)
				break;
		}
	}
	return cells;
}

NavigationGrid buildNavigationGrid(const ScenarioDefinition& scenario)
{
	NavigationGrid grid(scenario.grid.width, scenario.grid.height, scenario.grid.cell_size,
		scenario.grid.origin_x, scenario.grid.origin_z);
	for (const DepositDefinition& deposit : scenario.deposits)
		grid.setBlocked(deposit.cell, true);
	for (const StaticBlockerDefinition& blocker : scenario.static_blockers)
		for (int z = blocker.min.z; z <= blocker.max.z; ++z)
			for (int x = blocker.min.x; x <= blocker.max.x; ++x)
				grid.setBlocked(Cell{ x, z }, true);
	return grid;
}

ScenarioLoadResult ScenarioLoader::load(const std::filesystem::path& path) const
{
	json_data::Json document;
	DataLoadError error;
	if (!json_data::loadJson(path, document, error))
	{
		ScenarioLoadResult result;
		result.error = error;
		return result;
	}
	return parse(document, path);
}

ScenarioLoadResult ScenarioLoader::parse(const json_data::Json& document, const std::filesystem::path& path) const
{
	ScenarioLoadResult result;
	DataLoadError error;
	Reader r(path, error);
	ScenarioDefinition scenario;
	std::set<std::string> ids; // deposits and blockers share one id namespace

	const bool ok =
		r.keys(document, {"schemaVersion", "grid", "deposits", "staticBlockers", "initialColonists",
			"initialInventory", "cargoCapacity", "initialBuildings"}, "", {"debugRoute"}) &&
		json_data::checkSchemaVersion(document, supported_schema_version, path, error) &&
		parseGrid(r, document["grid"], scenario.grid) &&
		parseDeposits(r, document["deposits"], scenario, ids) &&
		parseBlockers(r, document["staticBlockers"], scenario, ids) &&
		parseColonists(r, document["initialColonists"], scenario) &&
		parseStartState(r, document, scenario) &&
		(!document.contains("debugRoute") || parseDebugRoute(r, document["debugRoute"], scenario)) &&
		validateLayout(r, scenario);

	if (!ok)
	{
		result.error = error;
		return result;
	}
	result.scenario = std::move(scenario);
	return result;
}

bool ScenarioLoader::validateSceneBlockers(const ScenarioDefinition& scenario,
	const std::vector<StaticSceneInstance>& scene_instances,
	const std::filesystem::path& scene_path, DataLoadError& error)
{
	const NavigationGrid grid(scenario.grid.width, scenario.grid.height, scenario.grid.cell_size,
		scenario.grid.origin_x, scenario.grid.origin_z);

	std::map<std::pair<int, int>, std::string> blocker_at;
	std::set<std::string> blocker_ids;
	for (const DepositDefinition& deposit : scenario.deposits)
	{
		blocker_at[{ deposit.cell.x, deposit.cell.z }] = deposit.id;
		blocker_ids.insert(deposit.id);
	}
	for (const StaticBlockerDefinition& blocker : scenario.static_blockers)
	{
		blocker_ids.insert(blocker.id);
		for (int z = blocker.min.z; z <= blocker.max.z; ++z)
			for (int x = blocker.min.x; x <= blocker.max.x; ++x)
				blocker_at[{ x, z }] = blocker.id;
	}

	std::set<std::string> with_visual;
	for (const StaticSceneInstance& instance : scene_instances)
	{
		if (!instance.blocks_navigation)
			continue;
		Cell cell;
		const std::string pointer = "/instances/" + instance.id;
		if (!grid.worldToCell(instance.position.x, instance.position.z, cell))
		{
			error = makeError(scene_path, pointer, "blocking instance lies outside the navigation grid");
			return false;
		}
		const auto it = blocker_at.find({ cell.x, cell.z });
		if (it == blocker_at.end())
		{
			error = makeError(scene_path, pointer, "blocksNavigation is true but scenario.json has no deposit/blocker at cell " + describeCell(cell));
			return false;
		}
		with_visual.insert(it->second);
	}
	for (const std::string& id : blocker_ids)
	{
		if (!with_visual.count(id))
		{
			error = makeError(scene_path, "/instances", "scenario blocker '" + id + "' has no blocking visual in scene.json");
			return false;
		}
	}
	return true;
}
