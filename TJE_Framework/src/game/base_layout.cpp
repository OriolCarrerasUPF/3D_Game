#include "base_layout.h"

#include <algorithm>
#include <cmath>

namespace base_layout
{
	std::vector<Piece> wallPieces(const CellSet& corridor, const CellSet& inside, const EdgeSet& open_edges)
	{
		std::vector<Piece> walls;
		for (const CellKey& cell : corridor) // std::set: sorted like the reference script
		{
			for (const WallSide& side : wall_sides)
			{
				const CellKey neighbour{ cell.first + side.dx, cell.second + side.dz };
				if (inside.count(neighbour) || open_edges.count({ cell, neighbour }))
					continue;
				walls.push_back(Piece{ PieceKind::Wall, cell.first + 0.5f, cell.second + 0.5f, side.yaw_degrees });
			}
		}
		return walls;
	}

	std::vector<Piece> archPieces(const CellSet& corridor, const CellSet& inside)
	{
		std::vector<Piece> arches;
		for (const CellKey& cell : corridor)
		{
			const int x = cell.first, z = cell.second;
			if (!inside.count({ x - 1, z }) && corridor.count({ x + 1, z }) && !inside.count({ x + 2, z }))
				arches.push_back(Piece{ PieceKind::Arch, x + 1.0f, z + 0.5f, 0.0f });
			if (!inside.count({ x, z - 1 }) && corridor.count({ x, z + 1 }) && !inside.count({ x, z + 2 }))
				arches.push_back(Piece{ PieceKind::Arch, x + 0.5f, z + 1.0f, 90.0f });
		}
		return arches;
	}

	namespace
	{
		// The two corridor cells an arch spans.
		void archCells(const Piece& arch, CellKey& a, CellKey& b)
		{
			if (arch.yaw_degrees == 0.0f)
			{
				const int z = static_cast<int>(std::floor(arch.z));
				const int x = static_cast<int>(std::lround(arch.x));
				a = { x - 1, z };
				b = { x, z };
			}
			else
			{
				const int x = static_cast<int>(std::floor(arch.x));
				const int z = static_cast<int>(std::lround(arch.z));
				a = { x, z - 1 };
				b = { x, z };
			}
		}
	}

	std::vector<Piece> junctionPieces(const CellSet& corridor, const CellSet& inside)
	{
		CellSet covered;
		for (const Piece& arch : archPieces(corridor, inside))
		{
			CellKey a, b;
			archCells(arch, a, b);
			covered.insert(a);
			covered.insert(b);
		}
		CellSet used;
		std::vector<Piece> caps;
		for (const CellKey& cell : corridor)
		{
			const int x = cell.first, z = cell.second;
			const CellKey block[4] = { { x, z }, { x + 1, z }, { x, z + 1 }, { x + 1, z + 1 } };
			bool ok = true;
			for (const CellKey& c : block)
				ok = ok && corridor.count(c) && !covered.count(c) && !used.count(c);
			if (!ok)
				continue;
			for (const CellKey& c : block)
				used.insert(c);
			caps.push_back(Piece{ PieceKind::Junction, x + 1.0f, z + 1.0f, 0.0f });
		}
		return caps;
	}

	std::vector<Piece> corridorPieces(const CellSet& corridor, const CellSet& inside, const EdgeSet& open_edges)
	{
		std::vector<Piece> pieces = wallPieces(corridor, inside, open_edges);
		for (const Piece& arch : archPieces(corridor, inside))
			pieces.push_back(arch);
		for (const Piece& cap : junctionPieces(corridor, inside))
			pieces.push_back(cap);
		return pieces;
	}

	void addCorridorOffsets(const std::vector<Piece>& pieces, OffsetMap& out)
	{
		for (const Piece& piece : pieces)
		{
			if (piece.kind == PieceKind::Arch)
			{
				CellKey a, b;
				archCells(piece, a, b);
				for (const CellKey& c : { a, b })
				{
					const float dx = piece.yaw_degrees == 0.0f ? piece.x - (c.first + 0.5f) : 0.0f;
					const float dz = piece.yaw_degrees == 0.0f ? 0.0f : piece.z - (c.second + 0.5f);
					out[c] = { dx, dz };
				}
			}
			else if (piece.kind == PieceKind::Junction)
			{
				const int x0 = static_cast<int>(std::lround(piece.x)) - 1;
				const int z0 = static_cast<int>(std::lround(piece.z)) - 1;
				for (int dz = 0; dz < 2; ++dz)
					for (int dx = 0; dx < 2; ++dx)
					{
						const CellKey c{ x0 + dx, z0 + dz };
						out[c] = { piece.x - (c.first + 0.5f), piece.z - (c.second + 0.5f) };
					}
			}
		}
	}

	void addAirlockOffsets(const std::vector<Cell>& footprint, int quarter_turns, OffsetMap& out)
	{
		if (footprint.empty())
			return;
		int min_x = footprint.front().x, max_x = min_x, min_z = footprint.front().z, max_z = min_z;
		for (Cell c : footprint)
		{
			min_x = std::min(min_x, c.x); max_x = std::max(max_x, c.x);
			min_z = std::min(min_z, c.z); max_z = std::max(max_z, c.z);
		}
		const float centre_x = (min_x + max_x + 1) * 0.5f;
		const float centre_z = (min_z + max_z + 1) * 0.5f;
		// The local X axis (across the door) is world X for even quarter turns, Z for odd.
		const bool across_x = ((quarter_turns % 4) + 4) % 2 == 0;
		for (Cell c : footprint)
		{
			const float dx = across_x ? centre_x - (c.x + 0.5f) : 0.0f;
			const float dz = across_x ? 0.0f : centre_z - (c.z + 0.5f);
			out[key(c)] = { dx, dz };
		}
	}

	std::pair<float, float> offsetAt(const OffsetMap& offsets, Cell cell)
	{
		const auto it = offsets.find(key(cell));
		return it == offsets.end() ? std::pair<float, float>{ 0.0f, 0.0f } : it->second;
	}
}
