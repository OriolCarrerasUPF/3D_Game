#include "building_catalog.h"

#include "asset_catalog.h"

#include <algorithm>
#include <set>

namespace
{
	using json_data::Json;
	using json_data::childPointer;
	using json_data::makeError;

	struct TypeName
	{
		BuildingType type;
		const char* id;
	};

	// Identity only: the JSON id each enum value is read from. No building values here.
	const TypeName type_names[building_type_count] = {
		{ BuildingType::ExitDoor, "exit_door" },
		{ BuildingType::Storage, "storage" },
		{ BuildingType::Oxygenator, "oxygenator" },
		{ BuildingType::Greenhouse, "greenhouse" },
		{ BuildingType::IronProcessor, "iron_processor" },
		{ BuildingType::CopperProcessor, "copper_processor" },
		{ BuildingType::AuriteProcessor, "aurite_processor" },
		{ BuildingType::IronMine, "iron_mine" },
		{ BuildingType::CopperMine, "copper_mine" },
		{ BuildingType::WaterExtractor, "water_extractor" },
		{ BuildingType::SolarPanel, "solar_panel" },
		{ BuildingType::LandingPad, "landing_pad" },
		{ BuildingType::HavenBeacon, "haven_beacon" },
	};

	const std::map<std::string, Job>& jobNames()
	{
		static const std::map<std::string, Job> names{ { "engineer", Job::Engineer }, { "miner", Job::Miner }, { "farmer", Job::Farmer } };
		return names;
	}

	constexpr int max_cost_amount = 10000;
	constexpr int max_slots = 16;
	constexpr int max_offset = 16;

	class Reader
	{
	public:
		Reader(const std::filesystem::path& file, DataLoadError& error) : file(file), error(error) {}

		bool keys(const Json& value, const std::set<std::string>& required, const std::string& pointer)
		{
			return json_data::validateKeys(value, required, {}, file, pointer, error);
		}
		bool fail(const std::string& pointer, const std::string& message)
		{
			error = makeError(file, pointer, message);
			return false;
		}
		bool readFloat(const Json& value, float min_value, float max_value, float& out, const std::string& pointer)
		{
			return json_data::readFloatInRange(value, min_value, max_value, out, file, pointer, error);
		}
		bool readInt(const Json& value, int min_value, int max_value, int& out, const std::string& pointer)
		{
			return json_data::readInt(value, out, min_value, max_value, file, pointer, error);
		}
		bool readOffset(const Json& value, Cell& out, const std::string& pointer)
		{
			if (!value.is_array() || value.size() != 2)
				return fail(pointer, "expected an offset [dx, dz]");
			return readInt(value[0], -max_offset, max_offset, out.x, childPointer(pointer, std::size_t(0))) &&
				readInt(value[1], -max_offset, max_offset, out.z, childPointer(pointer, std::size_t(1)));
		}

		const std::filesystem::path& file;
		DataLoadError& error;
	};

	bool parseRole(Reader& r, const Json& value, Job& role, int min_slots, int& slots, const std::string& pointer)
	{
		return json_data::readEnum(value["role"], jobNames(), role, r.file, childPointer(pointer, "role"), r.error) &&
			r.readInt(value["slots"], min_slots, max_slots, slots, childPointer(pointer, "slots"));
	}

	bool parseRecipe(Reader& r, const Json& value, RecipeDefinition& recipe, const std::string& pointer)
	{
		if (!r.keys(value, { "inputs", "outputs", "seconds", "perPresentWorker" }, pointer) ||
			!BuildingCatalog::readResourceAmounts(value["inputs"], max_cost_amount, recipe.inputs, r.file, childPointer(pointer, "inputs"), r.error) ||
			!BuildingCatalog::readResourceAmounts(value["outputs"], max_cost_amount, recipe.outputs, r.file, childPointer(pointer, "outputs"), r.error) ||
			!r.readFloat(value["seconds"], 0.1f, 3600.0f, recipe.seconds, childPointer(pointer, "seconds")) ||
			!json_data::readBool(value["perPresentWorker"], recipe.per_present_worker, r.file, childPointer(pointer, "perPresentWorker"), r.error))
			return false;
		if (recipe.outputs.empty())
			return r.fail(childPointer(pointer, "outputs"), "a recipe must produce at least one item");
		return true;
	}

