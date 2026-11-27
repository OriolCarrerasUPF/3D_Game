#pragma once

// Corridor view rules of the base layout (plan C5.2), headless.
//
// The simulation only knows which cells are corridors and which are inside; this file
// turns that into the pieces the view draws and the sideways offsets that put walking
// colonists on the passage axis. tools/base_layout_rules.py is the reference
// implementation (used by the Blender preview); both must give the same pieces.
//
// * Walls: one per edge between a corridor cell and a cell that is not inside, unless
//   the edge is open (a dome entrance). The wall asset is authored on the local -X face
//   of its cell and turned with the scene generator's yaws.
// * Arches: one full 2 m arch per row where the cross-section through two neighbouring
//   corridor cells is exactly "not inside | corridor | corridor | not inside".
// * Junctions: one cap per 2 x 2 block of corridor cells that no arch covers (L corner,
//   T or cross), chosen in cell order without overlapping.
// * Passage axis: colonists move between cell centres, 0.5 m off the axis of a 2-cell
//   passage, so the view draws them shifted by the offsets below (visual only).
//
// Coordinates are cell units: cell (x, z) spans x..x+1 and z..z+1. The view converts
// them with the grid origin and cell size.

#include "arkfall_types.h"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace base_layout
{
	using CellKey = std::pair<int, int>;
	using CellSet = std::set<CellKey>;
	// Edge from a corridor cell to a neighbour that stays open (no wall).
	using EdgeSet = std::set<std::pair<CellKey, CellKey>>;
	// Visual offset per cell, in cell units along x and z.
	using OffsetMap = std::map<CellKey, std::pair<float, float>>;

	inline CellKey key(Cell cell) { return { cell.x, cell.z }; }

	// Asset ids of the generated pieces (team-authored Blender models, plan C5.2).
	constexpr const char* wall_asset_id = "corridor_wall_frame";
	constexpr const char* arch_asset_id = "corridor_arch";
	constexpr const char* junction_asset_id = "corridor_junction";
	constexpr const char* piece_material_id = "architecture";

	enum class PieceKind : std::uint8_t { Wall, Arch, Junction };

	struct Piece
	{
		PieceKind kind = PieceKind::Wall;
		float x = 0.0f;            // placement point in cell units
		float z = 0.0f;
		float yaw_degrees = 0.0f;  // world_transform convention (+90 maps +X to +Z)
	};

	// Wall yaw for each neighbour direction, in the order the reference script uses:
	// west 0, east 180, south 90, north -90.
	struct WallSide { int dx; int dz; float yaw_degrees; };
	constexpr WallSide wall_sides[4] = { { -1, 0, 0.0f }, { 1, 0, 180.0f }, { 0, -1, 90.0f }, { 0, 1, -90.0f } };

	std::vector<Piece> wallPieces(const CellSet& corridor, const CellSet& inside, const EdgeSet& open_edges);
	std::vector<Piece> archPieces(const CellSet& corridor, const CellSet& inside);
	std::vector<Piece> junctionPieces(const CellSet& corridor, const CellSet& inside);
	// Walls, then arches, then junctions.
	std::vector<Piece> corridorPieces(const CellSet& corridor, const CellSet& inside, const EdgeSet& open_edges);

	// Offsets for corridor cells covered by an arch (towards the arch centre) or by a
	// junction cap (towards the block centre).
	void addCorridorOffsets(const std::vector<Piece>& pieces, OffsetMap& out);
	// Offsets for the cells of a 2-wide airlock footprint: towards its centre line across
	// the door (the footprint's local X axis after `quarter_turns`).
	void addAirlockOffsets(const std::vector<Cell>& footprint, int quarter_turns, OffsetMap& out);
	// Offset of one cell, (0, 0) when it has none.
	std::pair<float, float> offsetAt(const OffsetMap& offsets, Cell cell);
}
