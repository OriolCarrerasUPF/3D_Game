#pragma once

// Loaded data catalogs shared by the simulation and the views (plan C2), headless.
// Grouping them keeps ColonySimulation::initialize and ColonyWorld stable as new
// catalogs arrive: colonist roles and names (colonists.json) and, from P05, the 13
// building definitions (buildings.json).

#include "building_catalog.h"
#include "colonist_catalog.h"

struct GameDefinitions
{
	ColonistCatalog colonists;
	BuildingCatalog buildings;
};