	bool parseVisuals(Reader& r, const Json& value, std::vector<BuildingVisualDefinition>& visuals, const std::string& pointer)
	{
		if (!json_data::requireArray(value, 1, r.file, pointer, r.error))
			return false;
		for (std::size_t i = 0; i < value.size(); ++i)
		{
			const std::string vp = childPointer(pointer, i);
			BuildingVisualDefinition visual;
			if (!r.keys(value[i], { "assetId", "materialId", "offset", "yawDegrees", "scale" }, vp) ||
				!json_data::readString(value[i]["assetId"], visual.asset_id, r.file, childPointer(vp, "assetId"), r.error) ||
				!json_data::readString(value[i]["materialId"], visual.material_id, r.file, childPointer(vp, "materialId"), r.error) ||
				!json_data::readVector3(value[i]["offset"], visual.offset, r.file, childPointer(vp, "offset"), r.error) ||
				!r.readFloat(value[i]["yawDegrees"], -360.0f, 360.0f, visual.yaw_degrees, childPointer(vp, "yawDegrees")) ||
				!r.readFloat(value[i]["scale"], 0.01f, 100.0f, visual.scale, childPointer(vp, "scale")))
				return false;
			visuals.push_back(visual);
		}
		return true;
	}

	bool parseDefinition(Reader& r, const Json& value, BuildingDefinition& def, const std::string& pointer)
	{
		static const std::map<std::string, BuildingZone> zone_names{
			{ "interior", BuildingZone::Interior }, { "exterior", BuildingZone::Exterior }, { "boundary", BuildingZone::Boundary } };
		static const std::map<std::string, PlacementKind> placement_names{
			{ "footprint", PlacementKind::Footprint }, { "doorSocket", PlacementKind::DoorSocket } };
		static const std::map<std::string, DepositType> deposit_names{ { "iron", DepositType::Iron }, { "copper", DepositType::Copper } };
		static const std::map<std::string, ServiceType> service_names{ { "eat", ServiceType::Eat }, { "drink", ServiceType::Drink } };

		if (!r.keys(value, { "id", "label", "zone", "placement", "footprint", "requiredDeposit", "cost", "construction",
				"operation", "accessOffsets", "services", "power", "water", "oxygenCapacity", "storageCapacity", "recipe", "visuals" }, pointer))
			return false;

		if (!json_data::readString(value["id"], def.id, r.file, childPointer(pointer, "id"), r.error) ||
			!json_data::readString(value["label"], def.label, r.file, childPointer(pointer, "label"), r.error) ||
			!json_data::readEnum(value["zone"], zone_names, def.zone, r.file, childPointer(pointer, "zone"), r.error) ||
			!json_data::readEnum(value["placement"], placement_names, def.placement, r.file, childPointer(pointer, "placement"), r.error))
			return false;
		if (!BuildingCatalog::typeFromId(def.id, def.type))
			return r.fail(childPointer(pointer, "id"), "unknown building type '" + def.id + "'");

		// Placement, zone and footprint.
		const std::string footprint_pointer = childPointer(pointer, "footprint");
		if (def.placement == PlacementKind::DoorSocket)
		{
			if (def.zone != BuildingZone::Boundary)
				return r.fail(childPointer(pointer, "zone"), "a doorSocket building must use zone 'boundary'");
			if (!value["footprint"].is_null())
				return r.fail(footprint_pointer, "a doorSocket building has no footprint (use null)");
		}
		else
		{
			if (def.zone == BuildingZone::Boundary)
				return r.fail(childPointer(pointer, "zone"), "zone 'boundary' is only for doorSocket buildings");
			if (!value["footprint"].is_array() || value["footprint"].size() != 2)
				return r.fail(footprint_pointer, "expected a footprint [w, h]");
			if (!r.readInt(value["footprint"][0], 1, BuildingCatalog::max_footprint_size, def.footprint_width, childPointer(footprint_pointer, std::size_t(0))) ||
				!r.readInt(value["footprint"][1], 1, BuildingCatalog::max_footprint_size, def.footprint_height, childPointer(footprint_pointer, std::size_t(1))))
				return false;
		}

		if (!value["requiredDeposit"].is_null())
		{
			DepositType deposit = DepositType::Iron;
			if (!json_data::readEnum(value["requiredDeposit"], deposit_names, deposit, r.file, childPointer(pointer, "requiredDeposit"), r.error))
				return false;
			if (def.zone != BuildingZone::Exterior)
				return r.fail(childPointer(pointer, "requiredDeposit"), "only exterior buildings can require a deposit");
			def.required_deposit = deposit;
		}

		if (!BuildingCatalog::readResourceAmounts(value["cost"], max_cost_amount, def.cost, r.file, childPointer(pointer, "cost"), r.error))
			return false;

		// Construction (always) and operation (optional).
		const std::string construction_pointer = childPointer(pointer, "construction");
		if (!r.keys(value["construction"], { "role", "slots", "seconds" }, construction_pointer) ||
			!parseRole(r, value["construction"], def.construction_role, 1, def.construction_slots, construction_pointer) ||
			!r.readFloat(value["construction"]["seconds"], 0.1f, 3600.0f, def.construction_seconds, childPointer(construction_pointer, "seconds")))
			return false;

		if (!value["operation"].is_null())
		{
			const std::string operation_pointer = childPointer(pointer, "operation");
			if (!r.keys(value["operation"], { "role", "slots" }, operation_pointer) ||
				!parseRole(r, value["operation"], def.operation_role, 1, def.operation_slots, operation_pointer))
				return false;
			def.has_operation = true;
		}

		// Access offsets: outside the 0 deg footprint, sharing an edge with it, unique.
		const std::string access_pointer = childPointer(pointer, "accessOffsets");
		if (!json_data::requireArray(value["accessOffsets"], 0, r.file, access_pointer, r.error))
			return false;
		if (def.placement == PlacementKind::DoorSocket && !value["accessOffsets"].empty())
			return r.fail(access_pointer, "a doorSocket building uses its door crossings as access (use [])");
		if (def.placement == PlacementKind::Footprint && value["accessOffsets"].empty())
			return r.fail(access_pointer, "a footprint building needs at least one access offset");
		std::set<std::pair<int, int>> seen_offsets;
		for (std::size_t i = 0; i < value["accessOffsets"].size(); ++i)
		{
			const std::string op = childPointer(access_pointer, i);
			Cell offset;
			if (!r.readOffset(value["accessOffsets"][i], offset, op))
				return false;
			if (!footprint::isAdjacentOutside(offset, def.footprint_width, def.footprint_height))
				return r.fail(op, "access offset must be outside the footprint and share an edge with it");
			if (!seen_offsets.insert({ offset.x, offset.z }).second)
				return r.fail(op, "duplicate access offset");
			def.access_offsets.push_back(offset);
		}

		const std::string services_pointer = childPointer(pointer, "services");
		if (!json_data::requireArray(value["services"], 0, r.file, services_pointer, r.error))
			return false;
		for (std::size_t i = 0; i < value["services"].size(); ++i)
		{
			ServiceType service = ServiceType::Eat;
			if (!json_data::readEnum(value["services"][i], service_names, service, r.file, childPointer(services_pointer, i), r.error))
				return false;
			if (def.offersService(service))
				return r.fail(childPointer(services_pointer, i), "duplicate service");
			def.services.push_back(service);
		}
		if (!def.services.empty() && def.placement != PlacementKind::Footprint)
			return r.fail(services_pointer, "services need footprint access cells");

		// Utilities and capacities.
		const std::string power_pointer = childPointer(pointer, "power");
		const std::string water_pointer = childPointer(pointer, "water");
		if (!r.keys(value["power"], { "demand", "output" }, power_pointer) ||
			!r.readFloat(value["power"]["demand"], 0.0f, 1000.0f, def.power_demand, childPointer(power_pointer, "demand")) ||
			!r.readFloat(value["power"]["output"], 0.0f, 1000.0f, def.power_output, childPointer(power_pointer, "output")) ||
			!r.keys(value["water"], { "demand", "output" }, water_pointer) ||
			!r.readFloat(value["water"]["demand"], 0.0f, 1000.0f, def.water_demand, childPointer(water_pointer, "demand")) ||
			!r.readFloat(value["water"]["output"], 0.0f, 1000.0f, def.water_output, childPointer(water_pointer, "output")) ||
			!r.readInt(value["oxygenCapacity"], 0, 10000, def.oxygen_capacity, childPointer(pointer, "oxygenCapacity")) ||
			!r.readInt(value["storageCapacity"], 0, 100000, def.storage_capacity, childPointer(pointer, "storageCapacity")))
			return false;

		if (!value["recipe"].is_null())
		{
			RecipeDefinition recipe;
			if (!parseRecipe(r, value["recipe"], recipe, childPointer(pointer, "recipe")))
				return false;
			if (!def.has_operation)
				return r.fail(childPointer(pointer, "recipe"), "a recipe needs an operation role (someone has to work it)");
			def.recipe = recipe;
		}
		else if (def.has_operation)
			return r.fail(childPointer(pointer, "operation"), "operating workers need a recipe to work on");

		return parseVisuals(r, value["visuals"], def.visuals, childPointer(pointer, "visuals"));
	}
}

