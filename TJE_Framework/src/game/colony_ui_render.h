#pragma once

// OpenGL drawing of the HUD described by ColonyUI (plan P04, T16).
// Quads use the flat debug shader in an orthographic projection over the logical
// window size; text uses the framework's drawText. All quads are drawn before all
// text because drawText changes the depth/culling state (plan P10 note).

#include "colony_ui.h"

#include <memory>
#include <string>

class AssetCatalog;
class ColonySimulation;
class Mesh;

// Read-only facts shown by the HUD, gathered by Game once per frame.
struct HudInfo
{
	double simulation_seconds = 0.0;
	bool paused = false;
	int speed_level = 1;
	int colonists_alive = 0;
	bool has_door = false;
	bool door_open = false;
	int fps = 0;
	bool debug_overlay = false;
	bool edge_scroll = false;
	std::string gpu_stats;
	std::uint64_t navigation_revision = 0;
	// Build-mode ghost under the pointer.
	bool ghost_visible = false;
	Cell ghost_cell;
	bool ghost_inside = false;
	bool ghost_valid = false;
	std::string ghost_status;
};

class ColonyUIRenderer
{
public:
	explicit ColonyUIRenderer(const AssetCatalog& assets);
	~ColonyUIRenderer();
	ColonyUIRenderer(const ColonyUIRenderer&) = delete;
	ColonyUIRenderer& operator=(const ColonyUIRenderer&) = delete;

	void render(const ColonyUI& ui, const ColonySimulation& simulation, const HudInfo& hud);

private:
	const AssetCatalog* assets;
	std::unique_ptr<Mesh> quad_mesh;
};
