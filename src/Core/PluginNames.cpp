#include "Core/PluginNames.h"

namespace Core
{
	std::string ModuleBaseName(const std::string_view a_modulePath, const std::string_view a_fallback)
	{
		std::string_view name = a_modulePath;
		if (const auto slash = name.find_last_of("\\/"); slash != std::string_view::npos) {
			name.remove_prefix(slash + 1);
		}
		if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos && dot > 0) {
			name = name.substr(0, dot);
		}
		bool usable = !name.empty();
		for (const char c : name) {
			if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
				usable = false;
			}
		}
		return usable ? std::string(name) : std::string(a_fallback);
	}
}
