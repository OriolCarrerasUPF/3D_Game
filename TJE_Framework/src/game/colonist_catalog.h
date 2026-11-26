#pragma once

// Colonist role archetypes and the display-name pool from data/arkfall/colonists.json
// (schema 1), headless. Each role has its visual (asset + role tint, GDD 3.1: Engineer
// orange, Miner yellow, Farmer green) and walking speed. P05 adds `names`: a pool of
// unique display names handed out in stable colonist-ID order (plan C9.1). Role/building
// compatibility is NOT here: buildings.json is its only source.

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

private:
	std::filesystem::path source_path;
	std::map<Job, ColonistRoleDefinition> roles;
	std::vector<std::string> names;
};
