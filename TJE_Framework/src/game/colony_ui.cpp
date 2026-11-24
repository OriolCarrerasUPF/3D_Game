#include "colony_ui.h"

#include <algorithm>

namespace
{
	constexpr float button_height = 24.0f;
	constexpr float button_gap = 6.0f;
	constexpr float bar_padding = 8.0f;
	constexpr float popup_width = 290.0f;
	constexpr float popup_height = 236.0f;
	constexpr float popup_margin = 10.0f;
	constexpr float close_size = 22.0f;
	constexpr float build_panel_width = 560.0f;
	constexpr float build_panel_height = 62.0f;

	float buttonWidth(UiButton button)
	{
		switch (button)
		{
		case UiButton::Pause: return 86.0f;
		case UiButton::SlowDown: return 28.0f;
		case UiButton::SpeedUp: return 28.0f;
		case UiButton::Build: return 86.0f;
		case UiButton::Overlay: return 96.0f;
		default: return 0.0f;
		}
	}
}

ColonyUI::ColonyUI()
{
	layout(800.0f, 600.0f);
}

void ColonyUI::layout(float new_width, float new_height)
{
	width = std::max(new_width, 1.0f);
	height = std::max(new_height, 1.0f);

	top_bar = UiRect{ 0.0f, 0.0f, width, top_bar_height };
	help_bar = UiRect{ 0.0f, height - help_bar_height, width, help_bar_height };

	// Top-bar buttons, right aligned: [Pause][-][+]  [Build]  [Debug F3]
	const UiButton order[] = { UiButton::Pause, UiButton::SlowDown, UiButton::SpeedUp, UiButton::Build, UiButton::Overlay };
	float total = 0.0f;
	for (UiButton button : order)
		total += buttonWidth(button) + button_gap;
	float x = std::max(width - bar_padding - total + button_gap, bar_padding);
	const float y = (top_bar_height - button_height) * 0.5f;
	for (UiButton button : order)
	{
		buttons[static_cast<int>(button)] = UiRect{ x, y, buttonWidth(button), button_height };
		x += buttonWidth(button) + button_gap;
	}

	const float popup_w = std::min(popup_width, width - 2.0f * popup_margin);
	popup = UiRect{ width - popup_w - popup_margin, top_bar_height + popup_margin, popup_w, popup_height };
	buttons[static_cast<int>(UiButton::ClosePopup)] =
		UiRect{ popup.x + popup.w - close_size - 6.0f, popup.y + 6.0f, close_size, close_size };

	const float panel_w = std::min(build_panel_width, width - 20.0f);
	build_panel = UiRect{ (width - panel_w) * 0.5f, height - help_bar_height - build_panel_height - 8.0f, panel_w, build_panel_height };
}

const UiRect& ColonyUI::getButtonRect(UiButton button) const
{
	static const UiRect empty;
	const int index = static_cast<int>(button);
	if (index < 0 || index >= static_cast<int>(UiButton::Count))
		return empty;
	return buttons[index];
}

bool ColonyUI::isButtonVisible(UiButton button) const
{
	if (button == UiButton::ClosePopup)
		return isPopupVisible();
	return static_cast<int>(button) < static_cast<int>(UiButton::Count);
}

UiButton ColonyUI::buttonAt(float x, float y) const
{
	for (int i = 0; i < static_cast<int>(UiButton::Count); ++i)
	{
		const UiButton button = static_cast<UiButton>(i);
		if (isButtonVisible(button) && buttons[i].contains(x, y))
			return button;
	}
	return UiButton::None;
}

bool ColonyUI::isPointerOverUi(float x, float y) const
{
	return top_bar.contains(x, y) || help_bar.contains(x, y) ||
		(isPopupVisible() && popup.contains(x, y)) ||
		(build_mode && build_panel.contains(x, y));
}

