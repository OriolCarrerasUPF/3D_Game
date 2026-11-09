#include "asset_catalog.h"

#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

namespace
{
	using Json = nlohmann::json;

	DataLoadError makeError(const std::filesystem::path& file, const std::string& pointer, const std::string& message)
	{
		return DataLoadError{ file, pointer, message };
	}

	bool loadJson(const std::filesystem::path& path, Json& result, DataLoadError& error)
	{
		std::ifstream input(path);
		if (!input)
		{
			error = makeError(path, "", "could not open JSON file");
			return false;
		}

		try
		{
			input >> result;
		}
		catch (const Json::parse_error& exception)
		{
			error = makeError(path, "", std::string("invalid JSON at byte ") + std::to_string(exception.byte) + ": " + exception.what());
			return false;
		}
		return true;
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

	bool readVector4(const Json& value, Vector4& output, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_array() || value.size() != 4)
		{
			error = makeError(file, pointer, "expected an array of exactly four numbers");
			return false;
		}
		float components[4]{};
		for (size_t i = 0; i < 4; ++i)
		{
			if (!readFiniteFloat(value[i], components[i], file, pointer + "/" + std::to_string(i), error))
				return false;
			if (components[i] < 0.0f || components[i] > 1.0f)
			{
				error = makeError(file, pointer + "/" + std::to_string(i), "tint components must be in [0, 1]");
				return false;
			}
		}
		output.set(components[0], components[1], components[2], components[3]);
		return true;
	}

	bool isWithin(const std::filesystem::path& child, const std::filesystem::path& root)
	{
		const std::filesystem::path relative = child.lexically_relative(root);
		return !relative.empty() && relative != "." && *relative.begin() != "..";
	}

