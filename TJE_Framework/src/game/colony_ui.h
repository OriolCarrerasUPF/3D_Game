#pragma once

// HUD state, layout and pointer hit testing (plan P04, T16), headless.
//
// ColonyUI owns what the player is doing with the interface: the selected colonist or
// building (P05), build mode and its ghost rotation, the pressed button and the toast
// message. It
// receives pointer events BEFORE the world (UI first), says whether it consumed them
// and returns a UiAction for Game to execute. It never touches the simulation:
// gameplay changes go through GameCommand. Drawing lives in ColonyUIRenderer
// (colony_ui_render.cpp) so this class is testable without OpenGL.
//
// Coordinates are logical window pixels (SDL window size, origin top-left), the same
// space as SDL mouse events. The renderer maps them to the drawable size.

#include "arkfall_types.h"

#include <cstdint>
#include <string>
#include <vector>

struct UiRect
{
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;

	bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

enum class UiAction : std::uint8_t
{
	None,
	TogglePause,
	SlowDown,
	SpeedUp,
	ToggleBuildMode,
	ToggleOverlay,
	CloseSelection,
	AssignEngineer,   // P05 colonist popup: role buttons (Game queues GameCommand::assignJob)
	AssignMiner,
	AssignFarmer,
	SelectOccupant    // P05B dome popup: a row of the occupant list (getClickedOccupant)
};

enum class PointerType : std::uint8_t { Move, Down, Up };
enum class PointerButton : std::uint8_t { None, Left, Middle, Right };

struct PointerEvent
{
	PointerType type = PointerType::Move;
	PointerButton button = PointerButton::None;
	float x = 0.0f;
	float y = 0.0f;
};

struct UiPointerResult
{
	bool consumed = false;   // true: the world must ignore this event
	UiAction action = UiAction::None;
};

enum class UiButton : std::uint8_t
{
	Pause, SlowDown, SpeedUp, Build, Overlay, ClosePopup,
	JobEngineer, JobMiner, JobFarmer,   // colonist popup only
	Count, None
};

class ColonyUI
{
public:
	static constexpr float top_bar_height = 34.0f;
	static constexpr float resource_bar_height = 22.0f;   // P05 inventory line under the top bar
	static constexpr float help_bar_height = 24.0f;

	ColonyUI();

	// Recomputes every rectangle for the logical window size (call on resize).
	void layout(float width, float height);
	float getWidth() const { return width; }
	float getHeight() const { return height; }

	// UI-first input. Left button: a press on a button captures it and the action fires
	// only if the release happens on the same button; presses on panels are consumed;
	// a world press released over the UI is swallowed. Right/middle presses over the UI
	// are consumed so they never reach the camera or the world.
	UiPointerResult handlePointer(const PointerEvent& event);
	bool isPointerOverUi(float x, float y) const;

	// Selection (IDs only; the popup reads the record from the simulation). A colonist and
	// a building are never selected at the same time.
	void select(ColonistId id) { selected_colonist = id; selected_building = INVALID_BUILDING; occupants.clear(); }
	void selectBuilding(BuildingId id) { selected_building = id; selected_colonist = INVALID_COLONIST; }
	void clearSelection() { selected_colonist = INVALID_COLONIST; selected_building = INVALID_BUILDING; occupants.clear(); }

	// P05B: colonists inside the selected dome (Game refreshes this every frame). The
	// first max_occupant_rows are clickable rows at the bottom of the building popup; a
	// click returns UiAction::SelectOccupant and getClickedOccupant() names the colonist.
	static constexpr int max_occupant_rows = 4;
	static constexpr float occupant_row_height = 16.0f;
	void setOccupants(const std::vector<ColonistId>& ids);
	const std::vector<ColonistId>& getOccupants() const { return occupants; }
	int getVisibleOccupantRows() const;
	UiRect getOccupantRowRect(int row) const;
	int getHoveredOccupantRow() const { return hovered_row; }
	ColonistId getClickedOccupant() const { return clicked_occupant; }
	ColonistId getSelectedColonist() const { return selected_colonist; }
	BuildingId getSelectedBuilding() const { return selected_building; }

	// Build mode shell: ghost rotation in quarter turns (R key). Placement is P07.
	void setBuildMode(bool enabled);
	bool isBuildMode() const { return build_mode; }
	void rotateGhost() { ghost_quarter_turns = (ghost_quarter_turns + 1) % 4; }
	int getGhostQuarterTurns() const { return ghost_quarter_turns; }

	// Escape / right click: closes the innermost open thing (build mode, then the popup).
	enum class CancelResult : std::uint8_t { Nothing, BuildMode, Selection };
	CancelResult cancel();

	// Short feedback line (command results, hints). update() counts real time down.
	void showMessage(const std::string& text, float seconds = 4.0f);
	void update(float real_dt);
	const std::string& getMessage() const { return message; }
	bool hasMessage() const { return message_seconds > 0.0f && !message.empty(); }

	// Layout queries used by the renderer and the tests.
	const UiRect& getTopBar() const { return top_bar; }
	const UiRect& getResourceBar() const { return resource_bar; }
	const UiRect& getHelpBar() const { return help_bar; }
	const UiRect& getPopupRect() const { return popup; }
	const UiRect& getBuildPanelRect() const { return build_panel; }
	const UiRect& getButtonRect(UiButton button) const;
	bool isButtonVisible(UiButton button) const;
	UiButton getHoveredButton() const { return hovered; }
	UiButton getPressedButton() const { return pressed; }
	bool isPopupVisible() const { return selected_colonist != INVALID_COLONIST || selected_building != INVALID_BUILDING; }

	static const char* buttonLabel(UiButton button);
	static UiAction actionFor(UiButton button);
	// Role button for a job / role ordered by an action (false for other actions).
	static UiButton jobButton(Job job);
	static bool jobForAction(UiAction action, Job& out_job);

	// Display names shared by the HUD (kept here so they are testable).
	static const char* jobName(Job job);
	static const char* stateName(ColonistState state);

	// Every materials.json colour the HUD and the world markers need; Game checks
	// them at startup and shipped_data_tests checks the shipped file.
	static const std::vector<std::string>& requiredMaterialIds();

private:
	enum class Capture : std::uint8_t { None, Button, OccupantRow, Panel, World };

	UiButton buttonAt(float x, float y) const;
	int occupantRowAt(float x, float y) const;

	float width = 0.0f;
	float height = 0.0f;
	UiRect top_bar;
	UiRect resource_bar;
	UiRect help_bar;
	UiRect popup;
	UiRect build_panel;
	UiRect buttons[static_cast<int>(UiButton::Count)];

	ColonistId selected_colonist = INVALID_COLONIST;
	BuildingId selected_building = INVALID_BUILDING;
	std::vector<ColonistId> occupants;
	int hovered_row = -1;
	int pressed_row = -1;
	ColonistId clicked_occupant = INVALID_COLONIST;
	bool build_mode = false;
	int ghost_quarter_turns = 0;
	UiButton hovered = UiButton::None;
	UiButton pressed = UiButton::None;
	Capture capture = Capture::None;
	std::string message;
	float message_seconds = 0.0f;
};
