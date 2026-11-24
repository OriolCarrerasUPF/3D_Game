#include "scenario_loader.h"

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

	bool areNeighbours(Cell a, Cell b)
	{
		return std::abs(a.x - b.x) + std::abs(a.z - b.z) == 1;
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

	bool parseDoors(Reader& r, const Json& value, ScenarioDefinition& scenario, std::set<std::string>& ids)
	{
		if (!json_data::requireArray(value, 1, r.file, "/base/exitDoors", r.error))
			return false;
		std::set<std::pair<int, int>> used_cells;
		for (std::size_t d = 0; d < value.size(); ++d)
		{
			const std::string pointer = childPointer("/base/exitDoors", d);
			const Json& item = value[d];
			ExitDoorDefinition door;
			if (!r.keys(item, {"id", "openAtStart", "crossings"}, pointer) ||
				!json_data::readString(item["id"], door.id, r.file, childPointer(pointer, "id"), r.error) ||
				!json_data::readBool(item["openAtStart"], door.open_at_start, r.file, childPointer(pointer, "openAtStart"), r.error) ||
				!json_data::requireArray(item["crossings"], 1, r.file, childPointer(pointer, "crossings"), r.error))
				return false;
			if (!ids.insert(door.id).second)
				return r.fail(childPointer(pointer, "id"), "duplicate id '" + door.id + "'");

			int side_dx = 0, side_dz = 0;
			for (std::size_t c = 0; c < item["crossings"].size(); ++c)
			{
				const std::string cp = childPointer(childPointer(pointer, "crossings"), c);
				const Json& crossing = item["crossings"][c];
				DoorPortal portal;
				if (!r.keys(crossing, {"outside", "inside"}, cp) ||
					!r.cell(crossing["outside"], portal.outside, scenario.grid, childPointer(cp, "outside")) ||
					!r.cell(crossing["inside"], portal.inside, scenario.grid, childPointer(cp, "inside")))
					return false;
				if (scenario.isInterior(portal.outside))
					return r.fail(childPointer(cp, "outside"), "cell " + describeCell(portal.outside) + " is inside the base");
				if (!scenario.isInterior(portal.inside))
					return r.fail(childPointer(cp, "inside"), "cell " + describeCell(portal.inside) + " is outside the base");
				if (!areNeighbours(portal.outside, portal.inside))
					return r.fail(cp, "outside and inside cells must be 4-neighbours");
				const int dx = portal.inside.x - portal.outside.x;
				const int dz = portal.inside.z - portal.outside.z;
				if (c == 0)
				{
					side_dx = dx;
					side_dz = dz;
				}
				else if (dx != side_dx || dz != side_dz)
					return r.fail(cp, "all crossings of a door must be on the same side of the base");
				if (!used_cells.insert({ portal.inside.x, portal.inside.z }).second ||
					!used_cells.insert({ portal.outside.x, portal.outside.z }).second)
					return r.fail(cp, "crossing cells are already used by another crossing");
				door.crossings.push_back(portal);
			}

			// Contiguous along the wall: sorted coordinates along the side must be consecutive.
			std::vector<int> along;
			for (const DoorPortal& portal : door.crossings)
				along.push_back(side_dx != 0 ? portal.inside.z : portal.inside.x);
			std::sort(along.begin(), along.end());
			for (std::size_t i = 1; i < along.size(); ++i)
			{
				if (along[i] != along[i - 1] + 1)
					return r.fail(childPointer(pointer, "crossings"), "crossings of a door must be contiguous");
			}
			scenario.exit_doors.push_back(door);
		}
		return true;
	}

	bool parseBase(Reader& r, const Json& value, ScenarioDefinition& scenario, std::set<std::string>& ids)
	{
		if (!r.keys(value, {"interior", "exitDoors"}, "/base") ||
			!r.keys(value["interior"], {"min", "max"}, "/base/interior") ||
			!r.cell(value["interior"]["min"], scenario.interior_min, scenario.grid, "/base/interior/min") ||
			!r.cell(value["interior"]["max"], scenario.interior_max, scenario.grid, "/base/interior/max"))
			return false;
		if (scenario.interior_min.x > scenario.interior_max.x || scenario.interior_min.z > scenario.interior_max.z)
			return r.fail("/base/interior", "min must not exceed max");
		if (scenario.interior_min.x < 1 || scenario.interior_min.z < 1 ||
			scenario.interior_max.x > scenario.grid.width - 2 || scenario.interior_max.z > scenario.grid.height - 2)
			return r.fail("/base/interior", "leave at least one exterior cell around the base");
		return parseDoors(r, value["exitDoors"], scenario, ids);
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
			if (scenario.isInterior(deposit.cell))
				return r.fail(childPointer(pointer, "cell"), "deposits must be outside the base");
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
		for (std::size_t d = 0; d < scenario.exit_doors.size(); ++d)
			for (const DoorPortal& portal : scenario.exit_doors[d].crossings)
				for (Cell cell : { portal.outside, portal.inside })
				{
					const auto it = occupied.find({ cell.x, cell.z });
					if (it != occupied.end())
						return r.fail(childPointer("/base/exitDoors", d), "door cell " + describeCell(cell) + " is blocked by '" + it->second + "'");
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

		// Every leg (including the loop back) must be walkable with the doors' initial state.
		const NavigationGrid grid = buildNavigationGrid(scenario);
		std::vector<Cell> path;
		Cell from = colonist->cell;
		const std::size_t legs = route.stops.size() + (route.loop ? 1 : 0);
		for (std::size_t leg = 0; leg < legs; ++leg)
		{
			const std::size_t stop_index = leg % route.stops.size();
			const Cell to = route.stops[stop_index].cell;
			const std::string pointer = childPointer(childPointer("/debugRoute/stops", stop_index), "cell");
			if (!grid.isWalkable(to))
				return r.fail(pointer, "stop " + describeCell(to) + " is blocked");
			if (!grid.findPath(from, to, path))
				return r.fail(pointer, "no 4-neighbour path from " + describeCell(from) + " to " + describeCell(to));
			from = to;
		}
		return true;
	}
}

NavigationGrid buildNavigationGrid(const ScenarioDefinition& scenario)
{
	NavigationGrid grid(scenario.grid.width, scenario.grid.height, scenario.grid.cell_size,
		scenario.grid.origin_x, scenario.grid.origin_z);
	grid.setZoneRect(scenario.interior_min, scenario.interior_max, Zone::Inside);
	for (const ExitDoorDefinition& door : scenario.exit_doors)
		grid.addDoor(door.crossings, door.open_at_start);
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
	std::set<std::string> ids; // doors, deposits and blockers share one id namespace

	const bool ok =
		r.keys(document, {"schemaVersion", "grid", "base", "deposits", "staticBlockers", "initialColonists"}, "", {"debugRoute"}) &&
		json_data::checkSchemaVersion(document, supported_schema_version, path, error) &&
		parseGrid(r, document["grid"], scenario.grid) &&
		parseBase(r, document["base"], scenario, ids) &&
		parseDeposits(r, document["deposits"], scenario, ids) &&
		parseBlockers(r, document["staticBlockers"], scenario, ids) &&
		parseColonists(r, document["initialColonists"], scenario) &&
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