	bool resolveDataPath(const std::filesystem::path& base, const std::filesystem::path& data_root,
		const Json& value, std::filesystem::path& resolved, const std::filesystem::path& file,
		const std::string& pointer, DataLoadError& error)
	{
		if (!value.is_string())
		{
			error = makeError(file, pointer, "expected a relative path string");
			return false;
		}
		const std::filesystem::path relative_path(value.get<std::string>());
		if (relative_path.empty() || relative_path.is_absolute() || relative_path.has_root_name())
		{
			error = makeError(file, pointer, "path must be relative to the data directory");
			return false;
		}

		std::error_code ec;
		const auto canonical_data_root = std::filesystem::weakly_canonical(data_root, ec);
		if (ec)
		{
			error = makeError(file, pointer, "could not resolve the data directory");
			return false;
		}
		const auto candidate = std::filesystem::weakly_canonical(base / relative_path, ec);
		if (ec || !isWithin(candidate, canonical_data_root))
		{
			error = makeError(file, pointer, "path escapes the data directory or cannot be resolved");
			return false;
		}
		if (!std::filesystem::is_regular_file(candidate, ec) || ec)
		{
			error = makeError(file, pointer, "referenced file does not exist");
			return false;
		}
		resolved = candidate;
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

	bool validateObjMaterialReference(const std::filesystem::path& model_path,
		const std::filesystem::path& material_path, const std::filesystem::path& assets_file,
		const std::string& pointer, DataLoadError& error)
	{
		std::ifstream model_file(model_path);
		if (!model_file)
		{
			error = makeError(assets_file, pointer, "could not inspect OBJ for its mtllib declaration");
			return false;
		}
		bool found_expected_material = false;
		std::string line;
		while (std::getline(model_file, line))
		{
			std::istringstream line_stream(line);
			std::string directive;
			line_stream >> directive;
			if (directive != "mtllib")
				continue;
			std::string referenced_name;
			if (!(line_stream >> referenced_name))
			{
				error = makeError(assets_file, pointer, "OBJ mtllib declaration has no filename");
				return false;
			}
			std::error_code ec;
			const auto referenced_path = std::filesystem::weakly_canonical(model_path.parent_path() / referenced_name, ec);
			if (!ec && referenced_path == material_path)
				found_expected_material = true;
		}
		if (model_file.bad() || !found_expected_material)
		{
			error = makeError(assets_file, pointer, "materialPath must name the MTL file referenced by the OBJ");
			return false;
		}
		return true;
	}
}

AssetCatalogLoadResult AssetCatalog::load(const std::filesystem::path& assets_file,
	const std::filesystem::path& materials_file)
{
	assets.clear();
	materials.clear();
	standard_shader = nullptr;
	instanced_shader = nullptr;

	AssetCatalogLoadResult result;
	const std::filesystem::path absolute_assets_file = std::filesystem::absolute(assets_file).lexically_normal();
	arkfall_root = absolute_assets_file.parent_path();
	data_root = arkfall_root.parent_path();

	Json material_document;
	DataLoadError error;
	if (!loadJson(materials_file, material_document, error))
	{
		result.error = error;
		return result;
	}
	if (!validateKeys(material_document, {"schemaVersion", "materials"}, {}, materials_file, "", error))
	{
		result.error = error;
		return result;
	}
	if (!material_document["schemaVersion"].is_number_integer() || material_document["schemaVersion"].get<int>() != 1)
	{
		result.error = makeError(materials_file, "/schemaVersion", "expected supported schema version 1");
		return result;
	}
	if (!material_document["materials"].is_array() || material_document["materials"].empty())
	{
		result.error = makeError(materials_file, "/materials", "expected a non-empty array");
		return result;
	}

	for (size_t i = 0; i < material_document["materials"].size(); ++i)
	{
		const Json& item = material_document["materials"][i];
		const std::string pointer = "/materials/" + std::to_string(i);
		if (!validateKeys(item, {"id", "tint"}, {}, materials_file, pointer, error))
		{
			result.error = error;
			return result;
		}

		SemanticMaterial material;
		if (!readString(item["id"], material.id, materials_file, pointer + "/id", error) ||
			!readVector4(item["tint"], material.tint, materials_file, pointer + "/tint", error))
		{
			result.error = error;
			return result;
		}
		if (!materials.emplace(material.id, material).second)
		{
			result.error = makeError(materials_file, pointer + "/id", "duplicate material id '" + material.id + "'");
			return result;
		}
	}

	Json asset_document;
	if (!loadJson(assets_file, asset_document, error))
	{
		result.error = error;
		return result;
	}
	if (!validateKeys(asset_document, {"schemaVersion", "shaders", "assets"}, {}, assets_file, "", error))
	{
		result.error = error;
		return result;
	}
	if (!asset_document["schemaVersion"].is_number_integer() || asset_document["schemaVersion"].get<int>() != 1)
	{
		result.error = makeError(assets_file, "/schemaVersion", "expected supported schema version 1");
		return result;
	}

	const Json& shader_document = asset_document["shaders"];
	if (!validateKeys(shader_document, {"standardVertex", "instancedVertex", "fragment"}, {}, assets_file, "/shaders", error) ||
		!resolveDataPath(arkfall_root, data_root, shader_document["standardVertex"], standard_vertex_path, assets_file, "/shaders/standardVertex", error) ||
		!resolveDataPath(arkfall_root, data_root, shader_document["instancedVertex"], instanced_vertex_path, assets_file, "/shaders/instancedVertex", error) ||
		!resolveDataPath(arkfall_root, data_root, shader_document["fragment"], fragment_path, assets_file, "/shaders/fragment", error))
	{
		result.error = error;
		return result;
	}
	if (!asset_document["assets"].is_array() || asset_document["assets"].empty())
	{
		result.error = makeError(assets_file, "/assets", "expected a non-empty array");
		return result;
	}

	const std::filesystem::path models_root = std::filesystem::weakly_canonical(arkfall_root / "models");
	for (size_t i = 0; i < asset_document["assets"].size(); ++i)
	{
		const Json& item = asset_document["assets"][i];
		const std::string pointer = "/assets/" + std::to_string(i);
		if (!validateKeys(item,
			{"id", "modelPath", "materialPath", "materialId", "scale", "pivotOffset", "forwardAxis", "pickingBounds", "loadFailureIsFatal"},
			{}, assets_file, pointer, error))
		{
			result.error = error;
			return result;
		}

		AssetDefinition asset;
		std::filesystem::path model_path;
		std::filesystem::path material_path;
		if (!readString(item["id"], asset.id, assets_file, pointer + "/id", error) ||
			!readString(item["materialId"], asset.material_id, assets_file, pointer + "/materialId", error) ||
			!readString(item["forwardAxis"], asset.forward_axis, assets_file, pointer + "/forwardAxis", error) ||
			!readVector3(item["scale"], asset.default_scale, assets_file, pointer + "/scale", error) ||
			!readVector3(item["pivotOffset"], asset.pivot_offset, assets_file, pointer + "/pivotOffset", error))
		{
			result.error = error;
			return result;
		}
		if (!item["loadFailureIsFatal"].is_boolean())
		{
			result.error = makeError(assets_file, pointer + "/loadFailureIsFatal", "expected a boolean");
			return result;
		}
		asset.load_failure_is_fatal = item["loadFailureIsFatal"].get<bool>();
		if (asset.default_scale.x <= 0.0f || asset.default_scale.y <= 0.0f || asset.default_scale.z <= 0.0f ||
			std::fabs(asset.default_scale.x - asset.default_scale.y) > 0.0001f ||
			std::fabs(asset.default_scale.x - asset.default_scale.z) > 0.0001f)
		{
			result.error = makeError(assets_file, pointer + "/scale", "asset scale must be positive and uniform");
			return result;
		}
		if (asset.forward_axis != "+X" && asset.forward_axis != "-X" && asset.forward_axis != "+Z" && asset.forward_axis != "-Z")
		{
			result.error = makeError(assets_file, pointer + "/forwardAxis", "expected one of +X, -X, +Z or -Z");
			return result;
		}
		if (!materials.count(asset.material_id))
		{
			result.error = makeError(assets_file, pointer + "/materialId", "unknown material id '" + asset.material_id + "'");
			return result;
		}

		const Json& bounds = item["pickingBounds"];
		if (!validateKeys(bounds, {"min", "max"}, {}, assets_file, pointer + "/pickingBounds", error) ||
			!readVector3(bounds["min"], asset.source_bounds_min, assets_file, pointer + "/pickingBounds/min", error) ||
			!readVector3(bounds["max"], asset.source_bounds_max, assets_file, pointer + "/pickingBounds/max", error))
		{
			result.error = error;
			return result;
		}
		if (asset.source_bounds_min.x > asset.source_bounds_max.x ||
			asset.source_bounds_min.y > asset.source_bounds_max.y ||
			asset.source_bounds_min.z > asset.source_bounds_max.z)
		{
			result.error = makeError(assets_file, pointer + "/pickingBounds", "minimum bounds must not exceed maximum bounds");
			return result;
		}
		asset.picking_bounds_min.set(
			asset.source_bounds_min.x * asset.default_scale.x + asset.pivot_offset.x,
			asset.source_bounds_min.y * asset.default_scale.y + asset.pivot_offset.y,
			asset.source_bounds_min.z * asset.default_scale.z + asset.pivot_offset.z);
		asset.picking_bounds_max.set(
			asset.source_bounds_max.x * asset.default_scale.x + asset.pivot_offset.x,
			asset.source_bounds_max.y * asset.default_scale.y + asset.pivot_offset.y,
			asset.source_bounds_max.z * asset.default_scale.z + asset.pivot_offset.z);

		if (!resolveDataPath(arkfall_root, data_root, item["modelPath"], model_path, assets_file, pointer + "/modelPath", error) ||
			!resolveDataPath(arkfall_root, data_root, item["materialPath"], material_path, assets_file, pointer + "/materialPath", error))
		{
			result.error = error;
			return result;
		}
		if (!isWithin(model_path, models_root) || !isWithin(material_path, models_root))
		{
			result.error = makeError(assets_file, pointer, "model and material paths must stay under data/arkfall/models");
			return result;
		}
		if (!validateObjMaterialReference(model_path, material_path, assets_file, pointer + "/materialPath", error))
		{
			result.error = error;
			return result;
		}
		if (model_path.extension() != ".obj" || material_path.extension() != ".mtl")
		{
			result.error = makeError(assets_file, pointer, "P02 requires an OBJ model and its MTL material file");
			return result;
		}
		asset.model_path = model_path;
		asset.material_path = material_path;
		if (!assets.emplace(asset.id, asset).second)
		{
			result.error = makeError(assets_file, pointer + "/id", "duplicate asset id '" + asset.id + "'");
			return result;
		}
	}

	return result;
}

AssetCatalogLoadResult AssetCatalog::loadReferencedAssets(const std::set<std::string>& asset_ids)
{
	AssetCatalogLoadResult result;
	for (const std::string& id : asset_ids)
	{
		if (!assets.count(id))
		{
			result.error = DataLoadError{ arkfall_root / "assets.json", "/assets", "unknown asset id '" + id + "'" };
			return result;
		}
	}

	standard_shader = Shader::Get(standard_vertex_path.string().c_str(), fragment_path.string().c_str());
	if (!standard_shader || !standard_shader->compiled)
	{
		result.error = DataLoadError{ fragment_path, "", "could not compile the standard Arkfall shader" };
		return result;
	}
	instanced_shader = Shader::Get(instanced_vertex_path.string().c_str(), fragment_path.string().c_str());
	if (!instanced_shader || !instanced_shader->compiled)
	{
		result.error = DataLoadError{ fragment_path, "", "could not compile the instanced Arkfall shader" };
		return result;
	}

	const bool previous_use_binary = Mesh::use_binary;
	Mesh::use_binary = false; // validate the selected source OBJ/MTL files, not stale .mbin caches
	for (const std::string& id : asset_ids)
	{
		AssetDefinition& asset = assets.at(id);
		asset.mesh = Mesh::Get(asset.model_path.string().c_str());
		if (!asset.mesh && asset.load_failure_is_fatal)
		{
			Mesh::use_binary = previous_use_binary;
			result.error = DataLoadError{ asset.model_path, "/assets/" + id + "/modelPath", "OBJ/MTL loader rejected this asset" };
			return result;
		}
		if (asset.mesh)
		{
			const Vector3& actual_min = asset.mesh->aabb_min;
			const Vector3& actual_max = asset.mesh->aabb_max;
			const float tolerance = 0.0001f;
			const bool bounds_match =
				std::fabs(actual_min.x - asset.source_bounds_min.x) <= tolerance &&
				std::fabs(actual_min.y - asset.source_bounds_min.y) <= tolerance &&
				std::fabs(actual_min.z - asset.source_bounds_min.z) <= tolerance &&
				std::fabs(actual_max.x - asset.source_bounds_max.x) <= tolerance &&
				std::fabs(actual_max.y - asset.source_bounds_max.y) <= tolerance &&
				std::fabs(actual_max.z - asset.source_bounds_max.z) <= tolerance;
			if (!bounds_match)
			{
				asset.mesh = nullptr;
				if (asset.load_failure_is_fatal)
				{
					Mesh::use_binary = previous_use_binary;
					result.error = DataLoadError{ asset.model_path, "/assets/" + id + "/pickingBounds",
						"catalog bounds do not match the loaded OBJ bounds" };
					return result;
				}
			}
		}
	}
	Mesh::use_binary = previous_use_binary;
	return result;
}

const AssetDefinition* AssetCatalog::findAsset(const std::string& id) const
{
	const auto it = assets.find(id);
	return it == assets.end() ? nullptr : &it->second;
}

const SemanticMaterial* AssetCatalog::findMaterial(const std::string& id) const
{
	const auto it = materials.find(id);
	return it == materials.end() ? nullptr : &it->second;
}
