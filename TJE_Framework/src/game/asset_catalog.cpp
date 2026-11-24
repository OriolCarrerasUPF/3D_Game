// Headless half of AssetCatalog: parses and validates assets.json/materials.json.
// GPU resource loading lives in asset_catalog_resources.cpp so this file (and the
// tests that use it) never needs an OpenGL context.

#include "asset_catalog.h"

#include "json_data.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace
{
	using json_data::Json;
	using json_data::childPointer;
	using json_data::makeError;

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

	bool loadMaterials(const std::filesystem::path& materials_file,
		std::map<std::string, SemanticMaterial>& materials, DataLoadError& error)
	{
		Json document;
		if (!json_data::loadJson(materials_file, document, error) ||
			!json_data::validateKeys(document, {"schemaVersion", "materials"}, {}, materials_file, "", error) ||
			!json_data::checkSchemaVersion(document, 1, materials_file, error) ||
			!json_data::requireArray(document["materials"], 1, materials_file, "/materials", error))
			return false;

		for (std::size_t i = 0; i < document["materials"].size(); ++i)
		{
			const Json& item = document["materials"][i];
			const std::string pointer = childPointer("/materials", i);
			SemanticMaterial material;
			if (!json_data::validateKeys(item, {"id", "tint"}, {}, materials_file, pointer, error) ||
				!json_data::readString(item["id"], material.id, materials_file, childPointer(pointer, "id"), error) ||
				!json_data::readColor4(item["tint"], material.tint, materials_file, childPointer(pointer, "tint"), error))
				return false;
			if (!materials.emplace(material.id, material).second)
			{
				error = makeError(materials_file, childPointer(pointer, "id"), "duplicate material id '" + material.id + "'");
				return false;
			}
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
	debug_shader = nullptr;

	AssetCatalogLoadResult result;
	DataLoadError error;
	const std::filesystem::path absolute_assets_file = std::filesystem::absolute(assets_file).lexically_normal();
	assets_path = absolute_assets_file;
	arkfall_root = absolute_assets_file.parent_path();
	data_root = arkfall_root.parent_path();

	if (!loadMaterials(materials_file, materials, error))
	{
		materials.clear();
		result.error = error;
		return result;
	}

	Json document;
	if (!json_data::loadJson(assets_file, document, error) ||
		!json_data::validateKeys(document, {"schemaVersion", "shaders", "assets"}, {}, assets_file, "", error) ||
		!json_data::checkSchemaVersion(document, 1, assets_file, error))
	{
		materials.clear();
		result.error = error;
		return result;
	}

	const Json& shader_document = document["shaders"];
	if (!json_data::validateKeys(shader_document, {"standardVertex", "instancedVertex", "fragment", "debugFragment"}, {}, assets_file, "/shaders", error) ||
		!json_data::resolveDataPath(arkfall_root, data_root, shader_document["standardVertex"], standard_vertex_path, assets_file, "/shaders/standardVertex", error) ||
		!json_data::resolveDataPath(arkfall_root, data_root, shader_document["instancedVertex"], instanced_vertex_path, assets_file, "/shaders/instancedVertex", error) ||
		!json_data::resolveDataPath(arkfall_root, data_root, shader_document["fragment"], fragment_path, assets_file, "/shaders/fragment", error) ||
		!json_data::resolveDataPath(arkfall_root, data_root, shader_document["debugFragment"], debug_fragment_path, assets_file, "/shaders/debugFragment", error) ||
		!json_data::requireArray(document["assets"], 1, assets_file, "/assets", error))
	{
		materials.clear();
		result.error = error;
		return result;
	}

	std::map<std::string, AssetDefinition> parsed_assets;
	const std::filesystem::path models_root = std::filesystem::weakly_canonical(arkfall_root / "models");
	for (std::size_t i = 0; i < document["assets"].size(); ++i)
	{
		const Json& item = document["assets"][i];
		const std::string pointer = childPointer("/assets", i);
		auto fail = [&](const DataLoadError& failure) {
			materials.clear();
			result.error = failure;
			return result;
		};

		if (!json_data::validateKeys(item,
			{"id", "modelPath", "materialPath", "materialId", "scale", "pivotOffset", "forwardAxis",
			 "pickingBounds", "walkableSurface", "loadFailureIsFatal"},
			{}, assets_file, pointer, error))
			return fail(error);

		AssetDefinition asset;
		if (!json_data::readString(item["id"], asset.id, assets_file, childPointer(pointer, "id"), error) ||
			!json_data::readString(item["materialId"], asset.material_id, assets_file, childPointer(pointer, "materialId"), error) ||
			!json_data::readString(item["forwardAxis"], asset.forward_axis, assets_file, childPointer(pointer, "forwardAxis"), error) ||
			!json_data::readVector3(item["scale"], asset.default_scale, assets_file, childPointer(pointer, "scale"), error) ||
			!json_data::readVector3(item["pivotOffset"], asset.pivot_offset, assets_file, childPointer(pointer, "pivotOffset"), error) ||
			!json_data::readBool(item["walkableSurface"], asset.walkable_surface, assets_file, childPointer(pointer, "walkableSurface"), error) ||
			!json_data::readBool(item["loadFailureIsFatal"], asset.load_failure_is_fatal, assets_file, childPointer(pointer, "loadFailureIsFatal"), error))
			return fail(error);

		if (!json_data::isPositiveUniformScale(asset.default_scale))
			return fail(makeError(assets_file, childPointer(pointer, "scale"), "asset scale must be positive and uniform"));
		if (asset.forward_axis != "+X" && asset.forward_axis != "-X" && asset.forward_axis != "+Z" && asset.forward_axis != "-Z")
			return fail(makeError(assets_file, childPointer(pointer, "forwardAxis"), "expected one of +X, -X, +Z or -Z"));
		if (!materials.count(asset.material_id))
			return fail(makeError(assets_file, childPointer(pointer, "materialId"), "unknown material id '" + asset.material_id + "'"));

		const Json& bounds = item["pickingBounds"];
		const std::string bounds_pointer = childPointer(pointer, "pickingBounds");
		if (!json_data::validateKeys(bounds, {"min", "max"}, {}, assets_file, bounds_pointer, error) ||
			!json_data::readVector3(bounds["min"], asset.source_bounds_min, assets_file, childPointer(bounds_pointer, "min"), error) ||
			!json_data::readVector3(bounds["max"], asset.source_bounds_max, assets_file, childPointer(bounds_pointer, "max"), error))
			return fail(error);
		if (asset.source_bounds_min.x > asset.source_bounds_max.x ||
			asset.source_bounds_min.y > asset.source_bounds_max.y ||
			asset.source_bounds_min.z > asset.source_bounds_max.z)
			return fail(makeError(assets_file, bounds_pointer, "minimum bounds must not exceed maximum bounds"));

		// Bounds in asset space: source vertices scaled first, then offset by the pivot.
		asset.picking_bounds_min = asset.source_bounds_min * asset.default_scale + asset.pivot_offset;
		asset.picking_bounds_max = asset.source_bounds_max * asset.default_scale + asset.pivot_offset;

		std::filesystem::path model_path;
		std::filesystem::path material_path;
		if (!json_data::resolveDataPath(arkfall_root, data_root, item["modelPath"], model_path, assets_file, childPointer(pointer, "modelPath"), error) ||
			!json_data::resolveDataPath(arkfall_root, data_root, item["materialPath"], material_path, assets_file, childPointer(pointer, "materialPath"), error))
			return fail(error);
		if (!json_data::isWithin(model_path, models_root) || !json_data::isWithin(material_path, models_root))
			return fail(makeError(assets_file, pointer, "model and material paths must stay under data/arkfall/models"));
		if (model_path.extension() != ".obj" || material_path.extension() != ".mtl")
			return fail(makeError(assets_file, pointer, "assets must be an OBJ model with its MTL material file"));
		if (!validateObjMaterialReference(model_path, material_path, assets_file, childPointer(pointer, "materialPath"), error))
			return fail(error);

		asset.model_path = model_path;
		asset.material_path = material_path;
		if (!parsed_assets.emplace(asset.id, asset).second)
			return fail(makeError(assets_file, childPointer(pointer, "id"), "duplicate asset id '" + asset.id + "'"));
	}

	assets.swap(parsed_assets);
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