UiPointerResult ColonyUI::handlePointer(const PointerEvent& event)
{
	UiPointerResult result;
	hovered = buttonAt(event.x, event.y);
	const bool over_ui = isPointerOverUi(event.x, event.y);

	if (event.type == PointerType::Move)
	{
		result.consumed = over_ui;
		return result;
	}

	if (event.button != PointerButton::Left)
	{
		// Right/middle buttons never trigger UI actions, but presses on the UI stay there.
		result.consumed = over_ui && event.type == PointerType::Down;
		return result;
	}

	if (event.type == PointerType::Down)
	{
		if (hovered != UiButton::None)
		{
			pressed = hovered;
			capture = Capture::Button;
			result.consumed = true;
		}
		else if (over_ui)
		{
			capture = Capture::Panel;
			result.consumed = true;
		}
		else
		{
			capture = Capture::World;
			result.consumed = false;
		}
		return result;
	}

	// Left button up.
	const Capture previous_capture = capture;
	const UiButton previous_pressed = pressed;
	capture = Capture::None;
	pressed = UiButton::None;
	switch (previous_capture)
	{
	case Capture::Button:
		result.consumed = true;
		if (hovered == previous_pressed)
		{
			result.action = actionFor(previous_pressed);
			// UI-owned state changes happen here; Game handles the rest.
			if (result.action == UiAction::ToggleBuildMode)
				setBuildMode(!build_mode);
			else if (result.action == UiAction::CloseSelection)
				clearSelection();
		}
		return result;
	case Capture::Panel:
		result.consumed = true;
		return result;
	case Capture::World:
	case Capture::None:
	default:
		// A world press released over the UI is swallowed (no click through panels).
		result.consumed = over_ui;
		return result;
	}
}

void ColonyUI::setBuildMode(bool enabled)
{
	build_mode = enabled;
	if (!enabled)
		ghost_quarter_turns = 0;
}

ColonyUI::CancelResult ColonyUI::cancel()
{
	if (build_mode)
	{
		setBuildMode(false);
		return CancelResult::BuildMode;
	}
	if (selected_colonist != INVALID_COLONIST)
	{
		clearSelection();
		return CancelResult::Selection;
	}
	return CancelResult::Nothing;
}

void ColonyUI::showMessage(const std::string& text, float seconds)
{
	message = text;
	message_seconds = std::max(seconds, 0.0f);
}

void ColonyUI::update(float real_dt)
{
	if (message_seconds > 0.0f)
		message_seconds = std::max(0.0f, message_seconds - std::max(real_dt, 0.0f));
}

const char* ColonyUI::buttonLabel(UiButton button)
{
	switch (button)
	{
	case UiButton::Pause: return "Pause";
	case UiButton::SlowDown: return "-";
	case UiButton::SpeedUp: return "+";
	case UiButton::Build: return "Build (B)";
	case UiButton::Overlay: return "Debug (F3)";
	case UiButton::ClosePopup: return "x";
	default: return "";
	}
}

UiAction ColonyUI::actionFor(UiButton button)
{
	switch (button)
	{
	case UiButton::Pause: return UiAction::TogglePause;
	case UiButton::SlowDown: return UiAction::SlowDown;
	case UiButton::SpeedUp: return UiAction::SpeedUp;
	case UiButton::Build: return UiAction::ToggleBuildMode;
	case UiButton::Overlay: return UiAction::ToggleOverlay;
	case UiButton::ClosePopup: return UiAction::CloseSelection;
	default: return UiAction::None;
	}
}

const char* ColonyUI::jobName(Job job)
{
	switch (job)
	{
	case Job::Engineer: return "Engineer";
	case Job::Miner: return "Miner";
	case Job::Farmer: return "Farmer";
	}
	return "Unknown";
}

const char* ColonyUI::stateName(ColonistState state)
{
	switch (state)
	{
	case ColonistState::Idle: return "Idle";
	case ColonistState::MovingToWork: return "Walking";
	case ColonistState::Working: return "Working";
	case ColonistState::MovingToService: return "Going to refill";
	case ColonistState::Refilling: return "Refilling";
	case ColonistState::Dead: return "Dead";
	}
	return "Unknown";
}

const std::vector<std::string>& ColonyUI::requiredMaterialIds()
{
	static const std::vector<std::string> ids{
		// HUD
		"ui_panel", "ui_button", "ui_button_hover", "ui_button_active", "ui_bar_back",
		"ui_need_hunger", "ui_need_thirst", "ui_need_oxygen",
		// World markers drawn by ColonyWorld
		"ui_selection", "ui_ghost_valid", "ui_ghost_invalid",
		"debug_blocked", "debug_door_open", "debug_door_closed", "debug_path" };
	return ids;
}
