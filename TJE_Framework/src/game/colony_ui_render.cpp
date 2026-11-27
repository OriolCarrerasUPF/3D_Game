#include "colony_ui_render.h"

#include "asset_catalog.h"
#include "building_catalog.h"
#include "colony_simulation.h"

#include "framework/utils.h"
#include "graphics/mesh.h"
#include "graphics/shader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

namespace
{
	constexpr float text_scale = 1.5f;
	constexpr float small_text_scale = 1.25f;
	constexpr float glyph_height = 7.0f; // stb_easy_font cap height at scale 1

	const Vector3 text_white(0.95f, 0.96f, 0.98f);
	const Vector3 text_dim(0.68f, 0.72f, 0.78f);
	const Vector3 text_accent(1.0f, 0.72f, 0.35f);
	const Vector3 text_good(0.45f, 0.95f, 0.55f);
	const Vector3 text_bad(1.0f, 0.45f, 0.4f);

	struct TextItem
	{
		float x;
		float y;
		std::string text;
		Vector3 color;
		float scale;
	};

	// Quads grouped by material, drawn in insertion order of the groups.
	class QuadBatch
	{
	public:
		void add(const std::string& material, const UiRect& rect)
		{
			if (rect.w <= 0.0f || rect.h <= 0.0f)
				return;
			if (!groups.count(material))
				order.push_back(material);
			std::vector<Vector3>& v = groups[material];
			const Vector3 a(rect.x, rect.y, 0.0f), b(rect.x + rect.w, rect.y, 0.0f);
			const Vector3 c(rect.x + rect.w, rect.y + rect.h, 0.0f), d(rect.x, rect.y + rect.h, 0.0f);
			v.insert(v.end(), { a, b, c, a, c, d });
		}
		std::vector<std::string> order;
		std::map<std::string, std::vector<Vector3>> groups;
	};

	std::string formatTime(double seconds)
	{
		const long long total = static_cast<long long>(seconds);
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld", total / 60, total % 60);
		return buffer;
	}

	// Cuts text so it fits `max_width` pixels at `scale`.
	std::string fitText(const std::string& text, float max_width, float scale)
	{
		if (measureTextWidth(text, scale) <= max_width)
			return text;
		std::string cut = text;
		while (!cut.empty() && measureTextWidth(cut + "...", scale) > max_width)
			cut.pop_back();
		return cut + "...";
	}

	// Greedy word wrap into at most max_lines lines; the last line is cut with "..." if needed.
	std::vector<std::string> wrapText(const std::string& text, float max_width, float scale, std::size_t max_lines)
	{
		std::vector<std::string> lines;
		std::string line;
		std::size_t start = 0;
		while (start < text.size())
		{
			std::size_t end = text.find(' ', start);
			if (end == std::string::npos)
				end = text.size();
			const std::string word = text.substr(start, end - start);
			const std::string candidate = line.empty() ? word : line + " " + word;
			if (!line.empty() && measureTextWidth(candidate, scale) > max_width)
			{
				lines.push_back(line);
				line = word;
			}
			else
				line = candidate;
			start = end + 1;
		}
		if (!line.empty())
			lines.push_back(line);
		if (lines.size() > max_lines)
		{
			lines.resize(max_lines);
			lines.back() += " ...";
		}
		for (std::string& l : lines)
			l = fitText(l, max_width, scale);
		return lines;
	}

	float centeredTextY(const UiRect& rect, float scale)
	{
		return rect.y + (rect.h - glyph_height * scale) * 0.5f;
	}
}

ColonyUIRenderer::ColonyUIRenderer(const AssetCatalog& assets)
	: assets(&assets)
	, quad_mesh(std::make_unique<Mesh>())
{
}

ColonyUIRenderer::~ColonyUIRenderer() = default;

