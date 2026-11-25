#include "Core/ReloadLearn.h"

#include <algorithm>

namespace Core
{
	void AddReloadLearn(std::vector<ReloadLearn>& a_history, ReloadLearn a_learn)
	{
		a_history.push_back(std::move(a_learn));
		while (a_history.size() > kReloadLearnWindow) {
			a_history.erase(a_history.begin());
		}
	}

	namespace
	{
		struct Item
		{
			std::size_t   learn;
			std::uint32_t id;
			double        dt;
		};

		[[nodiscard]] double Median(std::vector<double> a_v)
		{
			if (a_v.empty()) {
				return 0.0;
			}
			std::sort(a_v.begin(), a_v.end());
			const std::size_t n = a_v.size();
			return n % 2 == 1 ? a_v[n / 2] : 0.5 * (a_v[n / 2 - 1] + a_v[n / 2]);
		}
	}

	ReloadSequence ResolveReload(const std::vector<ReloadLearn>& a_history)
	{
		ReloadSequence out;
		if (a_history.empty()) {
			return out;
		}
		// the plain length: the median of the whole reloads (at least 92 % of the longest)
		double longest = 0.0;
		for (const auto& l : a_history) {
			longest = (std::max)(longest, l.dur);
		}
		std::vector<double> whole;
		for (const auto& l : a_history) {
			if (l.dur >= kReloadCompleteShare * longest) {
				whole.push_back(l.dur);
			}
		}
		out.plainDur = Median(whole);

		// the events, clustered by id and time
		std::vector<Item> items;
		for (std::size_t i = 0; i < a_history.size(); ++i) {
			for (const auto& e : a_history[i].ev) {
				items.push_back({ i, e.id, e.dt });
			}
		}
		std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.id != b.id ? a.id < b.id : a.dt < b.dt; });
		std::size_t i = 0;
		while (i < items.size()) {
			std::size_t j = i + 1;
			while (j < items.size() && items[j].id == items[i].id && items[j].dt - items[j - 1].dt <= kReloadMatchWindow) {
				++j;
			}
			std::vector<double>      times;
			std::vector<bool>        present(a_history.size(), false);
			for (std::size_t k = i; k < j; ++k) {
				times.push_back(items[k].dt);
				present[items[k].learn] = true;
			}
			const double time = Median(times);
			int          here = 0;
			int          absent = 0;
			for (std::size_t l = 0; l < a_history.size(); ++l) {
				if (present[l]) {
					++here;
				} else if (a_history[l].dur - kReloadObserveMargin > time) {
					++absent;
				}
			}
			if (here > absent) {
				out.events.push_back({ items[i].id, time });
			}
			i = j;
		}
		std::sort(out.events.begin(), out.events.end(), [](const ReloadEvent& a, const ReloadEvent& b) { return a.dt != b.dt ? a.dt < b.dt : a.id < b.id; });
		return out;
	}
}
