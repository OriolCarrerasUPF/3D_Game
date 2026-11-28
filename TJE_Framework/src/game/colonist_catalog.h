#pragma once

// Colonist role archetypes and the display-name pool from data/arkfall/colonists.json
// (schema 1), headless. Each role has its visual (asset + role tint, GDD 3.1: Engineer
// orange, Miner yellow, Farmer green) and walking speed. P05 adds `names`: a pool of
// unique display names handed out in stable colonist-ID order (plan C9.1). P06 adds
// `needs`: the need thresholds, drain and refill rates of plan C8 (no C++ constants).
// Role/building compatibility is NOT here: buildings.json is its only source.

#include "arkfall_types.h"
#include "data_load_error.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

class AssetCatalog;

struct ColonistRoleDefinition
{
	Job job = Job::Engineer;
	std::string asset_id;
	std::string material_id;
	float move_speed = 0.0f; // metres per second
};

// Need tuning (plan C6/C8), read from colonists.json "needs". Rates are need points
// (0-100) per second.
struct NeedRules
{
	float refill_threshold = 20.0f;        // a need at or below this interrupts work
	float hunger_drain = 0.0f;
	float thirst_drain = 0.0f;
	float oxygen_drain = 0.0f;             // suit oxygen outside, or inside without coverage
	float drink_per_second = 0.0f;         // Thirst restored per second while water is allocated
	float water_per_thirst_point = 0.0f;   // water units used per restored Thirst point
	float eat_seconds = 0.0f;              // one meal restores Hunger to 100 after this time
	int vegetables_per_meal = 0;
	float suit_refill_per_second = 0.0f;   // Oxygen restored per second at a suit refill point
	float service_retry_seconds = 0.0f;    // retry interval when no service is available

	float drainFor(NeedType need) const;
};

struct ColonistCatalogLoadResult
{
	std::optional<DataLoadError> error;
	explicit operator bool() const { return !error.has_value(); }
};

class ColonistCatalog
{
public:
	// Exactly one entry per Job is required.
	ColonistCatalogLoadResult load(const std::filesystem::path& path);
	// Checks that every role's asset and material exist in the asset catalog.
	ColonistCatalogLoadResult validateVisuals(const AssetCatalog& assets) const;

	const ColonistRoleDefinition* findRole(Job job) const;
	const std::map<Job, ColonistRoleDefinition>& getRoles() const { return roles; }
	// Unique, non-empty display names in file order.
	const std::vector<std::string>& getNames() const { return names; }
	const NeedRules& getNeedRules() const { return need_rules; }

private:
	std::filesystem::path source_path;
	std::map<Job, ColonistRoleDefinition> roles;
	std::vector<std::string> names;
	NeedRules need_rules;
};
