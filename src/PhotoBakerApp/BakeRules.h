#pragma once

// Pure decisions of the bake run (no window, no clock, no files), so the offline test tool can check them:
// when the game is started again and how many automatic restarts are left, when a running game counts as stuck,
// and the list of cells the plugin could not photograph.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PhotoBaker
{
	// Automatic restarts after the game closes early: the budget fills up again whenever the bake made progress since
	// the last start, so it only runs out when the game keeps closing without taking a photo.
	inline constexpr int kDefaultRestarts = 10;
	inline constexpr int kMaxRestarts = 50;

	enum class GameExit
	{
		kCrash,           // closed early by itself
		kStuck,           // closed by the stall watchdog
		kPlannedRestart   // the plugin quit on purpose (status "restart") so the game starts fresh
	};

	struct RestartDecision
	{
		bool relaunch{ false };
		int  restartsLeft{ 0 };       // after this restart
		bool usedBudget{ false };     // this restart took one from the budget
		bool budgetRefilled{ false }; // progress since the last start filled the budget up again first
	};

	// a_doneAtStart: the status done count when the game was last started; a_doneNow: the highest done count since.
	RestartDecision DecideRestart(GameExit a_exit, int a_restartsLeft, int a_restartsAllowed, int a_doneAtStart, int a_doneNow);

	// Stall watchdog: the game runs but the photo count has not moved. Times in milliseconds from one steady clock.
	inline constexpr int           kDefaultStallMinutes = 5;   // 0 = off
	inline constexpr int           kMaxStallMinutes = 60;
	inline constexpr std::uint64_t kStartAllowanceMs = 300'000;  // game start, main menu, entering the world, listing the shots

	// True when the game should be closed and started again: no progress for a_limitMs since the later of the last
	// progress and the end of the start allowance after a_launchMs. Never while the plugin is writing tiles or quitting.
	bool IsStalled(std::string_view a_state, std::uint64_t a_nowMs, std::uint64_t a_lastProgressMs, std::uint64_t a_launchMs, std::uint64_t a_limitMs);

	// Data\SKSE\Plugins\ModernMinimapMissing.txt: one cell key per line; blank lines and spaces are ignored.
	std::vector<std::string> MissingCellKeys(std::string_view a_text);
}