// ---------------------------------------------------------------------------- footprint

namespace footprint
{
	int normalizeQuarterTurns(int quarter_turns)
	{
		return ((quarter_turns % 4) + 4) % 4;
	}

	void rotatedSize(int width, int height, int quarter_turns, int& out_width, int& out_height)
	{
		const bool odd = normalizeQuarterTurns(quarter_turns) % 2 == 1;
		out_width = odd ? height : width;
		out_height = odd ? width : height;
	}

	Cell rotateOffset(Cell offset, int width, int height, int quarter_turns)
	{
		switch (normalizeQuarterTurns(quarter_turns))
		{
		case 1: return Cell{ height - 1 - offset.z, offset.x };
		case 2: return Cell{ width - 1 - offset.x, height - 1 - offset.z };
		case 3: return Cell{ offset.z, width - 1 - offset.x };
		default: return offset;
		}
	}

	bool isAdjacentOutside(Cell offset, int width, int height)
	{
		const bool inside_x = offset.x >= 0 && offset.x < width;
		const bool inside_z = offset.z >= 0 && offset.z < height;
		if (inside_x && inside_z)
			return false; // inside the footprint
		// Edge neighbour: one coordinate within the span, the other exactly one step out.
		if (inside_x && (offset.z == -1 || offset.z == height))
			return true;
		if (inside_z && (offset.x == -1 || offset.x == width))
			return true;
		return false;
	}
}