void ColonyUIRenderer::render(const ColonyUI& ui, const ColonySimulation& simulation, const HudInfo& hud)
{
	Shader* shader = assets->getDebugShader();
	if (!shader || !shader->compiled)
		return;

	const float width = ui.getWidth();
	const float height = ui.getHeight();
	QuadBatch quads;
	std::vector<TextItem> texts;

	// ---- panels -------------------------------------------------------------------
	quads.add("ui_panel", ui.getTopBar());
	quads.add("ui_panel", ui.getResourceBar());
	quads.add("ui_panel", ui.getHelpBar());

	// Top bar: debug resource/status line (inventory arrives in P05).
	std::string status = "ARKFALL";
	texts.push_back({ 10.0f, centeredTextY(ui.getTopBar(), text_scale), status, text_accent, text_scale });
	const float status_x = 10.0f + measureTextWidth(status, text_scale) + 14.0f;
	// FPS goes first so fitText never cuts it off on narrow windows ("58/60 fps" when capped).
	std::string line = std::to_string(hud.fps) +
		(hud.max_fps > 0 ? "/" + std::to_string(hud.max_fps) : std::string()) + " fps  |  " +
		formatTime(hud.simulation_seconds) + "  " +
		(hud.paused ? std::string("PAUSED") : "x" + std::to_string(hud.speed_level)) +
		"  |  Colonists " + std::to_string(hud.colonists_alive);
	if (hud.has_door)
		line += std::string("  |  Door ") + (hud.door_open ? "open" : "closed");
	const float status_room = ui.getButtonRect(UiButton::Pause).x - status_x - 8.0f;
	texts.push_back({ status_x, centeredTextY(ui.getTopBar(), text_scale), fitText(line, status_room, text_scale),
		hud.paused ? text_bad : text_white, text_scale });

	// Resource bar (P05): the six stored items and the shared item capacity.
	{
		const Inventory& inventory = simulation.getInventory();
		std::string items;
		for (std::size_t i = 0; i < resource_type_count; ++i)
		{
			const ResourceType type = static_cast<ResourceType>(i);
			items += (items.empty() ? "" : "  ") + std::string(BuildingCatalog::resourceLabel(type)) + " " + std::to_string(inventory.get(type));
		}
		const std::string capacity = "Items " + std::to_string(inventory.getTotal()) + "/" + std::to_string(inventory.getCapacity()) +
			(simulation.usesCargoCapacity() ? " (cargo)" : "");
		const UiRect& bar = ui.getResourceBar();
		const float capacity_width = measureTextWidth(capacity, small_text_scale);
		const bool full = inventory.getTotal() >= inventory.getCapacity();
		texts.push_back({ 10.0f, centeredTextY(bar, small_text_scale),
			fitText(items, width - capacity_width - 34.0f, small_text_scale), text_white, small_text_scale });
		texts.push_back({ width - capacity_width - 10.0f, centeredTextY(bar, small_text_scale), capacity,
			full ? text_bad : text_accent, small_text_scale });
	}

	// The selected colonist's role button is drawn as active.
	const ColonistRecord* selected_colonist = simulation.findColonist(ui.getSelectedColonist());
	const UiButton current_job_button = selected_colonist ? ColonyUI::jobButton(selected_colonist->job) : UiButton::None;

	// Buttons.
	for (int i = 0; i < static_cast<int>(UiButton::Count); ++i)
	{
		const UiButton button = static_cast<UiButton>(i);
		if (!ui.isButtonVisible(button))
			continue;
		const bool active = (button == UiButton::Pause && hud.paused) ||
			(button == UiButton::Build && ui.isBuildMode()) ||
			(button == UiButton::Overlay && hud.debug_overlay) ||
			(button != UiButton::None && button == current_job_button);
		const bool hot = ui.getHoveredButton() == button || ui.getPressedButton() == button;
		const UiRect& rect = ui.getButtonRect(button);
		if (button != UiButton::ClosePopup)
			quads.add(active ? "ui_button_active" : hot ? "ui_button_hover" : "ui_button", rect);
		const std::string label = button == UiButton::Pause && hud.paused ? "Resume" : ColonyUI::buttonLabel(button);
		const float label_width = measureTextWidth(label, text_scale);
		texts.push_back({ rect.x + (rect.w - label_width) * 0.5f, centeredTextY(rect, text_scale), label,
			hot ? text_accent : text_white, text_scale });
	}

	// Help line.
	const std::string help = "WASD pan  Q/E or middle-drag rotate  Wheel zoom  Click colonist/building  B build  "
		"Space/-/+ speed  Esc cancel  F3 debug";
	texts.push_back({ 8.0f, centeredTextY(ui.getHelpBar(), small_text_scale),
		fitText(help, width - 16.0f, small_text_scale), text_dim, small_text_scale });

	// ---- colonist / building popup --------------------------------------------------
	if (ui.isPopupVisible())
	{
		const UiRect& popup = ui.getPopupRect();
		quads.add("ui_panel", popup);
		float y = popup.y + 10.0f;
		const float x = popup.x + 12.0f;
		const float inner = popup.w - 24.0f;
		const float line_room = inner - 26.0f; // keep clear of the close button on the title line
		const BuildingRecord* building = simulation.findBuilding(ui.getSelectedBuilding());
		if (selected_colonist)
		{
			const ColonistRecord* colonist = selected_colonist;
			texts.push_back({ x, y, fitText(colonist->name + "  #" + std::to_string(colonist->id), line_room, text_scale), text_accent, text_scale });
			y += 22.0f;
			texts.push_back({ x, y, std::string("Job: ") + ColonyUI::jobName(colonist->job), text_white, text_scale });
			y += 18.0f;
			texts.push_back({ x, y, std::string("State: ") + ColonyUI::stateName(colonist->state), text_white, text_scale });
			y += 18.0f;
			// Current task / why it is idle (P05), wrapped so the reason is never cut off.
			for (const std::string& line : wrapText("Task: " + colonist->status, inner, small_text_scale, 2))
			{
				texts.push_back({ x, y, line, text_dim, small_text_scale });
				y += 13.0f;
			}
			y += 3.0f;
			const NavigationGrid& grid = simulation.getGrid();
			const Cell cell = colonist->last_cell;
			// P05B: a colonist inside a dome is hidden in the world; say where it is.
			const std::string where = colonist->current_dome != INVALID_BUILDING
				? "Inside " + simulation.describeBuilding(colonist->current_dome)
				: std::string(grid.getZone(cell) == Zone::Inside ? "In the base" : "Outside");
			texts.push_back({ x, y, fitText(where + "  -  cell (" + std::to_string(cell.x) + ", " + std::to_string(cell.z) + ")",
				inner, small_text_scale), colonist->current_dome != INVALID_BUILDING ? text_accent : text_dim, small_text_scale });
			y += 22.0f;

			const struct { const char* label; float value; const char* material; } needs[] = {
				{ "Hunger", colonist->needs.hunger, "ui_need_hunger" },
				{ "Thirst", colonist->needs.thirst, "ui_need_thirst" },
				{ "Oxygen", colonist->needs.oxygen, "ui_need_oxygen" } };
			const float bar_x = x + 70.0f;
			const float bar_w = inner - 70.0f - 40.0f;
			for (const auto& need : needs)
			{
				texts.push_back({ x, y + 1.0f, need.label, text_white, text_scale });
				const UiRect back{ bar_x, y, bar_w, 12.0f };
				quads.add("ui_bar_back", back);
				const float fill = std::clamp(need.value / 100.0f, 0.0f, 1.0f);
				quads.add(need.material, UiRect{ bar_x, y, bar_w * fill, 12.0f });
				char value[16];
				std::snprintf(value, sizeof(value), "%d", static_cast<int>(std::lround(need.value)));
				texts.push_back({ bar_x + bar_w + 8.0f, y + 1.0f, value, need.value <= 20.0f ? text_bad : text_white, text_scale });
				y += 22.0f;
			}
			const float roles_y = ui.getButtonRect(UiButton::JobEngineer).y - 16.0f;
			texts.push_back({ x, roles_y, "Role (frees the current work slot):", text_dim, small_text_scale });
		}
		else if (building)
		{
			const BuildingDefinition* def = simulation.findDefinition(building->type);
			const std::string label = def ? def->label : std::string("Building");
			texts.push_back({ x, y, fitText(label + "  #" + std::to_string(building->id), line_room, text_scale), text_accent, text_scale });
			y += 22.0f;
			const std::string where = std::to_string(building->width) + "x" + std::to_string(building->height) + " at (" +
				std::to_string(building->origin.x) + ", " + std::to_string(building->origin.z) + "), " +
				std::to_string(building->quarter_turns * 90) + " deg";
			texts.push_back({ x, y, fitText(std::string(building->isComplete() ? "Complete" : "Blueprint") + ", " + where, inner, small_text_scale),
				text_dim, small_text_scale });
			y += 20.0f;

			if (def && def->has_operation)
			{
				const int present = simulation.countPresentWorkers(*building);
				const int reserved = simulation.countReservedWorkers(*building);
				texts.push_back({ x, y, std::to_string(building->slots.size()) + " " + ColonyUI::jobName(def->operation_role) +
					" slot" + (building->slots.size() == 1 ? "" : "s") + ":", text_white, text_scale });
				y += 16.0f;
				texts.push_back({ x, y, std::to_string(present) + " present, " + std::to_string(reserved) + " reserved",
					present > 0 ? text_good : text_white, small_text_scale });
				y += 15.0f;
				for (std::size_t slot = 0; slot < building->slots.size(); ++slot)
				{
					const ColonistRecord* owner = simulation.findColonist(building->slots[slot]);
					std::string text = "  slot " + std::to_string(slot + 1) + ": ";
					if (!owner)
						text += "free";
					else
						text += owner->name + (owner->state == ColonistState::Working ? " (working)" : " (on the way)");
					texts.push_back({ x, y, fitText(text, inner, small_text_scale), owner ? text_white : text_dim, small_text_scale });
					y += 14.0f;
				}
				y += 4.0f;
			}
			else
			{
				texts.push_back({ x, y, "No operating workers", text_dim, text_scale });
				y += 18.0f;
			}

			const bool running = building->status == BuildingStatus::Running || building->status == BuildingStatus::Idle;
			for (const std::string& line : wrapText("Status: " + building->status_text, inner, text_scale, 2))
			{
				texts.push_back({ x, y, line, running ? text_good : text_bad, text_scale });
				y += 16.0f;
			}
			y += 4.0f;
			if (def && def->recipe)
			{
				const RecipeDefinition& recipe = *def->recipe;
				auto list = [](const ResourceAmounts& amounts) {
					std::string out;
					for (std::size_t i = 0; i < resource_type_count; ++i)
					{
						const ResourceType type = static_cast<ResourceType>(i);
						if (amounts[type] > 0)
							out += (out.empty() ? "" : " + ") + std::to_string(amounts[type]) + " " + BuildingCatalog::resourceLabel(type);
					}
					return out.empty() ? std::string("nothing") : out;
				};
				char seconds[16];
				std::snprintf(seconds, sizeof(seconds), "%.0f", recipe.seconds);
				const std::string inputs = recipe.inputs.empty() ? std::string() : list(recipe.inputs) + " -> ";
				for (const std::string& line : wrapText("Recipe: " + inputs + list(recipe.outputs) + " every " + seconds + " s" +
						(recipe.per_present_worker ? " per present worker" : ""), inner, small_text_scale, 2))
				{
					texts.push_back({ x, y, line, text_dim, small_text_scale });
					y += 13.0f;
				}
				y += 5.0f;
				const float bar_w = inner - 60.0f;
				quads.add("ui_bar_back", UiRect{ x, y, bar_w, 10.0f });
				quads.add("ui_progress", UiRect{ x, y, bar_w * std::clamp(building->production_progress / recipe.seconds, 0.0f, 1.0f), 10.0f });
				texts.push_back({ x + bar_w + 8.0f, y, std::to_string(building->cycles_completed) + " made", text_white, small_text_scale });
				y += 18.0f;
			}
			if (def && def->storage_capacity > 0)
			{
				for (const std::string& line : wrapText("Adds " + std::to_string(def->storage_capacity) +
						" item capacity; eat/drink point from P06", inner, small_text_scale, 2))
				{
					texts.push_back({ x, y, line, text_dim, small_text_scale });
					y += 13.0f;
				}
			}

			// P05B: who is inside the dome (hidden in the world); each row opens that colonist.
			if (def && def->isDome())
			{
				const std::vector<ColonistId>& inside = ui.getOccupants();
				const UiRect first_row = ui.getOccupantRowRect(0);
				texts.push_back({ x, first_row.y - 16.0f, "Inside (" + std::to_string(inside.size()) + "):", text_white, text_scale });
				if (inside.empty())
					texts.push_back({ x, first_row.y + 2.0f, "nobody", text_dim, small_text_scale });
				for (int row = 0; row < ui.getVisibleOccupantRows(); ++row)
				{
					const ColonistRecord* occupant = simulation.findColonist(inside[static_cast<std::size_t>(row)]);
					if (!occupant)
						continue;
					const UiRect rect = ui.getOccupantRowRect(row);
					const bool hot = ui.getHoveredOccupantRow() == row;
					if (hot)
						quads.add("ui_button_hover", rect);
					std::string text = occupant->name + " (" + ColonyUI::jobName(occupant->job) + ") - " + ColonyUI::stateName(occupant->state);
					if (row == ColonyUI::max_occupant_rows - 1 && inside.size() > static_cast<std::size_t>(ColonyUI::max_occupant_rows))
						text += "  +" + std::to_string(inside.size() - ColonyUI::max_occupant_rows) + " more";
					texts.push_back({ rect.x + 4.0f, rect.y + 2.0f, fitText(text, rect.w - 8.0f, small_text_scale),
						hot ? text_accent : text_white, small_text_scale });
				}
			}
		}
	}

	// ---- build mode panel ---------------------------------------------------------
	if (ui.isBuildMode())
	{
		const UiRect& panel = ui.getBuildPanelRect();
		quads.add("ui_panel", panel);
		const char* rotations[] = { "0", "90", "180", "270" };
		texts.push_back({ panel.x + 12.0f, panel.y + 10.0f,
			fitText(std::string("BUILD MODE  -  R rotate (") + rotations[ui.getGhostQuarterTurns()] +
				" deg)  -  Right-click/Esc cancel", panel.w - 24.0f, text_scale), text_accent, text_scale });
		std::string ghost = "Point at the ground";
		Vector3 ghost_color = text_dim;
		if (hud.ghost_visible)
		{
			ghost = "Cell (" + std::to_string(hud.ghost_cell.x) + ", " + std::to_string(hud.ghost_cell.z) + ") " +
				(hud.ghost_inside ? "inside" : "outside") + ": " + hud.ghost_status;
			ghost_color = hud.ghost_valid ? text_good : text_bad;
		}
		ghost += "   |   Placing buildings arrives in P07";
		texts.push_back({ panel.x + 12.0f, panel.y + 34.0f, fitText(ghost, panel.w - 24.0f, small_text_scale),
			ghost_color, small_text_scale });
	}

	// ---- toast message and debug lines --------------------------------------------
	if (ui.hasMessage())
	{
		const float message_width = std::min(measureTextWidth(ui.getMessage(), text_scale), width - 40.0f);
		const UiRect box{ (width - message_width) * 0.5f - 10.0f, ColonyUI::top_bar_height + 8.0f, message_width + 20.0f, 24.0f };
		quads.add("ui_panel", box);
		texts.push_back({ box.x + 10.0f, centeredTextY(box, text_scale), fitText(ui.getMessage(), message_width, text_scale),
			text_white, text_scale });
	}
	if (hud.debug_overlay)
	{
		const float y = height - ColonyUI::help_bar_height - 20.0f;
		texts.push_back({ 8.0f, y, fitText(hud.gpu_stats, width - 16.0f, small_text_scale), text_white, small_text_scale });
		texts.push_back({ 8.0f, y - 16.0f, fitText("Nav revision " + std::to_string(hud.navigation_revision) +
			"  |  F4 obstacle  F6 clear  F7 door  F8 edge scroll: " + (hud.edge_scroll ? "on" : "off"),
			width - 16.0f, small_text_scale), text_dim, small_text_scale });
	}

	// ---- draw: all quads, then all text ------------------------------------------------
	Matrix44 projection;
	projection.ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	shader->enable();
	shader->setUniform("u_viewprojection", projection);
	shader->setUniform("u_model", Matrix44());
	for (const std::string& material_id : quads.order)
	{
		const SemanticMaterial* material = assets->findMaterial(material_id);
		shader->setUniform("u_color", material ? material->tint : Vector4(1.0f, 0.0f, 1.0f, 1.0f));
		quad_mesh->vertices = quads.groups[material_id];
		quad_mesh->render(GL_TRIANGLES);
	}
	shader->disable();
	glDisable(GL_BLEND);

	for (const TextItem& item : texts)
		drawText(item.x, item.y, item.text, item.color, item.scale);

	// drawText leaves depth test and culling enabled; the scene pass sets its own state.
	glEnable(GL_DEPTH_TEST);
}