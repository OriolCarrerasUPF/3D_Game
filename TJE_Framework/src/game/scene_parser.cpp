#include "scene_parser.h"

#include "json_data.h"

#include <set>
#include <string>

SceneLoadResult SceneParser::parse(const std::filesystem::path& path, const AssetCatalog& assets) const
{
	using json_data::Json;
	using json_data::childPointer;
	using json_data::makeError;

	SceneLoadResult result;
	DataLoadError error;
	Json document;
	if (!json_data::loadJsonFile(path, document, error) ||
		!json_data::validateKeys(document, {"schemaVersion", "instances"}, {}, path, "", error) ||
		!json_data::checkSchemaVersion(document, 1, path, error))
	{
		result.error = error;
		return result;
	}
	if (!document["instances"].is_array())
	{
		result.error = makeError(path, "/instances", "expected an array");
		return result;
	}

	std::set<std::string> ids;
	std::vector<StaticSceneInstance> instances;
	instances.reserve(document["instances"].size());
	for (std::size_t i = 0; i < document["instances"].size(); ++i)
	{
		const Json& item = document["instances"][i];
		const std::string pointer = childPointer("/instances", i);
		auto fail = [&](const DataLoadError& failure) {
			result.error = failure;
			return result;
		};

		StaticSceneInstance instance;
		if (!json_data::validateKeys(item, {"id", "assetId", "position", "rotationYDegrees", "scale", "blocksNavigation"},
				{"materialId"}, path, pointer, error) ||
			!json_data::readString(item["id"], instance.id, path, childPointer(pointer, "id"), error) ||
			!json_data::readString(item["assetId"], instance.asset_id, path, childPointer(pointer, "assetId"), error) ||
			!json_data::readVector3(item["position"], instance.position, path, childPointer(pointer, "position"), error) ||
			!json_data::readVector3(item["scale"], instance.scale, path, childPointer(pointer, "scale"), error) ||
			!json_data::readFiniteFloat(item["rotationYDegrees"], instance.rotation_y_degrees, path, childPointer(pointer, "rotationYDegrees"), error) ||
			!json_data::readBool(item["blocksNavigation"], instance.blocks_navigation, path, childPointer(pointer, "blocksNavigation"), error))
			return fail(error);

		if (!ids.insert(instance.id).second)
			return fail(makeError(path, childPointer(pointer, "id"), "duplicate instance id '" + instance.id + "'"));

		const AssetDefinition* asset = assets.findAsset(instance.asset_id);
		if (!asset)
			return fail(makeError(path, childPointer(pointer, "assetId"), "unknown asset id '" + instance.asset_id + "'"));

		instance.material_id = asset->material_id;
		if (item.contains("materialId") &&
			!json_data::readString(item["materialId"], instance.material_id, path, childPointer(pointer, "materialId"), error))
			return fail(error);
		if (!assets.findMaterial(instance.material_id))
			return fail(makeError(path, childPointer(pointer, "materialId"), "unknown material id '" + instance.material_id + "'"));
		if (!json_data::isPositiveUniformScale(instance.scale))
			return fail(makeError(path, childPointer(pointer, "scale"), "scene scale must be positive and uniform"));

		instances.push_back(instance);
	}

	result.instances.swap(instances);
	return result;
}
