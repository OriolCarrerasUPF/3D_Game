#pragma once

// Loaded data catalogs shared by the simulation and the views (plan C2), headless.
// Grouping them keeps ColonySimulation::initialize and ColonyWorld stable as new
// catalogs arrive: P05 adds `BuildingCatalog buildings` here.

#include "colonist_catalog.h"

struct GameDefinitions
{
	ColonistCatalog colonists;
};
