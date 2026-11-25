#include "BakeRules.h"

#include <algorithm>
#include <array>

namespace PhotoBaker
{
	namespace
	{
		// States in which the game is busy finishing or leaving on purpose: no stall restart then.
		constexpr std::array<std::string_view, 5> kNoStallStates{ "writing", "done", "cancelled", "error", "restart" };

		std::string_view Trim(std::string_view a_s)
		{
			const auto b = a_s.find_first_not_of(" \t\r");
			if (b == std::string_view::npos)
				return {};
			const auto e = a_s.find_last_not_of(" \t\r");
			return a_s.substr(b, e - b + 1);
		}
	}

	RestartDecision DecideRestart(GameExit a_exit, int a_restartsLeft, int a_restartsAllowed, int a_doneAtStart, int a_doneNow)
	{
		RestartDecision decision;
		decision.budgetRefilled = a_doneNow > a_doneAtStart && a_restartsLeft < a_restartsAllowed;
		decision.restartsLeft = a_doneNow > a_doneAtStart ? a_restartsAllowed : a_restartsLeft;
		if (a_exit == GameExit::kPlannedRestart) {
			decision.relaunch = true;
			return decision;
		}
		if (decision.restartsLeft <= 0) {
			decision.restartsLeft = 0;
			return decision;
		}
		--decision.restartsLeft;
		decision.relaunch = true;
		decision.usedBudget = true;
		return decision;
	}

	bool IsStalled(std::string_view a_state, std::uint64_t a_nowMs, std::uint64_t a_lastProgressMs, std::uint64_t a_launchMs, std::uint64_t a_limitMs)
	{
		if (a_limitMs == 0)
			return false;
		if (std::ranges::find(kNoStallStates, a_state) != kNoStallStates.end())
			return false;
		const std::uint64_t since = std::max(a_lastProgressMs, a_launchMs + kStartAllowanceMs);
		return a_nowMs > since && a_nowMs - since >= a_limitMs;
	}

	std::vector<std::string> MissingCellKeys(std::string_view a_text)
	{
		std::vector<std::string> keys;
		while (!a_text.empty()) {
			const auto eol = a_text.find('\n');
			const auto line = Trim(a_text.substr(0, eol));
			if (!line.empty())
				keys.emplace_back(line);
			if (eol == std::string_view::npos)
				break;
			a_text.remove_prefix(eol + 1);
		}
		return keys;
	}
}
