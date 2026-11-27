#include "core.h"
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sml {
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode text");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
constexpr int64_t epoch_ticks = 116444736000000000LL;
Time now() {
    FILETIME ft{}; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER raw{}; raw.LowPart = ft.dwLowDateTime; raw.HighPart = ft.dwHighDateTime;
    return (static_cast<int64_t>(raw.QuadPart) - epoch_ticks) / 10;
}
Time parse_time(const std::string& value) {
    static const std::regex pattern(R"(^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(Z|[+-]\d{2}:\d{2})$)");
    std::smatch m;
    if (!std::regex_match(value, m, pattern)) throw std::runtime_error("Capture time must be an ISO date/time with a timezone");
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(std::stoi(m[1])); st.wMonth = static_cast<WORD>(std::stoi(m[2]));
    st.wDay = static_cast<WORD>(std::stoi(m[3])); st.wHour = static_cast<WORD>(std::stoi(m[4]));
    st.wMinute = static_cast<WORD>(std::stoi(m[5])); st.wSecond = static_cast<WORD>(std::stoi(m[6]));
    FILETIME ft{}; SYSTEMTIME check{};
    if (!SystemTimeToFileTime(&st, &ft) || !FileTimeToSystemTime(&ft, &check)
        || st.wDay != check.wDay || st.wMonth != check.wMonth || st.wYear < 1970 || st.wYear > 9998)
        throw std::runtime_error("Invalid capture date");
    ULARGE_INTEGER raw{}; raw.LowPart = ft.dwLowDateTime; raw.HighPart = ft.dwHighDateTime;
    std::string fraction = m[7].str(); fraction.resize(6, '0');
    Time result = (static_cast<int64_t>(raw.QuadPart) - epoch_ticks) / 10 + std::stoll(fraction);
    std::string zone = m[8];
    if (zone != "Z") {
        int hours = std::stoi(zone.substr(1, 2)), minutes = std::stoi(zone.substr(4, 2));
        if (hours > 23 || minutes > 59) throw std::runtime_error("Invalid timezone offset");
        result -= (zone[0] == '+' ? 1LL : -1LL) * (hours * 60 + minutes) * 60000000LL;
    }
    return result;
}
std::string stamp(Time value) {
    ULARGE_INTEGER raw{}; raw.QuadPart = static_cast<ULONGLONG>(value * 10 + epoch_ticks);
    FILETIME ft{raw.LowPart, raw.HighPart}; SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) throw std::runtime_error("Timestamp is out of range");
    char buffer[50];
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%06lld+00:00", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, (value % 1000000 + 1000000) % 1000000);
    return buffer;
}
std::string id(const Json& value) {
    std::string text;
    if (value.is_string()) text = value.get<std::string>();
    else if (value.is_number_integer()) text = value.dump();
    else throw std::runtime_error("IDs must be positive integers or numeric strings");
    if (text.starts_with("<@") && text.ends_with('>')) {
        text = text.substr(2, text.size()-3);
        if (text.starts_with('!')) text.erase(0, 1);
    }
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Enter a valid Discord ID or member mention");
    uint64_t number = 0;
    try { number = std::stoull(text); } catch (...) { throw std::runtime_error("Discord ID is out of range"); }
    if (!number) throw std::runtime_error("Discord ID cannot be zero");
    return std::to_string(number);
}
Json read_json(const Path& path) {
    if (std::filesystem::file_size(path) > 512ULL * 1024 * 1024) throw std::runtime_error("Maximum snapshot size is 512 MB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open snapshot file");
    return Json::parse(input);
}
void write_json(const Path& path, const Json& value) {
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << value.dump(2) << '\n';
    if (!out) throw std::runtime_error("Cannot write JSON file");
}
std::string digest(const std::string& name, const std::map<std::string, std::string>& members) {
    // Match Python's previous json.dumps([name, sorted_members], ensure_ascii=False).
    std::string content = "[" + Json(name).dump() + ", [";
    bool first = true;
    for (auto& [uid, username] : members) {
        if (!first) content += ", "; first = false;
        content += "[" + Json(uid).dump() + ", " + Json(username).dump() + "]";
    }
    content += "]]";
    BCRYPT_ALG_HANDLE algorithm{}; unsigned char output[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("Cannot initialize snapshot hash");
    auto status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(content.data()),
                             static_cast<ULONG>(content.size()), output, sizeof(output));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) throw std::runtime_error("Cannot hash snapshot");
    std::ostringstream result; result << std::hex << std::setfill('0');
    for (auto b : output) result << std::setw(2) << static_cast<int>(b);
    return result.str();
}
static void execute(sqlite3* db, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        std::string error = message ? message : "Database error"; sqlite3_free(message); throw std::runtime_error(error);
    }
}
struct Statement {
    sqlite3_stmt* s{};
    sqlite3* db;
    Statement(sqlite3* database, const char* sql) : db(database) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(s); }
    void bind(int index, const std::string& value) {
        if (sqlite3_bind_text(s, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    bool step() {
        int result = sqlite3_step(s);
        if (result != SQLITE_ROW && result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return result == SQLITE_ROW;
    }
    std::string text(int column) { const auto* value = sqlite3_column_text(s, column); return value ? reinterpret_cast<const char*>(value) : ""; }
};
Database::Database(const Path& path, double max_age_hours) : max_age_(max_age_hours) {
    if (!std::isfinite(max_age_) || max_age_ <= 0 || max_age_ > 8760) throw std::runtime_error("Freshness must be between 0 and 8,760 hours");
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    if (sqlite3_open(utf8(path.wstring()).c_str(), &db_) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot open database";
        sqlite3_close(db_); db_ = nullptr; throw std::runtime_error(error);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        execute(db_, "CREATE TABLE IF NOT EXISTS server_counts(server_id TEXT PRIMARY KEY,server_name TEXT NOT NULL,total INTEGER NOT NULL,checked_at TEXT NOT NULL);");
        execute(db_, "PRAGMA foreign_keys=ON; CREATE TABLE IF NOT EXISTS servers (server_id TEXT PRIMARY KEY, server_name TEXT NOT NULL, captured_at TEXT NOT NULL, imported_at TEXT NOT NULL, digest TEXT NOT NULL); CREATE TABLE IF NOT EXISTS members (server_id TEXT NOT NULL REFERENCES servers(server_id), user_id TEXT NOT NULL, username TEXT NOT NULL, PRIMARY KEY(server_id,user_id)); CREATE INDEX IF NOT EXISTS members_by_user ON members(user_id,server_id);");
        execute(db_, "CREATE TABLE IF NOT EXISTS own_accounts (user_id TEXT PRIMARY KEY, username TEXT NOT NULL, captured_at TEXT NOT NULL); CREATE TABLE IF NOT EXISTS account_servers (user_id TEXT NOT NULL REFERENCES own_accounts(user_id), server_id TEXT NOT NULL, server_name TEXT NOT NULL, PRIMARY KEY(user_id,server_id));");
        execute(db_, "CREATE TABLE IF NOT EXISTS roster_coverage(server_id TEXT PRIMARY KEY, complete INTEGER NOT NULL); CREATE TABLE IF NOT EXISTS member_seen(server_id TEXT NOT NULL,user_id TEXT NOT NULL,last_seen TEXT NOT NULL,PRIMARY KEY(server_id,user_id)); INSERT OR IGNORE INTO member_seen SELECT m.server_id,m.user_id,s.captured_at FROM members m JOIN servers s USING(server_id);");
        execute(db_, "CREATE VIEW IF NOT EXISTS tracked_memberships AS SELECT m.user_id,m.username,m.server_id,s.server_name,o.last_seen,CASE WHEN COALESCE(c.complete,1)=1 THEN 'Complete roster' ELSE 'Partial cache' END AS coverage FROM members m JOIN servers s USING(server_id) JOIN member_seen o ON o.server_id=m.server_id AND o.user_id=m.user_id LEFT JOIN roster_coverage c ON c.server_id=m.server_id WHERE NOT EXISTS(SELECT 1 FROM own_accounts a WHERE a.user_id=m.user_id) UNION ALL SELECT a.user_id,a.username,g.server_id,g.server_name,a.captured_at,'Connected account' FROM account_servers g JOIN own_accounts a USING(user_id);");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Database::~Database() { sqlite3_close(db_); }
Json Database::apply(const Json& payload, Time at, bool remove_leaves) {
    struct Snapshot { std::string sid, name, captured, hash; std::map<std::string,std::string> members; bool complete=true; };
    auto list = payload.is_array() ? payload : Json::array({payload});
    if (list.empty() || list.size() > 1000) throw std::runtime_error("Supply 1 to 1,000 server snapshots");
    std::vector<Snapshot> prepared; std::set<std::string> seen;
    for (const auto& item : list) {
        if (!item.is_object() || !item.contains("complete") || !item["complete"].is_boolean())
            throw std::runtime_error("Each snapshot must declare complete: true or false");
        Snapshot row; row.complete=item["complete"].get<bool>()&&remove_leaves;
        row.sid = id(item.at("server_id")); row.name = item.at("server_name").get<std::string>();
        if (!seen.insert(row.sid).second) throw std::runtime_error("Duplicate server in this upload");
        if (row.name.empty() || wide(row.name).size() > 100 || row.name.find_first_not_of(" \r\n\t") == std::string::npos)
            throw std::runtime_error("Server name must contain 1 to 100 characters");
        Time captured = parse_time(item.at("captured_at").get<std::string>());
        if (captured > at + 300000000) throw std::runtime_error("Capture time cannot be in the future");
        row.captured = stamp(captured);
        const auto& members = item.at("members");
        if (!members.is_array() || members.size() > 5000000) throw std::runtime_error("members must be an array of at most 5,000,000 accounts");
        for (const auto& member : members) {
            std::string uid = id(member.at("user_id")); std::string username = member.value("username", std::string{});
            if (wide(username).size() > 100 || !row.members.emplace(uid, username).second)
                throw std::runtime_error("Duplicate member or invalid username");
        }
        row.hash = (row.complete?"":"partial:")+digest(row.name, row.members);
        prepared.push_back(std::move(row));
    }
    Json results = Json::array();
    execute(db_, "BEGIN IMMEDIATE");
    try {
        for (const auto& row : prepared) {
            std::string old_time, old_hash;
            { Statement q(db_, "SELECT captured_at,digest FROM servers WHERE server_id=?"); q.bind(1,row.sid);
              if (q.step()) { old_time=q.text(0); old_hash=q.text(1); } }
            if (!old_time.empty() && row.captured < old_time) throw std::runtime_error("Older snapshot rejected for " + row.name);
            if (old_time == row.captured) {
                if (old_hash != row.hash) throw std::runtime_error("Different contents at the same capture time; provide the actual newer capture time");
                Statement count(db_,"SELECT COUNT(*) FROM members WHERE server_id=?");count.bind(1,row.sid);count.step();
                results.push_back({{"server_id",row.sid},{"status","unchanged"},{"count",sqlite3_column_int64(count.s,0)},{"added",0},{"removed",0},{"complete",row.complete}}); continue;
            }
            std::set<std::string> previous;
            { Statement q(db_, "SELECT user_id FROM members WHERE server_id=?"); q.bind(1,row.sid); while(q.step()) previous.insert(q.text(0)); }
            size_t added=0, removed=0;
            for (auto& [uid,name] : row.members) if (!previous.contains(uid)) ++added;
            if(row.complete)for (auto& uid : previous) if (!row.members.contains(uid)) ++removed;
            { Statement q(db_, "INSERT INTO servers VALUES (?,?,?,?,?) ON CONFLICT(server_id) DO UPDATE SET server_name=excluded.server_name,captured_at=excluded.captured_at,imported_at=excluded.imported_at,digest=excluded.digest");
              q.bind(1,row.sid);q.bind(2,row.name);q.bind(3,row.captured);q.bind(4,stamp(at));q.bind(5,row.hash);q.step(); }
            { Statement q(db_, "INSERT INTO roster_coverage VALUES (?,?) ON CONFLICT(server_id) DO UPDATE SET complete=excluded.complete");q.bind(1,row.sid);q.bind(2,row.complete?"1":"0");q.step(); }
            if(row.complete) {
                Statement q(db_, "DELETE FROM members WHERE server_id=?");q.bind(1,row.sid);q.step();
                Statement clear_seen(db_, "DELETE FROM member_seen WHERE server_id=?");clear_seen.bind(1,row.sid);clear_seen.step();
            }
            Statement insert(db_, "INSERT INTO members VALUES (?,?,?) ON CONFLICT(server_id,user_id) DO UPDATE SET username=excluded.username");
            Statement observed(db_, "INSERT INTO member_seen VALUES (?,?,?) ON CONFLICT(server_id,user_id) DO UPDATE SET last_seen=excluded.last_seen");
            for (auto& [uid,name] : row.members) {
                sqlite3_reset(insert.s); insert.bind(1,row.sid);insert.bind(2,uid);insert.bind(3,name);insert.step();
                sqlite3_reset(observed.s);observed.bind(1,row.sid);observed.bind(2,uid);observed.bind(3,row.captured);observed.step();
            }
            results.push_back({{"server_id",row.sid},{"status","updated"},{"count",row.complete?row.members.size():previous.size()+added},{"added",added},{"removed",removed},{"complete",row.complete}});
        }
        execute(db_, "COMMIT");
    } catch (...) { sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    return results;
}
bool Database::scan_due(const std::string& server,bool counts,int days,Time at) {
    if(days!=0&&days!=1&&days!=2&&days!=7&&days!=14&&days!=30)throw std::runtime_error("Invalid scan age");
    if(days==0)return true;
    Statement q(db_,counts?"SELECT checked_at FROM server_counts WHERE server_id=?":"SELECT captured_at FROM servers WHERE server_id=?");q.bind(1,id(server));
    return !q.step()||parse_time(q.text(0))<=at-static_cast<Time>(days)*86400000000LL;
}
void Database::save_server_count(const Json& snapshot) {
    auto sid=id(snapshot.at("server_id"));auto name=snapshot.at("server_name").get<std::string>();
    auto checked=stamp(parse_time(snapshot.at("captured_at").get<std::string>()));
    const auto& count=snapshot.at("server_total");
    if(!count.is_number_integer()||count.get<int64_t>()<0||name.empty()||wide(name).size()>100||parse_time(checked)>now()+300000000)throw std::runtime_error("Invalid server count");
    Statement q(db_,"INSERT INTO server_counts VALUES(?,?,?,?) ON CONFLICT(server_id) DO UPDATE SET server_name=excluded.server_name,total=excluded.total,checked_at=excluded.checked_at WHERE excluded.checked_at>=server_counts.checked_at");
    q.bind(1,sid);q.bind(2,name);q.bind(3,std::to_string(count.get<int64_t>()));q.bind(4,checked);q.step();
}
Json Database::servers(Time at) {
    Json rows=Json::array();auto cutoff=stamp(at-static_cast<Time>(max_age_*3600000000.0));
    Statement q(db_, "WITH ids AS (SELECT server_id FROM servers UNION SELECT server_id FROM account_servers UNION SELECT server_id FROM server_counts), counts AS (SELECT server_id,COUNT(DISTINCT user_id) n FROM tracked_memberships GROUP BY server_id), accounts AS (SELECT server_id,MAX(server_name) name,MAX(a.captured_at) checked FROM account_servers g JOIN own_accounts a USING(user_id) GROUP BY server_id) SELECT i.server_id,COALESCE(s.server_name,a.name,t.server_name),COALESCE(s.captured_at,a.checked,''),COALESCE(n.n,0),CASE WHEN s.server_id IS NULL AND a.name IS NULL THEN 'Counts only' WHEN s.server_id IS NULL THEN 'Connected accounts' WHEN COALESCE(c.complete,1)=1 THEN 'Complete roster' ELSE 'Partial cache' END,COALESCE(t.total,-1),COALESCE(t.checked_at,'') FROM ids i LEFT JOIN servers s USING(server_id) LEFT JOIN accounts a USING(server_id) LEFT JOIN counts n USING(server_id) LEFT JOIN roster_coverage c USING(server_id) LEFT JOIN server_counts t USING(server_id) ORDER BY 2 COLLATE NOCASE,1");
    while(q.step())rows.push_back({{"server_id",q.text(0)},{"server_name",q.text(1)},{"captured_at",q.text(2)},{"member_count",sqlite3_column_int64(q.s,3)},{"stale",q.text(2)<cutoff},{"coverage",q.text(4)},{"complete",q.text(4)=="Complete roster"},{"server_total",sqlite3_column_int64(q.s,5)},{"count_checked",q.text(6)}});
    return rows;
}
Json Database::members_page(const std::string& search,int sort,bool descending,size_t offset) {
    if(sort<0||sort>3||offset>100000000)throw std::runtime_error("Invalid member page");
    std::string pattern="%";for(char c:search){if(c=='%'||c=='_'||c=='\\')pattern+='\\';pattern+=c;}pattern+='%';
    std::string base="WITH grouped AS (SELECT user_id,username,COUNT(*) server_count,MAX(last_seen) last_seen,GROUP_CONCAT(server_name,' ') server_names,GROUP_CONCAT(server_id,' ') server_ids FROM tracked_memberships GROUP BY user_id) ";
    std::string where=" WHERE username LIKE ? ESCAPE '\\' OR user_id LIKE ? ESCAPE '\\' OR server_names LIKE ? ESCAPE '\\' OR server_ids LIKE ? ESCAPE '\\' ";
    auto bind=[&](Statement& q){for(int i=1;i<=4;++i)q.bind(i,pattern);};
    int64_t count=0;{Statement q(db_,(base+"SELECT COUNT(*) FROM grouped"+where).c_str());bind(q);if(q.step())count=sqlite3_column_int64(q.s,0);}
    const char* columns[]={"username COLLATE NOCASE","LENGTH(user_id)","server_count","last_seen"};auto order=std::string(columns[sort])+(descending?" DESC":" ASC")+",user_id"+(descending?" DESC":" ASC");
    Statement q(db_,(base+"SELECT user_id,username,server_count,last_seen FROM grouped"+where+" ORDER BY "+order+" LIMIT 500 OFFSET "+std::to_string(offset)).c_str());bind(q);Json rows=Json::array();
    while(q.step())rows.push_back({{"user_id",q.text(0)},{"username",q.text(1)},{"server_count",sqlite3_column_int64(q.s,2)},{"last_seen",q.text(3)}});
    return {{"total",count},{"rows",rows}};
}
Json Database::member_servers(const std::string& user) {
    Json rows=Json::array();Statement q(db_,"SELECT server_id,server_name,last_seen,coverage FROM tracked_memberships WHERE user_id=? ORDER BY server_name COLLATE NOCASE,server_id");q.bind(1,id(user));
    while(q.step())rows.push_back({{"server_id",q.text(0)},{"server_name",q.text(1)},{"last_seen",q.text(2)},{"coverage",q.text(3)}});return rows;
}
Json Database::preview(const std::string& user, const std::string& target, Time at) {
    auto uid=id(user), sid=id(target); Json matches=Json::array();
    bool own=false;
    {Statement profile(db_, "SELECT 1 FROM own_accounts WHERE user_id=?");profile.bind(1,uid);own=profile.step();}
    // A connected account's own complete server list overrides older imported rosters, including removals.
    Statement q(db_, own ? "SELECT g.server_id,g.server_name,a.captured_at FROM account_servers g JOIN own_accounts a USING(user_id) WHERE g.user_id=? AND g.server_id<>? AND a.captured_at>=? ORDER BY g.server_name,g.server_id" : "SELECT s.server_id,s.server_name,o.last_seen FROM members m JOIN servers s USING(server_id) JOIN member_seen o ON o.server_id=m.server_id AND o.user_id=m.user_id WHERE m.user_id=? AND s.server_id<>? AND o.last_seen>=? ORDER BY s.server_name,s.server_id");
    q.bind(1,uid);q.bind(2,sid);q.bind(3,stamp(at-static_cast<Time>(max_age_*3600000000.0)));
    while(q.step()) matches.push_back({{"server_id",q.text(0)},{"server_name",q.text(1)},{"captured_at",q.text(2)}});
    Json embed={{"title","Member Joined - Preview"},{"description","<@"+uid+">"},{"color",0x2ECC71},
                {"footer",{{"text","Local preview - based on saved membership checks"}}}};
    if (!matches.empty()) {
        std::string text; size_t shown=0;
        for (auto& match : matches) {
            std::string name;
            for (char ch : match["server_name"].get<std::string>()) {
                if (ch=='\n'||ch=='\r') { name+=' ';continue; }
                if (std::string("\\*_~`|>").find(ch)!=std::string::npos) name+='\\';
                name+=ch;
            }
            if (text.size()+name.size()+1>900) break;
            if (!text.empty()) text+='\n';text+=name;++shown;
        }
        if(shown<matches.size())text+="\n+ "+std::to_string(matches.size()-shown)+" more communities";
        embed["fields"]=Json::array({{{"name","Also part of"},{"value",text},{"inline",false}}});
    }
    return {{"user_id",uid},{"joining_server_id",sid},{"common_servers",matches},
        {"discord_message",{{"embeds",Json::array({embed})},{"allowed_mentions",{{"parse",Json::array()}}}}}};
}
void Database::apply_account(const Json& snapshot) {
    auto uid=id(snapshot.at("user_id"));auto username=snapshot.at("username").get<std::string>();
    auto captured=stamp(parse_time(snapshot.at("captured_at").get<std::string>()));
    if(username.empty()||wide(username).size()>100||parse_time(captured)>now()+300000000)
        throw std::runtime_error("Invalid account snapshot");
    const auto& guilds=snapshot.at("guilds");
    if(!guilds.is_array()||guilds.size()>20000)throw std::runtime_error("Invalid account server list");
    std::map<std::string,std::string> unique;
    for(auto& guild:guilds){auto sid=id(guild.at("server_id"));auto name=guild.at("server_name").get<std::string>();
        if(name.empty()||wide(name).size()>100||!unique.emplace(sid,name).second)throw std::runtime_error("Invalid or duplicate server in account snapshot");}
    execute(db_,"BEGIN IMMEDIATE");
    try {
        {Statement q(db_,"SELECT captured_at FROM own_accounts WHERE user_id=?");q.bind(1,uid);
         if(q.step()&&captured<q.text(0))throw std::runtime_error("Older account snapshot rejected");}
        {Statement q(db_,"INSERT INTO own_accounts VALUES (?,?,?) ON CONFLICT(user_id) DO UPDATE SET username=excluded.username,captured_at=excluded.captured_at");
         q.bind(1,uid);q.bind(2,username);q.bind(3,captured);q.step();}
        {Statement q(db_,"DELETE FROM account_servers WHERE user_id=?");q.bind(1,uid);q.step();}
        Statement q(db_,"INSERT INTO account_servers VALUES (?,?,?)");
        for(auto& [sid,name]:unique){sqlite3_reset(q.s);q.bind(1,uid);q.bind(2,sid);q.bind(3,name);q.step();}
        execute(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
void Database::forget_account(const std::string& user) {
    auto uid=id(user);execute(db_,"BEGIN IMMEDIATE");
    try {
        {Statement q(db_,"DELETE FROM account_servers WHERE user_id=?");q.bind(1,uid);q.step();}
        // Keep a marker to prevent older manual rosters resurrecting the forgotten account's matches.
        {Statement q(db_,"UPDATE own_accounts SET username='',captured_at='1970-01-01T00:00:00.000000+00:00' WHERE user_id=?");q.bind(1,uid);q.step();}
        execute(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
Json scan(Database& database,const Path& inbox) {
    std::filesystem::create_directories(inbox);std::vector<Path> paths;Json results=Json::array();
    for(auto& entry:std::filesystem::directory_iterator(inbox)) if(entry.is_regular_file()&&entry.path().extension()==L".json")paths.push_back(entry.path());
    std::sort(paths.begin(),paths.end());
    for(auto& path:paths) {
        try {results.push_back({{"file",utf8(path.filename().wstring())},{"results",database.apply(read_json(path))}});}
        catch(const std::exception& e){results.push_back({{"file",utf8(path.filename().wstring())},{"error",e.what()}});}
    }
    return results;
}
}