// ---------------------------------------------------------------------------- BuildingCatalog

const char* BuildingCatalog::resourceKey(ResourceType type)
{
	switch (type)
	{
	case ResourceType::IronOre: return "ironOre";
	case ResourceType::CopperOre: return "copperOre";
	case ResourceType::Steel: return "steel";
	case ResourceType::Components: return "components";
	case ResourceType::Vegetables: return "vegetables";
	case ResourceType::Aurite: return "aurite";
	}
	return "";
}

const char* BuildingCatalog::resourceLabel(ResourceType type)
{
	switch (type)
	{
	case ResourceType::IronOre: return "Iron ore";
	case ResourceType::CopperOre: return "Copper ore";
	case ResourceType::Steel: return "Steel";
	case ResourceType::Components: return "Components";
	case ResourceType::Vegetables: return "Vegetables";
	case ResourceType::Aurite: return "Aurite";
	}
	return "";
}

const char* BuildingCatalog::typeId(BuildingType type)
{
	for (const TypeName& name : type_names)
		if (name.type == type)
			return name.id;
	return "";
}

bool BuildingCatalog::typeFromId(const std::string& id, BuildingType& out_type)
{
	for (const TypeName& name : type_names)
		if (id == name.id)
		{
			out_type = name.type;
			return true;
		}
	return false;
}

