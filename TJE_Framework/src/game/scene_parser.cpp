#include "scene_parser.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <set>
#include <string>

namespace
{
	using Json = nlohmann::json;

	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer, const std::string& message)
	{
		return DataLoadError{ file, pointer, message };
	}

	bool validateKeys(const Json& value, const std::set<std::string>& required,
		const std::set<std::string>& optional, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_object())
		{
			error = makeError(file, pointer, "expected an object");
			return false;
		}
		for (const std::string& key : required)
		{
			if (!value.contains(key))
			{
				error = makeError(file, pointer + "/" + key, "required field is missing");
				return false;
			}
		}
		for (auto it = value.begin(); it != value.end(); ++it)
		{
			if (!required.count(it.key()) && !optional.count(it.key()))
			{
				error = makeError(file, pointer + "/" + it.key(), "unknown field");
				return false;
			}
		}
		return true;
	}

	bool readString(const Json& value, std::string& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_string() || value.get_ref<const std::string&>().empty())
		{
			error = makeError(file, pointer, "expected a non-empty string");
			return false;
		}
		output = value.get<std::string>();
		return true;
	}

	bool readFiniteFloat(const Json& value, float& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_number())
		{
			error = makeError(file, pointer, "expected a finite number");
			return false;
		}
		const double number = value.get<double>();
		if (!std::isfinite(number) || !std::isfinite(static_cast<float>(number)))
		{
			error = makeError(file, pointer, "number must be finite and representable as a float");
			return false;
		}
		output = static_cast<float>(number);
		return true;
	}

	bool readVector3(const Json& value, Vector3& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.size() != 3)
		{
			error = makeError(file, pointer, "expected an array of exactly three numbers");
			return false;
		}
		float components[3]{};
		for (size_t i = 0; i < 3; ++i)
		{
			if (!readFiniteFloat(value[i], components[i], file, pointer + "/" + std::to_string(i), error))
				return false;
		}
		output.set(components[0], components[1], components[2]);
		return true;
	}
}

SceneLoadResult SceneParser::parse(const std::filesystem::path& path, const AssetCatalog& assets) const
{
	SceneLoadResult result;
	Json document;
	std::ifstream input(path);
	if (!input)
	{
		result.error = makeError(path, "", "could not open JSON file");
		return result;
	}
	try
	{
		input >> document;
	}
	catch (const Json::parse_error& exception)
	{
		result.error = makeError(path, "", std::string("invalid JSON at byte ") + std::to_string(exception.byte) + ": " + exception.what());
		return result;
	}

	DataLoadError error;
	if (!validateKeys(document, {"schemaVersion", "instances"}, {}, path, "", error))
	{
		result.error = error;
		return result;
	}
	if (!document["schemaVersion"].is_number_integer() || document["schemaVersion"].get<int>() != 1)
	{
		result.error = makeError(path, "/schemaVersion", "expected supported schema version 1");
		return result;
	}
	if (!document["instances"].is_array())
	{
		result.error = makeError(path, "/instances", "expected an array");
		return result;
	}

	std::set<std::string> ids;
	result.instances.reserve(document["instances"].size());
	for (size_t i = 0; i < document["instances"].size(); ++i)
	{
		const Json& item = document["instances"][i];
		const std::string pointer = "/instances/" + std::to_string(i);
		if (!validateKeys(item, {"id", "assetId", "position", "rotationYDegrees", "scale", "blocksNavigation"},
			{"materialId"}, path, pointer, error))
		{
			result.error = error;
			return result;
		}

		StaticSceneInstance instance;
		if (!readString(item["id"], instance.id, path, pointer + "/id", error) ||
			!readString(item["assetId"], instance.asset_id, path, pointer + "/assetId", error) ||
			!readVector3(item["position"], instance.position, path, pointer + "/position", error) ||
			!readVector3(item["scale"], instance.scale, path, pointer + "/scale", error) ||
			!readFiniteFloat(item["rotationYDegrees"], instance.rotation_y_degrees, path, pointer + "/rotationYDegrees", error))
		{
			result.error = error;
			return result;
		}
		if (!ids.insert(instance.id).second)
		{
			result.error = makeError(path, pointer + "/id", "duplicate instance id '" + instance.id + "'");
			return result;
		}
		if (!item["blocksNavigation"].is_boolean())
		{
			result.error = makeError(path, pointer + "/blocksNavigation", "expected a boolean");
			return result;
		}
		instance.blocks_navigation = item["blocksNavigation"].get<bool>();

		const AssetDefinition* asset = assets.findAsset(instance.asset_id);
		if (!asset)
		{
			result.error = makeError(path, pointer + "/assetId", "unknown asset id '" + instance.asset_id + "'");
			return result;
		}
		instance.material_id = asset->material_id;
		if (item.contains("materialId") && !readString(item["materialId"], instance.material_id, path, pointer + "/materialId", error))
		{
			result.error = error;
			return result;
		}
		if (!assets.findMaterial(instance.material_id))
		{
			result.error = makeError(path, pointer + "/materialId", "unknown material id '" + instance.material_id + "'");
			return result;
		}
		if (instance.scale.x <= 0.0f || instance.scale.y <= 0.0f || instance.scale.z <= 0.0f ||
			std::fabs(instance.scale.x - instance.scale.y) > 0.0001f ||
			std::fabs(instance.scale.x - instance.scale.z) > 0.0001f)
		{
			result.error = makeError(path, pointer + "/scale", "scene scale must be positive and uniform");
			return result;
		}
		result.instances.push_back(instance);
	}

	return result;
}
