#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"
#include "sqlite3.h"

namespace sml {
using Json = nlohmann::json;
using Path = std::filesystem::path;
using Time = int64_t; // Microseconds since the Unix epoch.
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
Time now();
Time parse_time(const std::string& value);
std::string stamp(Time value);
std::string id(const Json& value);
Json read_json(const Path& path);
void write_json(const Path& path, const Json& value);
std::string digest(const std::string& name, const std::map<std::string, std::string>& members);

class Database {
    sqlite3* db_ = nullptr;
    double max_age_;
public:
    Database(const Path& path, double max_age_hours = 24);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Json apply(const Json& payload, Time at = now(), bool remove_leaves=true);
    bool scan_due(const std::string& server, bool counts, int days, Time at=now());
    Json servers(Time at = now());
    void save_server_count(const Json& snapshot);
    Json members_page(const std::string& search, int sort, bool descending, size_t offset=0);
    Json member_servers(const std::string& user);
    Json preview(const std::string& user, const std::string& target, Time at = now());
    void apply_account(const Json& snapshot);
    void forget_account(const std::string& user);
};
Json scan(Database& database, const Path& inbox);
}
