#include "colonist_catalog.h"

#include "asset_catalog.h"
#include "json_data.h"

#include <set>

ColonistCatalogLoadResult ColonistCatalog::load(const std::filesystem::path& path)
{
	using json_data::Json;
	using json_data::childPointer;

	static const std::map<std::string, Job> job_names{ { "engineer", Job::Engineer }, { "miner", Job::Miner }, { "farmer", Job::Farmer } };

	roles.clear();
	names.clear();
	source_path = path;
	ColonistCatalogLoadResult result;
	DataLoadError error;
	Json document;
	if (!json_data::loadJson(path, document, error) ||
		!json_data::validateKeys(document, {"schemaVersion", "roles", "names"}, {}, path, "", error) ||
		!json_data::checkSchemaVersion(document, 1, path, error) ||
		!json_data::requireArray(document["roles"], 1, path, "/roles", error))
	{
		result.error = error;
		return result;
	}

	std::map<Job, ColonistRoleDefinition> parsed;
	for (std::size_t i = 0; i < document["roles"].size(); ++i)
	{
		const Json& item = document["roles"][i];
		const std::string pointer = childPointer("/roles", i);
		ColonistRoleDefinition role;
		if (!json_data::validateKeys(item, {"job", "assetId", "materialId", "moveSpeed"}, {}, path, pointer, error) ||
			!json_data::readEnum(item["job"], job_names, role.job, path, childPointer(pointer, "job"), error) ||
			!json_data::readString(item["assetId"], role.asset_id, path, childPointer(pointer, "assetId"), error) ||
			!json_data::readString(item["materialId"], role.material_id, path, childPointer(pointer, "materialId"), error) ||
			!json_data::readFloatInRange(item["moveSpeed"], 0.1f, 20.0f, role.move_speed, path, childPointer(pointer, "moveSpeed"), error))
		{
			result.error = error;
			return result;
		}
		if (!parsed.emplace(role.job, role).second)
		{
			result.error = json_data::makeError(path, childPointer(pointer, "job"), "duplicate role");
			return result;
		}
	}
	for (const auto& [name, job] : job_names)
	{
		if (!parsed.count(job))
		{
			result.error = json_data::makeError(path, "/roles", "missing role '" + name + "'");
			return result;
		}
	}

	// Name pool: unique, non-empty. Whether it is large enough depends on the scenario
	// (initial colonists, later + arkPassengers), so ColonySimulation checks the count.
	std::vector<std::string> parsed_names;
	std::set<std::string> seen;
	if (!json_data::requireArray(document["names"], 1, path, "/names", error))
	{
		result.error = error;
		return result;
	}
	for (std::size_t i = 0; i < document["names"].size(); ++i)
	{
		std::string name;
		if (!json_data::readString(document["names"][i], name, path, childPointer("/names", i), error))
		{
			result.error = error;
			return result;
		}
		if (!seen.insert(name).second)
		{
			result.error = json_data::makeError(path, childPointer("/names", i), "duplicate name '" + name + "'");
			return result;
		}
		parsed_names.push_back(name);
	}
	roles.swap(parsed);
	names.swap(parsed_names);
	return result;
}

ColonistCatalogLoadResult ColonistCatalog::validateVisuals(const AssetCatalog& assets) const
{
	ColonistCatalogLoadResult result;
	for (const auto& [job, role] : roles)
	{
		(void)job;
		if (!assets.findAsset(role.asset_id))
		{
			result.error = json_data::makeError(source_path, "/roles", "unknown asset id '" + role.asset_id + "'");
			return result;
		}
		if (!assets.findMaterial(role.material_id))
		{
			result.error = json_data::makeError(source_path, "/roles", "unknown material id '" + role.material_id + "'");
			return result;
		}
	}
	return result;
}

const ColonistRoleDefinition* ColonistCatalog::findRole(Job job) const
{
	const auto it = roles.find(job);
	return it == roles.end() ? nullptr : &it->second;
}
