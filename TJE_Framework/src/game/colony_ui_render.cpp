#include "colony_ui_render.h"

#include "asset_catalog.h"
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
	quads.add("ui_panel", ui.getHelpBar());

	// Top bar: debug resource/status line (inventory arrives in P05).
	std::string status = "ARKFALL";
	texts.push_back({ 10.0f, centeredTextY(ui.getTopBar(), text_scale), status, text_accent, text_scale });
	const float status_x = 10.0f + measureTextWidth(status, text_scale) + 14.0f;
	std::string line = formatTime(hud.simulation_seconds) + "  " +
		(hud.paused ? std::string("PAUSED") : "x" + std::to_string(hud.speed_level)) +
		"  |  Colonists " + std::to_string(hud.colonists_alive);
	if (hud.has_door)
		line += std::string("  |  Door ") + (hud.door_open ? "open" : "closed");
	line += "  |  " + std::to_string(hud.fps) + " fps";
	const float status_room = ui.getButtonRect(UiButton::Pause).x - status_x - 8.0f;
	texts.push_back({ status_x, centeredTextY(ui.getTopBar(), text_scale), fitText(line, status_room, text_scale),
		hud.paused ? text_bad : text_white, text_scale });

	// Buttons.
	for (int i = 0; i < static_cast<int>(UiButton::Count); ++i)
	{
		const UiButton button = static_cast<UiButton>(i);
		if (!ui.isButtonVisible(button))
			continue;
		const bool active = (button == UiButton::Pause && hud.paused) ||
			(button == UiButton::Build && ui.isBuildMode()) ||
			(button == UiButton::Overlay && hud.debug_overlay);
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
	const std::string help = "WASD pan  Q/E or middle-drag rotate  Wheel zoom  Click select  B build  "
		"Space/-/+ speed  Esc cancel  F3 debug";
	texts.push_back({ 8.0f, centeredTextY(ui.getHelpBar(), small_text_scale),
		fitText(help, width - 16.0f, small_text_scale), text_dim, small_text_scale });

	// ---- colonist popup -----------------------------------------------------------
	if (ui.isPopupVisible())
	{
		const UiRect& popup = ui.getPopupRect();
		quads.add("ui_panel", popup);
		const ColonistRecord* colonist = simulation.findColonist(ui.getSelectedColonist());
		float y = popup.y + 10.0f;
		const float x = popup.x + 12.0f;
		const float inner = popup.w - 24.0f;
		if (colonist)
		{
			texts.push_back({ x, y, "Colonist #" + std::to_string(colonist->id), text_accent, text_scale });
			y += 22.0f;
			texts.push_back({ x, y, std::string("Job: ") + ColonyUI::jobName(colonist->job), text_white, text_scale });
			y += 18.0f;
			texts.push_back({ x, y, std::string("State: ") + ColonyUI::stateName(colonist->state), text_white, text_scale });
			y += 18.0f;
			const NavigationGrid& grid = simulation.getGrid();
			const Cell cell = colonist->last_cell;
			texts.push_back({ x, y, "Cell (" + std::to_string(cell.x) + ", " + std::to_string(cell.z) + ") " +
				(grid.getZone(cell) == Zone::Inside ? "inside" : "outside"), text_white, text_scale });
			y += 18.0f;
			texts.push_back({ x, y, fitText(colonist->status, inner, small_text_scale), text_dim, small_text_scale });
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
			texts.push_back({ x, popup.y + popup.h - 20.0f, "Names and job changes arrive in P05", text_dim, small_text_scale });
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
		ghost += "   |   Building types arrive with buildings.json (P05)";
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
