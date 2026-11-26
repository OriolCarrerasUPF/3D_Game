#pragma once

// Building definitions from data/arkfall/buildings.json (schema 1), headless.
//
// buildings.json is the ONLY source of building values (plan C8/F3): footprint, cost,
// construction and operating roles/slots, access offsets, services, utilities,
// storage, recipe and visual composition. C++ only names the 13 types. The loader is
// strict: every key is required (absent features are explicit null / 0 / []), unknown
// keys, enums, resource names, roles and deposits are errors, and each of the 13
// types must appear exactly once. Errors report file + JSON pointer + message.
//
// footprint:: holds the single quarter-turn transform shared by placement validation,
// the placement preview (P07) and the building views (plan C5).

#include "arkfall_types.h"
#include "data_load_error.h"
#include "json_data.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class AssetCatalog;

enum class BuildingZone : std::uint8_t { Interior, Exterior, Boundary };
enum class PlacementKind : std::uint8_t { Footprint, DoorSocket };
enum class ServiceType : std::uint8_t { Eat, Drink };

struct RecipeDefinition
{
	ResourceAmounts inputs;
	ResourceAmounts outputs;
	float seconds = 0.0f;              // one cycle
	bool per_present_worker = false;   // true: each present worker adds one cycle's worth of rate (mines)
};

struct BuildingVisualDefinition
{
	std::string asset_id;
	std::string material_id;
	Vector3 offset;          // metres from the footprint centre at 0 deg (rotates with the building)
	float yaw_degrees = 0.0f;
	float scale = 1.0f;      // multiplier on the asset's own scale
};

struct BuildingDefinition
{
	BuildingType type = BuildingType::Storage;
	std::string id;                    // JSON id, e.g. "iron_mine"
	std::string label;                 // UI name, e.g. "Iron Mine"
	BuildingZone zone = BuildingZone::Interior;
	PlacementKind placement = PlacementKind::Footprint;
	int footprint_width = 0;           // cells at 0 deg (0 for door sockets)
	int footprint_height = 0;
	std::optional<DepositType> required_deposit;
	ResourceAmounts cost;

	Job construction_role = Job::Engineer;
	int construction_slots = 0;
	float construction_seconds = 0.0f;

	bool has_operation = false;        // false: no operating worker (Storage, Solar Panel, ...)
	Job operation_role = Job::Miner;
	int operation_slots = 0;

	std::vector<Cell> access_offsets;  // relative to the footprint origin at 0 deg
	std::vector<ServiceType> services;
	float power_demand = 0.0f;
	float power_output = 0.0f;
	float water_demand = 0.0f;         // units per second
	float water_output = 0.0f;
	int oxygen_capacity = 0;           // indoor colonists covered
	int storage_capacity = 0;          // item units added when complete
	std::optional<RecipeDefinition> recipe;
	std::vector<BuildingVisualDefinition> visuals;

	bool offersService(ServiceType service) const
	{
		for (ServiceType offered : services)
			if (offered == service)
				return true;
		return false;
	}
};

struct BuildingCatalogLoadResult
{
	std::optional<DataLoadError> error;
	explicit operator bool() const { return !error.has_value(); }
};

class BuildingCatalog
{
public:
	static constexpr int supported_schema_version = 1;
	static constexpr int max_footprint_size = 8;

	BuildingCatalogLoadResult load(const std::filesystem::path& path);
	// Parses an already loaded document; `path` only labels errors (used by tests).
	BuildingCatalogLoadResult parse(const json_data::Json& document, const std::filesystem::path& path);
	// Every visual asset and material must exist in the asset catalog.
	BuildingCatalogLoadResult validateVisuals(const AssetCatalog& assets) const;

	const BuildingDefinition* find(BuildingType type) const;
	const BuildingDefinition* findById(const std::string& id) const;
	const std::vector<BuildingDefinition>& getDefinitions() const { return definitions; }
	bool empty() const { return definitions.empty(); }

	// JSON names shared with scenario files and the UI.
	static const char* resourceKey(ResourceType type);     // "ironOre"
	static const char* resourceLabel(ResourceType type);   // "Iron ore"
	static const char* typeId(BuildingType type);          // "iron_mine"
	static bool typeFromId(const std::string& id, BuildingType& out_type);
	// Reads {"steel": 5, ...}: known item keys only, integers in [0, max_amount].
	static bool readResourceAmounts(const json_data::Json& value, int max_amount, ResourceAmounts& out,
		const std::filesystem::path& file, const std::string& pointer, DataLoadError& error);

private:
	std::filesystem::path source_path;
	std::vector<BuildingDefinition> definitions;   // ordered by BuildingType
};

namespace footprint
{
	// Quarter turns normalised to 0..3 (negative values wrap).
	int normalizeQuarterTurns(int quarter_turns);

	// Size after rotation: odd quarter turns swap width and height.
	void rotatedSize(int width, int height, int quarter_turns, int& out_width, int& out_height);

	// Plan C5 transform of an offset (dx, dz) given in the 0 deg frame of a w x h footprint:
	// q=1 -> (h-1-dz, dx), q=2 -> (w-1-dx, h-1-dz), q=3 -> (dz, w-1-dx).
	// Works for footprint cells and for access offsets outside the footprint.
	Cell rotateOffset(Cell offset, int width, int height, int quarter_turns);

	// True when `offset` (0 deg frame) is outside the w x h footprint and shares an edge with it.
	bool isAdjacentOutside(Cell offset, int width, int height);
}