bool BuildingCatalog::readResourceAmounts(const Json& value, int max_amount, ResourceAmounts& out,
	const std::filesystem::path& file, const std::string& pointer, DataLoadError& error)
{
	if (!value.is_object())
	{
		error = makeError(file, pointer, "expected an object of item amounts");
		return false;
	}
	ResourceAmounts amounts;
	for (auto it = value.begin(); it != value.end(); ++it)
	{
		bool known = false;
		for (std::size_t i = 0; i < resource_type_count; ++i)
		{
			const ResourceType type = static_cast<ResourceType>(i);
			if (it.key() != resourceKey(type))
				continue;
			known = true;
			if (!json_data::readInt(it.value(), amounts[type], 0, max_amount, file, childPointer(pointer, it.key()), error))
				return false;
		}
		if (!known)
		{
			error = makeError(file, childPointer(pointer, it.key()),
				"unknown item (expected ironOre, copperOre, steel, components, vegetables or aurite)");
			return false;
		}
	}
	out = amounts;
	return true;
}

BuildingCatalogLoadResult BuildingCatalog::load(const std::filesystem::path& path)
{
	Json document;
	DataLoadError error;
	if (!json_data::loadJson(path, document, error))
	{
		definitions.clear();
		BuildingCatalogLoadResult result;
		result.error = error;
		return result;
	}
	return parse(document, path);
}

BuildingCatalogLoadResult BuildingCatalog::parse(const Json& document, const std::filesystem::path& path)
{
	definitions.clear();
	source_path = path;
	BuildingCatalogLoadResult result;
	DataLoadError error;
	Reader r(path, error);

	if (!r.keys(document, { "schemaVersion", "buildings" }, "") ||
		!json_data::checkSchemaVersion(document, supported_schema_version, path, error) ||
		!json_data::requireArray(document["buildings"], 1, path, "/buildings", error))
	{
		result.error = error;
		return result;
	}

	std::vector<BuildingDefinition> parsed;
	std::set<std::string> ids;
	for (std::size_t i = 0; i < document["buildings"].size(); ++i)
	{
		const std::string pointer = childPointer("/buildings", i);
		BuildingDefinition def;
		if (!parseDefinition(r, document["buildings"][i], def, pointer))
		{
			result.error = error;
			return result;
		}
		if (!ids.insert(def.id).second)
		{
			result.error = makeError(path, childPointer(pointer, "id"), "duplicate building id '" + def.id + "'");
			return result;
		}
		parsed.push_back(def);
	}
	for (const TypeName& name : type_names)
	{
		if (!ids.count(name.id))
		{
			result.error = makeError(path, "/buildings", std::string("missing building type '") + name.id + "'");
			return result;
		}
	}
	std::sort(parsed.begin(), parsed.end(), [](const BuildingDefinition& a, const BuildingDefinition& b) {
		return static_cast<int>(a.type) < static_cast<int>(b.type);
	});
	definitions.swap(parsed);
	return result;
}

BuildingCatalogLoadResult BuildingCatalog::validateVisuals(const AssetCatalog& assets) const
{
	BuildingCatalogLoadResult result;
	for (const BuildingDefinition& def : definitions)
	{
		for (std::size_t i = 0; i < def.visuals.size(); ++i)
		{
			const BuildingVisualDefinition& visual = def.visuals[i];
			const std::string pointer = "/buildings/" + def.id + "/visuals/" + std::to_string(i);
			if (!assets.findAsset(visual.asset_id))
			{
				result.error = makeError(source_path, pointer + "/assetId", "unknown asset id '" + visual.asset_id + "'");
				return result;
			}
			if (!assets.findMaterial(visual.material_id))
			{
				result.error = makeError(source_path, pointer + "/materialId", "unknown material id '" + visual.material_id + "'");
				return result;
			}
		}
	}
	return result;
}

const BuildingDefinition* BuildingCatalog::find(BuildingType type) const
{
	for (const BuildingDefinition& def : definitions)
		if (def.type == type)
			return &def;
	return nullptr;
}

const BuildingDefinition* BuildingCatalog::findById(const std::string& id) const
{
	for (const BuildingDefinition& def : definitions)
		if (def.id == id)
			return &def;
	return nullptr;
}
