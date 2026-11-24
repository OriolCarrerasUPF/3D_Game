#pragma once

// Colonist role archetypes from data/arkfall/colonists.json (schema 1), headless.
// P03 needs the visual (asset + role tint, GDD 3.1: Engineer orange, Miner yellow,
// Farmer green) and the walking speed. P05 extends this file with the name pool and
// role capabilities (plan C9.1) without changing the role list contract.

#include "arkfall_types.h"
#include "data_load_error.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

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

private:
	std::filesystem::path source_path;
	std::map<Job, ColonistRoleDefinition> roles;
};
