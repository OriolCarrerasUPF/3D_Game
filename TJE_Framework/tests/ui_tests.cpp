// P04 headless checks: HUD layout and UI-first pointer handling.

#include "test_harness.h"

#include "game/colony_ui.h"

namespace
{
	UiPointerResult press(ColonyUI& ui, PointerType type, float x, float y, PointerButton button = PointerButton::Left)
	{
		return ui.handlePointer(PointerEvent{ type, button, x, y });
	}

	float centerX(const UiRect& r) { return r.x + r.w * 0.5f; }
	float centerY(const UiRect& r) { return r.y + r.h * 0.5f; }
}

TEST_CASE("ui: layout keeps every control inside the window")
{
	for (const auto& size : { std::pair<float, float>(800.0f, 600.0f), std::pair<float, float>(1920.0f, 1009.0f),
			std::pair<float, float>(640.0f, 480.0f) })
	{
		ColonyUI ui;
		ui.layout(size.first, size.second);
		const UiRect& bar = ui.getTopBar();
		CHECK_NEAR(bar.w, size.first, 1e-4);
		float previous_right = 0.0f;
		for (UiButton button : { UiButton::Pause, UiButton::SlowDown, UiButton::SpeedUp, UiButton::Build, UiButton::Overlay })
		{
			const UiRect& r = ui.getButtonRect(button);
			CHECK(r.x >= previous_right);                       // left to right, no overlap
			CHECK(r.x + r.w <= size.first);
			CHECK(r.y >= bar.y && r.y + r.h <= bar.y + bar.h);  // inside the top bar
			previous_right = r.x + r.w;
		}
		const UiRect& popup = ui.getPopupRect();
		CHECK(popup.x >= 0.0f && popup.x + popup.w <= size.first);
		CHECK(popup.y >= ColonyUI::top_bar_height);
		const UiRect& panel = ui.getBuildPanelRect();
		CHECK(panel.x >= 0.0f && panel.x + panel.w <= size.first);
		CHECK(panel.y + panel.h <= size.second - ColonyUI::help_bar_height);
	}
}

TEST_CASE("ui: a button fires only when pressed and released on it")
{
	ColonyUI ui;
	ui.layout(800.0f, 600.0f);
	const UiRect& build = ui.getButtonRect(UiButton::Build);

	UiPointerResult down = press(ui, PointerType::Down, centerX(build), centerY(build));
	CHECK(down.consumed);
	CHECK(down.action == UiAction::None);
	UiPointerResult up = press(ui, PointerType::Up, centerX(build), centerY(build));
	CHECK(up.consumed);
	CHECK(up.action == UiAction::ToggleBuildMode);
	CHECK(ui.isBuildMode());

	// Pressed on the button, released in the world: nothing happens, still consumed.
	press(ui, PointerType::Down, centerX(build), centerY(build));
	up = press(ui, PointerType::Up, 400.0f, 300.0f);
	CHECK(up.consumed);
	CHECK(up.action == UiAction::None);
	CHECK(ui.isBuildMode());

	const UiRect& pause = ui.getButtonRect(UiButton::Pause);
	press(ui, PointerType::Down, centerX(pause), centerY(pause));
	CHECK(press(ui, PointerType::Up, centerX(pause), centerY(pause)).action == UiAction::TogglePause);
}

TEST_CASE("ui: world clicks pass through, UI areas swallow them")
{
	ColonyUI ui;
	ui.layout(800.0f, 600.0f);
	// Plain world click.
	CHECK(!press(ui, PointerType::Down, 300.0f, 300.0f).consumed);
	CHECK(!press(ui, PointerType::Up, 302.0f, 301.0f).consumed);

	// World press released over a button: swallowed, no action (no drag-through).
	const UiRect& pause = ui.getButtonRect(UiButton::Pause);
	CHECK(!press(ui, PointerType::Down, 300.0f, 300.0f).consumed);
	UiPointerResult up = press(ui, PointerType::Up, centerX(pause), centerY(pause));
	CHECK(up.consumed);
	CHECK(up.action == UiAction::None);

	// Empty part of the top bar and the help bar are UI too.
	CHECK(press(ui, PointerType::Down, 40.0f, 10.0f).consumed);
	CHECK(press(ui, PointerType::Up, 40.0f, 10.0f).consumed);
	CHECK(ui.isPointerOverUi(10.0f, 590.0f));

	// Right/middle presses: consumed only over the UI.
	CHECK(press(ui, PointerType::Down, 40.0f, 10.0f, PointerButton::Right).consumed);
	CHECK(!press(ui, PointerType::Down, 300.0f, 300.0f, PointerButton::Middle).consumed);
}

TEST_CASE("ui: the popup exists only while something is selected")
{
	ColonyUI ui;
	ui.layout(800.0f, 600.0f);
	const UiRect& popup = ui.getPopupRect();
	const float px = centerX(popup), py = popup.y + popup.h - 10.0f;
	CHECK(!ui.isPointerOverUi(px, py));
	ui.select(1);
	CHECK(ui.isPopupVisible());
	CHECK(ui.isPointerOverUi(px, py));
	CHECK(press(ui, PointerType::Down, px, py).consumed);
	press(ui, PointerType::Up, px, py);
	CHECK_EQ(ui.getSelectedColonist(), 1u);

	const UiRect& close = ui.getButtonRect(UiButton::ClosePopup);
	press(ui, PointerType::Down, centerX(close), centerY(close));
	UiPointerResult up = press(ui, PointerType::Up, centerX(close), centerY(close));
	CHECK(up.action == UiAction::CloseSelection);
	CHECK(!ui.isPopupVisible());
	// Hidden close button no longer reacts.
	CHECK(!press(ui, PointerType::Down, centerX(close), centerY(close)).consumed);
}

TEST_CASE("ui: cancel closes build mode first, then the selection")
{
	ColonyUI ui;
	ui.select(4);
	ui.setBuildMode(true);
	ui.rotateGhost();
	ui.rotateGhost();
	CHECK_EQ(ui.getGhostQuarterTurns(), 2);
	CHECK(ui.cancel() == ColonyUI::CancelResult::BuildMode);
	CHECK(!ui.isBuildMode());
	CHECK_EQ(ui.getGhostQuarterTurns(), 0);
	CHECK(ui.cancel() == ColonyUI::CancelResult::Selection);
	CHECK_EQ(ui.getSelectedColonist(), INVALID_COLONIST);
	CHECK(ui.cancel() == ColonyUI::CancelResult::Nothing);
	for (int i = 0; i < 5; ++i)
		ui.rotateGhost();
	CHECK_EQ(ui.getGhostQuarterTurns(), 1);
}

TEST_CASE("ui: messages expire on real time")
{
	ColonyUI ui;
	CHECK(!ui.hasMessage());
	ui.showMessage("Exit Door closed", 1.0f);
	CHECK(ui.hasMessage());
	ui.update(0.5f);
	CHECK(ui.hasMessage());
	ui.update(0.6f);
	CHECK(!ui.hasMessage());
	CHECK_EQ(std::string(ColonyUI::jobName(Job::Miner)), std::string("Miner"));
	CHECK_EQ(std::string(ColonyUI::stateName(ColonistState::MovingToWork)), std::string("Walking"));
}
