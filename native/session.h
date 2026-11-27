#pragma once
#include "core.h"
#include "gateway.h"
#include <functional>

namespace sml {
// Requests use the supplied session and only the access Discord grants it.
using DiscordGet = std::function<Json(const std::string&)>;
Json fetch_account(const DiscordGet& get);
Json connect_account(const std::string& token);
Json collect_members(const std::string& token, const std::string& server, const std::atomic<bool>& cancel, const ProgressCallback& progress={});
Json collect_server_count(const std::string& token, const std::string& server, const std::atomic<bool>& cancel, const ProgressCallback& progress={});
Json collect_server_counts(const std::string& token, const std::vector<std::string>& servers, const std::atomic<bool>& cancel, const ProgressCallback& progress={});
void save_sessions(const Path& path, const Json& sessions);
Json load_sessions(const Path& path);
}
