#include "chat.h"
#include <winhttp.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include <cctype>
#include <limits>
#include <cmath>
#include <sstream>
#include <iomanip>
namespace sml {
namespace {
void cancelled(const std::atomic<bool>& cancel){if(cancel)throw std::runtime_error("Message collection cancelled; saved messages kept");}
void pause_for(double seconds,const std::atomic<bool>& cancel){
    auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(static_cast<int64_t>(std::clamp(seconds,0.0,3600.0)*1000));
    while(std::chrono::steady_clock::now()<end){cancelled(cancel);std::this_thread::sleep_for(std::chrono::milliseconds(50));}
}
struct HttpHandle {HINTERNET h{};explicit HttpHandle(HINTERNET h):h(h){if(!h)throw std::runtime_error("Could not open HTTP connection");}~HttpHandle(){WinHttpCloseHandle(h);}HttpHandle(const HttpHandle&)=delete;};
std::string lower(std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;}
std::string header(HINTERNET request,const wchar_t* name){DWORD bytes=0;WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,name,nullptr,&bytes,nullptr);if(!bytes)return {};std::wstring s(bytes/sizeof(wchar_t),0);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,name,s.data(),&bytes,nullptr))return {};s.resize(bytes/sizeof(wchar_t));return utf8(s);}
void exec(sqlite3* db,const char* sql){char* error{};if(sqlite3_exec(db,sql,nullptr,nullptr,&error)!=SQLITE_OK){std::string message=error?error:"Database error";sqlite3_free(error);throw std::runtime_error(message);}}
struct Query {
    sqlite3* db;sqlite3_stmt* s{};
    Query(sqlite3* db,const char* sql):db(db){if(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    ~Query(){sqlite3_finalize(s);}
    void bind(int n,const std::string& value){if(sqlite3_bind_text(s,n,value.c_str(),static_cast<int>(value.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
    bool step(){auto rc=sqlite3_step(s);if(rc!=SQLITE_ROW&&rc!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));return rc==SQLITE_ROW;}
    std::string text(int n){auto p=sqlite3_column_text(s,n);return p?reinterpret_cast<const char*>(p):"";}
};

uint64_t snow(const std::string& s){return std::stoull(id(s));}
bool has_guild(const Json& value){return value.contains("guild_id")&&!value["guild_id"].is_null();}
int dm_type(const Json& channel){
    if(!channel.is_object()||has_guild(channel)||!channel.contains("type")||!channel["type"].is_number_integer())throw std::runtime_error("Invalid DM channel; saved conversations kept");
    if(channel["type"]!=1&&channel["type"]!=3)throw std::runtime_error("Expected a DM or group DM; saved conversations kept");return channel["type"].get<int>();
}
std::string optional_name(const Json& value,const char* field){
    if(!value.contains(field)||value[field].is_null())return {};
    if(!value[field].is_string())throw std::runtime_error("Invalid DM recipient or conversation name");return value[field].get<std::string>();
}
std::string dm_name(const Json& channel){
    auto name=optional_name(channel,"name");std::string recipients;
    if(channel.contains("recipients")){
        if(!channel["recipients"].is_array())throw std::runtime_error("Invalid DM recipient list");
        std::set<std::string> seen;
        for(auto& recipient:channel["recipients"]){
            auto uid=id(recipient.at("id"));auto global=optional_name(recipient,"global_name"),username=optional_name(recipient,"username");
            if(!seen.insert(uid).second)throw std::runtime_error("Duplicate DM recipient");
            if(!recipients.empty())recipients+=", ";auto label=!global.empty()?global:username;recipients+=label.empty()?uid:label+" ("+uid+")";
        }
    }
    return !name.empty()?name:!recipients.empty()?recipients:(channel["type"]==3?"Group DM ":"DM ")+id(channel.at("id"));
}
}
struct ChatClient::Impl {
    std::string token;const std::atomic<bool>& cancel;ChatProgress progress;
    HttpHandle session,connection;std::chrono::steady_clock::time_point next{};
    Impl(const std::string& token,const std::atomic<bool>& cancel,ChatProgress progress):token(token),cancel(cancel),progress(std::move(progress)),
        session(WinHttpOpen(L"XADiscordExplorer/0.6",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)),
        connection(WinHttpConnect(session.h,L"discord.com",INTERNET_DEFAULT_HTTPS_PORT,0)) {
        if(token.empty()||token.size()>4096||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<33||c>126;}))throw std::runtime_error("Reconnect this account; invalid session token");
        WinHttpSetTimeouts(session.h,10000,10000,10000,10000);
    }
    ~Impl(){SecureZeroMemory(token.data(),token.size());}
};
ChatClient::ChatClient(const std::string& token,const std::atomic<bool>& cancel,ChatProgress progress):impl_(std::make_unique<Impl>(token,cancel,std::move(progress))){}
ChatClient::~ChatClient()=default;
Json ChatClient::get(const std::string& route){
    auto& c=*impl_;if(route.empty()||route[0]=='/'||route.find(":")!=std::string::npos||route.find("..")!=std::string::npos)throw std::runtime_error("Invalid Discord route");
    for(int attempt=0;attempt<6;++attempt){
        cancelled(c.cancel);auto wait=std::chrono::duration<double>(c.next-std::chrono::steady_clock::now()).count();if(wait>0)pause_for(wait,c.cancel);
        auto path=wide("/api/v10/"+route);HttpHandle request(WinHttpOpenRequest(c.connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
        DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
        auto auth=wide("Authorization: "+c.token+"\r\n");auto sent=WinHttpSendRequest(request.h,auth.c_str(),static_cast<DWORD>(auth.size()),nullptr,0,0,0);SecureZeroMemory(auth.data(),auth.size()*sizeof(wchar_t));
        if(!sent||!WinHttpReceiveResponse(request.h,nullptr))throw std::runtime_error("Discord request failed; saved messages kept");
        DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr))throw std::runtime_error("Unreadable Discord status");
        std::string body;char buffer[16384];DWORD received=0;
        do{cancelled(c.cancel);if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&received))throw std::runtime_error("Interrupted Discord response");body.append(buffer,received);if(body.size()>32*1024*1024)throw std::runtime_error("Discord response exceeded 32 MB");}while(received);
        auto data=Json::parse(body,nullptr,false);
        if(status==429){double seconds=1;if(data.is_object()&&data.contains("retry_after")&&data["retry_after"].is_number())seconds=data["retry_after"].get<double>();if(c.progress)c.progress("Discord rate limit: waiting before retry");pause_for(seconds+0.25,c.cancel);continue;}
        c.next=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        if(header(request.h,L"X-RateLimit-Remaining")=="0"){auto reset=header(request.h,L"X-RateLimit-Reset-After");try{c.next=std::chrono::steady_clock::now()+std::chrono::milliseconds(static_cast<int64_t>((std::clamp(std::stod(reset),0.0,3600.0)+0.1)*1000));}catch(...){} }
        // Search can request indexing before returning a page. Let the collection path retry cancellably.
        if(status==202&&(route.starts_with("guilds/")||route.starts_with("channels/"))&&route.find("/messages/search?")!=std::string::npos&&data.is_object()&&data.value("code",0)==110000)return data;
        if(status!=200){int code=data.is_object()?data.value("code",0):0;throw ChatError(static_cast<int>(status),code,status==401?"Session expired; reconnect the account":status==403?"Channel access denied (403); saved messages kept":"Discord returned HTTP "+std::to_string(status)+" (code "+std::to_string(code)+")");}
        if(data.is_discarded())throw std::runtime_error("Invalid Discord JSON response");return data;
    }
    throw ChatError(429,0,"Discord rate limit persisted; retry later");
}
void ChatClient::download(const std::string& url,const Path& path,int64_t max_bytes){
    auto& c=*impl_;cancelled(c.cancel);auto address=wide(url);URL_COMPONENTS parts{};parts.dwStructSize=sizeof(parts);parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=static_cast<DWORD>(-1);
    if(!WinHttpCrackUrl(address.c_str(),static_cast<DWORD>(address.size()),0,&parts)||parts.nScheme!=INTERNET_SCHEME_HTTPS||parts.nPort!=443)throw std::runtime_error("Attachment URL must use HTTPS");
    std::wstring host(parts.lpszHostName,parts.dwHostNameLength),route(parts.lpszUrlPath,parts.dwUrlPathLength);if(parts.dwExtraInfoLength)route.append(parts.lpszExtraInfo,parts.dwExtraInfoLength);
    if(host!=L"cdn.discordapp.com"&&host!=L"media.discordapp.net")throw std::runtime_error("Attachment host not supported");
    if(!route.starts_with(L"/attachments/")&&!route.starts_with(L"/ephemeral-attachments/"))throw std::runtime_error("Invalid attachment path");
    HttpHandle connection(WinHttpConnect(c.session.h,host.c_str(),443,0));HttpHandle request(WinHttpOpenRequest(connection.h,L"GET",route.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects));
    if(!WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0)||!WinHttpReceiveResponse(request.h,nullptr))throw std::runtime_error("Attachment request failed");
    DWORD status=0,bytes=sizeof(status);WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&bytes,nullptr);if(status!=200)throw std::runtime_error("Attachment HTTP "+std::to_string(status));
    std::filesystem::create_directories(path.parent_path());auto temp=path;temp+=L".part";
    try{std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Could not create attachment file");char buffer[32768];DWORD received=0;int64_t total=0;
        do{cancelled(c.cancel);if(!WinHttpReadData(request.h,buffer,sizeof(buffer),&received))throw std::runtime_error("Attachment download interrupted");total+=received;if(total>max_bytes)throw std::runtime_error("Attachment exceeds size limit");out.write(buffer,received);if(!out)throw std::runtime_error("Attachment write failed");}while(received);out.close();
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not finish attachment download");
    }catch(...){std::error_code ec;std::filesystem::remove(temp,ec);throw;}
}
namespace {
struct ParsedMessage {std::string text;Json attachments=Json::array();std::vector<std::string> detection;std::set<std::string> attachment_ids;size_t nodes=0;};
std::string string_field(const Json& value,const char* key){auto it=value.find(key);return it!=value.end()&&it->is_string()?it->get<std::string>():std::string();}
void append_text(std::string& target,const std::string& value){if(value.empty())return;if(!target.empty())target+='\n';target+=value;}
void parse_message(const Json& message,ParsedMessage& parsed,size_t depth){
    if(++parsed.nodes>64||depth>8){append_text(parsed.text,"[Forwarded content truncated]");return;}
    if(!message.is_object()){append_text(parsed.text,"[Forwarded content unavailable]");return;}
    auto text=string_field(message,"content");
    if(depth){
        auto embeds=message.find("embeds");if(embeds!=message.end()&&embeds->is_array())for(const auto& embed:*embeds)if(embed.is_object()){
            append_text(text,string_field(embed,"title"));append_text(text,string_field(embed,"description"));
            auto fields=embed.find("fields");if(fields!=embed.end()&&fields->is_array())for(const auto& field:*fields)if(field.is_object()){append_text(text,string_field(field,"name"));append_text(text,string_field(field,"value"));}
        }
        append_text(parsed.text,"[Forwarded message]");
    }
    append_text(parsed.text,text);if(!text.empty())parsed.detection.push_back(text);
    auto attachments=message.find("attachments");if(attachments!=message.end()&&attachments->is_array())for(const auto& a:*attachments)if(a.is_object()){
        auto aid=string_field(a,"id");if(aid.empty())continue;
        if(parsed.attachment_ids.insert(aid).second){parsed.attachments.push_back(a);if(depth)append_text(parsed.text,"Forwarded attachment: "+string_field(a,"filename"));}
    }
    auto snapshots=message.find("message_snapshots");if(snapshots==message.end()||snapshots->is_null()||(snapshots->is_array()&&snapshots->empty())){
        auto reference=message.find("message_reference");if(reference!=message.end()&&reference->is_object()&&reference->contains("type")&&(*reference)["type"]==1)append_text(parsed.text,"[Forwarded content unavailable]");return;
    }
    if(!snapshots->is_array()){append_text(parsed.text,"[Forwarded content unavailable]");return;}
    for(const auto& snapshot:*snapshots){
        if(parsed.nodes>=64){append_text(parsed.text,"[Forwarded content truncated]");break;}
        if(!snapshot.is_object()||!snapshot.contains("message")){++parsed.nodes;append_text(parsed.text,"[Forwarded content unavailable]");continue;}
        parse_message(snapshot["message"],parsed,depth+1);
    }
}
ParsedMessage parsed_message(const Json& message){ParsedMessage parsed;parse_message(message,parsed,0);return parsed;}
}
std::string chat_message_text(const Json& message){return parsed_message(message).text;}
Json chat_message_attachments(const Json& message){return parsed_message(message).attachments;}
std::string chat_message_suggestion(const Json& message,const Json& patterns){
    if(message.at("author").value("bot",false))return {};
    for(const auto& text:parsed_message(message).detection){auto reason=suggestion_reason(text,patterns);if(!reason.empty())return reason;}return {};
}
void ChatStore::reindex_forwards(){
    exec(db_,"BEGIN IMMEDIATE");try{
        Query version(db_,"SELECT value FROM chat_settings WHERE key='forward_content_version'");
        if(version.step()&&version.text(0)=="1"){exec(db_,"COMMIT");return;}
        auto patterns=detection_patterns();Query rows(db_,"SELECT id,raw FROM chat_messages WHERE instr(raw,'\"message_snapshots\"')>0");
        Query update(db_,"UPDATE chat_messages SET content=?,suggestion=? WHERE id=?");
        while(rows.step()){auto message=Json::parse(rows.text(1));sqlite3_reset(update.s);update.bind(1,chat_message_text(message));update.bind(2,chat_message_suggestion(message,patterns));update.bind(3,rows.text(0));update.step();}
        exec(db_,"INSERT INTO chat_settings VALUES('forward_content_version','1') ON CONFLICT(key) DO UPDATE SET value='1'");exec(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
ChatStore::ChatStore(const Path& path){
    std::filesystem::create_directories(path.parent_path());if(sqlite3_open(utf8(path.wstring()).c_str(),&db_)!=SQLITE_OK){if(db_)sqlite3_close(db_);throw std::runtime_error("Cannot open message database");}
    try{sqlite3_busy_timeout(db_,5000);exec(db_,"PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS chat_settings(key TEXT PRIMARY KEY,value TEXT NOT NULL); CREATE TABLE IF NOT EXISTS chat_channels(id TEXT PRIMARY KEY,guild TEXT NOT NULL,name TEXT NOT NULL,type INTEGER NOT NULL,parent TEXT NOT NULL); CREATE TABLE IF NOT EXISTS chat_messages(id TEXT PRIMARY KEY,guild TEXT NOT NULL,channel TEXT NOT NULL,author TEXT NOT NULL,username TEXT NOT NULL,sent TEXT NOT NULL,content TEXT NOT NULL,raw TEXT NOT NULL,suggestion TEXT NOT NULL,review TEXT NOT NULL DEFAULT 'New',deleted INTEGER NOT NULL DEFAULT 0,seen_run TEXT NOT NULL); CREATE INDEX IF NOT EXISTS chat_by_channel ON chat_messages(channel,id); CREATE INDEX IF NOT EXISTS chat_by_guild ON chat_messages(guild,sent); CREATE TABLE IF NOT EXISTS chat_state(channel TEXT NOT NULL,account TEXT NOT NULL,lower_id TEXT NOT NULL,upper_id TEXT NOT NULL,checked TEXT NOT NULL,PRIMARY KEY(channel,account)); CREATE TABLE IF NOT EXISTS chat_files(message TEXT NOT NULL,attachment TEXT NOT NULL,path TEXT NOT NULL,status TEXT NOT NULL,PRIMARY KEY(message,attachment));");reindex_forwards();}catch(...){sqlite3_close(db_);throw;}
}
ChatStore::~ChatStore(){sqlite3_close(db_);}
void ChatStore::channels(const std::string& guild,const Json& rows){auto scope=guild=="@me"?guild:id(guild);exec(db_,"BEGIN IMMEDIATE");try{Query q(db_,"INSERT INTO chat_channels VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET name=excluded.name,type=excluded.type,parent=excluded.parent");for(auto& row:rows){if(scope=="@me")dm_type(row);sqlite3_reset(q.s);q.bind(1,id(row.at("id")));q.bind(2,scope);q.bind(3,row.value("name",std::string("Unnamed")));q.bind(4,std::to_string(row.at("type").get<int>()));q.bind(5,row.value("parent_id",Json()).is_string()?row["parent_id"].get<std::string>():"");q.step();}exec(db_,"COMMIT");}catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}}
Json ChatStore::channels(const std::string& guild,const std::string& account){Query q(db_,"SELECT c.id,c.name,c.type,COALESCE(s.checked,''),(SELECT COUNT(*) FROM chat_messages m WHERE m.channel=c.id),c.parent FROM chat_channels c LEFT JOIN chat_state s ON c.id=s.channel AND s.account=? WHERE c.guild=? ORDER BY c.name COLLATE NOCASE,c.id");q.bind(1,account);q.bind(2,guild);Json rows=Json::array();while(q.step())rows.push_back({{"id",q.text(0)},{"name",q.text(1)},{"type",sqlite3_column_int(q.s,2)},{"checked",q.text(3)},{"messages",sqlite3_column_int64(q.s,4)},{"parent_id",q.text(5)}});return rows;}
Json ChatStore::state(const std::string& channel,const std::string& account){Query q(db_,"SELECT lower_id,upper_id,checked FROM chat_state WHERE channel=? AND account=?");q.bind(1,channel);q.bind(2,account);if(!q.step())return Json::object();return {{"lower",q.text(0)},{"upper",q.text(1)},{"checked",q.text(2)}};}
std::string ChatStore::latest_message(const std::string& channel){Query q(db_,"SELECT id FROM chat_messages WHERE channel=? ORDER BY CAST(id AS INTEGER) DESC LIMIT 1");q.bind(1,channel);return q.step()?q.text(0):"1";}
Json default_detection_patterns(){static const Json value=Json::parse(
#include "detection_defaults.inc"
);return value;}
std::string suggestion_reason(const std::string& text,const Json& patterns){
    auto s=lower(text);auto first=s.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};s=s.substr(first);if(s.size()<4)return {};
    double suggestion=0,bug=0;std::vector<std::string> matched;
    for(auto& p:patterns){auto phrase=lower(p.at("phrase").get<std::string>());bool hit=phrase.starts_with('^')?s.starts_with(phrase.substr(1)):s.find(phrase)!=std::string::npos;if(!hit)continue;
        (p.at("kind")=="bug"?bug:suggestion)+=p.at("weight").get<double>();matched.push_back(p.at("phrase"));}
    if(s.find('?')!=std::string::npos&&(s.find("can ")!=std::string::npos||s.find("could ")!=std::string::npos||s.find("would ")!=std::string::npos)&&suggestion>0)suggestion+=0.15;
    if((s.find("when i ")!=std::string::npos||s.find("after i ")!=std::string::npos)&&bug>0)bug+=0.15;
    if(std::max(suggestion,bug)<0.5)return {};std::ostringstream reason;reason<<(bug>suggestion?"bug":"suggestion")<<" ("<<std::fixed<<std::setprecision(2)<<std::min(1.0,std::max(suggestion,bug))<<"): ";
    for(size_t i=0;i<matched.size();++i){if(i)reason<<", ";reason<<matched[i];}return reason.str();
}
Json ChatStore::detection_patterns(){Query q(db_,"SELECT value FROM chat_settings WHERE key='detection_patterns'");if(q.step())return Json::parse(q.text(0));return default_detection_patterns();}
void ChatStore::set_detection_patterns(const Json& patterns){
    if(!patterns.is_array()||patterns.empty()||patterns.size()>500)throw std::runtime_error("Supply 1 to 500 detection templates");
    for(auto& p:patterns){if(!p.is_object()||!p.contains("phrase")||!p["phrase"].is_string()||p["phrase"].get<std::string>().empty()||p["phrase"].get<std::string>()=="^"||p["phrase"].get<std::string>().size()>200||!p.contains("kind")||(p["kind"]!="suggestion"&&p["kind"]!="bug")||!p.contains("weight")||!p["weight"].is_number()||!std::isfinite(p["weight"].get<double>())||p["weight"].get<double>()<0||p["weight"].get<double>()>1)throw std::runtime_error("Each template needs phrase, kind (suggestion/bug), and weight 0 to 1");}
    exec(db_,"BEGIN IMMEDIATE");try{Query setting(db_,"INSERT INTO chat_settings VALUES('detection_patterns',?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");setting.bind(1,patterns.dump());setting.step();Query rows(db_,"SELECT id,content,raw FROM chat_messages");Query update(db_,"UPDATE chat_messages SET suggestion=? WHERE id=?");
        while(rows.step()){auto m=Json::parse(rows.text(2));auto reason=chat_message_suggestion(m,patterns);sqlite3_reset(update.s);update.bind(1,reason);update.bind(2,rows.text(0));update.step();}exec(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}

void ChatStore::save_messages(const std::string& guild,const std::string& channel,const Json& messages,const std::string& run){
    auto patterns=detection_patterns();exec(db_,"BEGIN IMMEDIATE");try{Query q(db_,"INSERT INTO chat_messages(id,guild,channel,author,username,sent,content,raw,suggestion,seen_run) VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET username=excluded.username,sent=excluded.sent,content=excluded.content,raw=excluded.raw,suggestion=excluded.suggestion,deleted=0,seen_run=excluded.seen_run");
        for(auto& m:messages){auto mid=id(m.at("id"));if(id(m.at("channel_id"))!=channel||(guild=="@me"&&has_guild(m)))throw std::runtime_error("Message channel mismatch");auto& a=m.at("author");auto content=chat_message_text(m);auto reason=chat_message_suggestion(m,patterns);parse_time(m.at("timestamp").get<std::string>());
            sqlite3_reset(q.s);q.bind(1,mid);q.bind(2,guild);q.bind(3,channel);q.bind(4,id(a.at("id")));q.bind(5,a.value("username",std::string()));q.bind(6,m.at("timestamp"));q.bind(7,content);q.bind(8,m.dump());q.bind(9,reason);q.bind(10,run);q.step();}exec(db_,"COMMIT");
    }catch(...){sqlite3_exec(db_,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
}
std::vector<std::string> ChatStore::missing(const std::string& channel,const std::string& from,const std::string& to,const std::string& run,const std::string& author){Query q(db_,"SELECT id FROM chat_messages WHERE channel=? AND CAST(id AS INTEGER)>=CAST(? AS INTEGER) AND CAST(id AS INTEGER)<CAST(? AS INTEGER) AND seen_run<>? AND deleted=0 AND (?='' OR author=?)");q.bind(1,channel);q.bind(2,from);q.bind(3,to);q.bind(4,run);q.bind(5,author);q.bind(6,author);std::vector<std::string> rows;while(q.step())rows.push_back(q.text(0));return rows;}
void ChatStore::deleted(const std::string& message){Query q(db_,"UPDATE chat_messages SET deleted=1 WHERE id=?");q.bind(1,message);q.step();}
void ChatStore::checkpoint(const std::string& channel,const std::string& account,const std::string& from,const std::string& to){Query q(db_,"INSERT INTO chat_state VALUES(?,?,?,?,?) ON CONFLICT(channel,account) DO UPDATE SET lower_id=CASE WHEN CAST(excluded.lower_id AS INTEGER)<=CAST(chat_state.upper_id AS INTEGER) THEN CAST(MIN(CAST(chat_state.lower_id AS INTEGER),CAST(excluded.lower_id AS INTEGER)) AS TEXT) ELSE excluded.lower_id END,upper_id=excluded.upper_id,checked=excluded.checked");q.bind(1,channel);q.bind(2,account);q.bind(3,from);q.bind(4,to);q.bind(5,stamp(now()));q.step();}
bool ChatStore::saved_attachment(const std::string& message,const std::string& attachment,const Path& path,int64_t max_bytes){Query q(db_,"SELECT path FROM chat_files WHERE message=? AND attachment=?");q.bind(1,message);q.bind(2,attachment);if(!q.step()||q.text(0)!=utf8(path.wstring()))return false;std::error_code error;auto size=std::filesystem::file_size(path,error);return !error&&size<=static_cast<uint64_t>(max_bytes);}
void ChatStore::attachment(const std::string& message,const std::string& attachment,const std::string& path,const std::string& status){Query q(db_,"INSERT INTO chat_files VALUES(?,?,?,?) ON CONFLICT(message,attachment) DO UPDATE SET path=CASE WHEN excluded.path='' THEN chat_files.path ELSE excluded.path END,status=excluded.status");q.bind(1,message);q.bind(2,attachment);q.bind(3,path);q.bind(4,status);q.step();}
Json ChatStore::messages(const std::string& guild,const std::string& channel,const std::string& search,bool suggestions,bool deleted_only,size_t offset,const std::string& author){
    std::string where=" FROM chat_messages WHERE guild=? AND (?='' OR channel=?) AND (instr(lower(content),lower(?))>0 OR instr(lower(username),lower(?))>0 OR instr(id,?)>0) AND (?='' OR author=?)";if(suggestions)where+=" AND suggestion<>''";if(deleted_only)where+=" AND deleted=1";
    auto bind=[&](Query& q){q.bind(1,guild);q.bind(2,channel);q.bind(3,channel);q.bind(4,search);q.bind(5,search);q.bind(6,search);q.bind(7,author);q.bind(8,author);};Query total(db_,("SELECT COUNT(*)"+where).c_str());bind(total);total.step();
    Query q(db_,("SELECT id,username,sent,content,suggestion,review,deleted,channel"+where+" ORDER BY sent DESC,id DESC LIMIT 100 OFFSET ?").c_str());bind(q);q.bind(9,std::to_string(offset));Json rows=Json::array();while(q.step())rows.push_back({{"id",q.text(0)},{"username",q.text(1)},{"sent",q.text(2)},{"content",q.text(3)},{"suggestion",q.text(4)},{"review",q.text(5)},{"deleted",sqlite3_column_int(q.s,6)!=0},{"channel",q.text(7)}});return {{"total",sqlite3_column_int64(total.s,0)},{"rows",rows}};
}
Json ChatStore::message_location(const std::string& guild,const std::string& message){
    // Count preceding rows using the exact same TEXT ordering as messages(), without search/review filters.
    Query q(db_,"SELECT target.channel,(SELECT COUNT(*) FROM chat_messages m WHERE m.guild=target.guild AND m.channel=target.channel AND (m.sent>target.sent OR (m.sent=target.sent AND m.id>target.id))) FROM chat_messages target WHERE target.guild=? AND target.id=?");
    q.bind(1,guild);q.bind(2,message);if(!q.step())return Json();return {{"channel",q.text(0)},{"position",sqlite3_column_int64(q.s,1)}};
}
Json ChatStore::message(const std::string& message){Query q(db_,"SELECT raw,deleted,suggestion,review,guild FROM chat_messages WHERE id=?");q.bind(1,message);if(!q.step())return Json();Json result={{"message",Json::parse(q.text(0))},{"deleted",sqlite3_column_int(q.s,1)!=0},{"suggestion",q.text(2)},{"review",q.text(3)},{"guild",q.text(4)},{"files",Json::array()}};Query f(db_,"SELECT attachment,path,status FROM chat_files WHERE message=?");f.bind(1,message);while(f.step())result["files"].push_back({{"id",f.text(0)},{"path",f.text(1)},{"status",f.text(2)}});result["parsed_content"]=chat_message_text(result["message"]);return result;}
void ChatStore::review(const std::string& message,const std::string& state){if(state!="New"&&state!="Keep"&&state!="Dismiss")throw std::runtime_error("Invalid suggestion state");Query q(db_,"UPDATE chat_messages SET review=? WHERE id=?");q.bind(1,state);q.bind(2,message);q.step();}
std::string chat_time_id(Time at){constexpr int64_t epoch=1420070400000LL;auto ms=at/1000;if(ms<epoch)return "1";auto delta=static_cast<uint64_t>(ms-epoch);if(delta>(static_cast<uint64_t>(INT64_MAX)>>22))throw std::runtime_error("Date outside supported Discord range");return std::to_string(delta<<22);}
std::string chat_date_id(const std::string& date){if(date.size()!=10||date[4]!='-'||date[7]!='-')throw std::runtime_error("From date must be YYYY-MM-DD (UTC)");auto at=parse_time(date+"T00:00:00+00:00");if(stamp(at).substr(0,10)!=date||at>now())throw std::runtime_error("From date must be a valid date no later than today");return chat_time_id(at);}
std::string attachment_type(const std::string& filename){auto dot=filename.find_last_of('.');auto ext=dot==std::string::npos?std::string():lower(filename.substr(dot+1));if(ext=="png"||ext=="jpg"||ext=="jpeg"||ext=="gif"||ext=="webp"||ext=="bmp"||ext=="avif")return "pictures";if(ext=="zip"||ext=="txt"||ext=="lua"||ext=="json"||ext=="py")return ext;return "files";}
bool attachment_allowed(const Json& a,const ChatOptions& o){return a.contains("size")&&a["size"].is_number_integer()&&a["size"].get<int64_t>()>=0&&a["size"].get<int64_t>()<=o.max_bytes&&o.types.contains(attachment_type(a.value("filename",std::string())));}
Json list_chat_channels(const ChatGet& get,const std::string& guild,const std::atomic<bool>& cancel,const ChatProgress& progress,bool include_threads){
    id(guild);auto base=get("guilds/"+guild+"/channels");if(!base.is_array())throw std::runtime_error("Invalid channel list");Json rows=Json::array(),warnings=Json::array();std::set<std::string> seen;
    auto add=[&](const Json& items){for(auto& c:items){int type=c.value("type",-1);if(type==0||type==2||type==5||type==10||type==11||type==12||type==13||type==15||type==16)if(seen.insert(id(c.at("id"))).second)rows.push_back({{"id",c.at("id")},{"name",c.value("name",std::string("Unnamed"))},{"type",type},{"parent_id",c.value("parent_id",Json())}});}};add(base);
    if(!include_threads)return {{"channels",rows},{"warnings",warnings}};
    // Like DiscordChatExporter, user sessions discover threads through channel search.
    for(auto& channel:base){int type=channel.value("type",-1);if(type!=0&&type!=5&&type!=15&&type!=16)continue;auto cid=id(channel.at("id"));if(progress)progress("Finding threads in #"+channel.value("name",cid));
        for(bool archived:{false,true}){
            size_t offset=0;
            try{for(;;){cancelled(cancel);auto page=get("channels/"+cid+"/threads/search?sort_by=last_message_time&sort_order=desc&archived="+(archived?"true":"false")+"&offset="+std::to_string(offset));auto& threads=page.at("threads");if(!threads.is_array())throw std::runtime_error("Invalid thread list");add(threads);if(!page.value("has_more",false))break;
                    if(threads.empty()||offset>1000000)throw std::runtime_error("Thread pagination stalled");offset+=threads.size();
                }}catch(const ChatError& e){if(e.status==401||e.status==429)throw;warnings.push_back("Threads in #"+channel.value("name",cid)+": "+e.what());}
        }
    }
    return {{"channels",rows},{"warnings",warnings}};
}
Json list_dm_channels(const ChatGet& get,const std::atomic<bool>& cancel,const ChatProgress& progress){
    cancelled(cancel);if(progress)progress("Loading active DMs and group DMs");auto response=get("users/@me/channels");cancelled(cancel);
    if(!response.is_array())throw std::runtime_error("Invalid DM conversation list; saved conversations kept");
    Json rows=Json::array(),warnings=Json::array();std::set<std::string> seen;
    for(auto& channel:response){
        cancelled(cancel);if(!channel.is_object()||!channel.contains("type")||!channel["type"].is_number_integer())throw std::runtime_error("Invalid DM channel; saved conversations kept");
        auto cid=id(channel.at("id"));
        if(!seen.insert(cid).second)throw std::runtime_error("Duplicate DM channel; saved conversations kept");
        if(channel["type"]!=1&&channel["type"]!=3){warnings.push_back("Skipped unsupported conversation type "+channel["type"].dump()+" (channel "+cid+")");continue;}
        auto type=dm_type(channel);
        rows.push_back({{"id",cid},{"name",dm_name(channel)},{"type",type},{"parent_id",nullptr}});
    }
    return {{"channels",rows},{"warnings",warnings}};
}
std::string chat_from_id(const ChatOptions& options,bool dms){
    if(!options.live_from.empty()){id(options.live_from);if(dms||!options.author_id.empty()||options.check_deletions)throw std::runtime_error("Live capture requires server channels, all senders and no deletion scan");return options.live_from;}
    if(!dms&&options.check_deletions&&options.from_date.empty())throw std::runtime_error("Check for deletion requires a From date (YYYY-MM-DD UTC)");
    if(!dms&&!options.author_id.empty()&&!options.check_deletions)return "1";
    return options.from_date.empty()?"1":chat_date_id(options.from_date);
}
Json collect_chat(ChatStore& store,const ChatGet& get,const ChatDownload& download,const Path& files,const std::string& account,const std::string& guild,const std::vector<std::string>& channels,const ChatOptions& options,const std::atomic<bool>& cancel,const ChatProgress& progress){
    const bool dms=guild=="@me";
    const bool live=!options.live_from.empty();
    auto author=options.author_id.empty()?std::string():id(options.author_id);
    auto from=chat_from_id(options,dms),upper=chat_time_id(now()),run=stamp(now());id(account);if(!dms)id(guild);if(options.max_bytes<1||options.max_bytes>1024LL*1024*1024)throw std::runtime_error("File size limit must be 1 byte to 1 GiB");Json results=Json::array();std::set<std::string> unique;
    for(const auto& channel:channels){id(channel);if(!unique.insert(channel).second)continue;if(cancel)break;
        int64_t collected=0,scanned=0,downloaded=0,deleted_count=0,file_errors=0;auto from_id=from;Json result={{"channel_id",channel},{"checkpoint_updated",false}};
        try{
            if(live){auto latest=store.latest_message(channel);from_id=latest=="1"?options.live_from:latest;}
            // A user-only scan cannot establish that a channel's other messages were saved.
            if(!live&&author.empty()&&options.since&&!options.check_deletions){
                if(!dms&&options.from_date.empty())from_id=store.latest_message(channel);
                else if(dms){auto state=store.state(channel,account);if(!state.empty()&&snow(from)>=snow(state.at("lower"))&&snow(from)<=snow(state.at("upper")))from_id=state.at("upper");}
            }
            // Validate access before walking messages. Missing channels/access never become deletions.
            auto metadata=get("channels/"+channel);cancelled(cancel);if(id(metadata.at("id"))!=channel||(!dms&&id(metadata.at("guild_id"))!=guild))throw std::runtime_error("Channel/server mismatch");
            int type=dms?dm_type(metadata):metadata.value("type",0);if(type==15||type==16)throw std::runtime_error("Forum/media container: select its listed threads to collect messages");
            std::string before=upper,after=from_id;bool done=false;
            auto label=dms?dm_name(metadata):metadata.value("name",channel);
            auto report_progress=[&]{if(progress)progress(std::string(dms?"Conversation ":"Channel ")+std::to_string(results.size()+1)+"/"+std::to_string(channels.size())+" | "+std::to_string(collected)+(author.empty()?" messages":" search matches from user "+author+((dms||options.check_deletions)&&!options.from_date.empty()?" (from "+options.from_date+")":" (all dates)"))+" | "+(dms?"":"#")+label);};
            while(!done){cancelled(cancel);report_progress();
                uint64_t oldest=snow(before);Json save=Json::array();std::set<std::string> ids;
                if(live){
                    auto page=get("channels/"+channel+"/messages?limit=100&after="+after);cancelled(cancel);
                    if(!page.is_array()||page.size()>100)throw std::runtime_error("Invalid live message page");if(page.empty())break;
                    uint64_t newest=snow(after);
                    for(auto& m:page){auto mid=id(m.at("id"));auto numeric=snow(mid);if(id(m.at("channel_id"))!=channel||numeric<=snow(after)||!ids.insert(mid).second)throw std::runtime_error("Live message pagination did not advance or channel mismatch");++scanned;
                        if(numeric>=snow(upper)){done=true;continue;}newest=std::max(newest,numeric);save.push_back(m);
                    }
                    // Persist each forward page before requesting the next, so retrying a partial catch-up cannot skip a gap.
                    after=std::to_string(newest);
                }else if(author.empty()){
                    auto page=get("channels/"+channel+"/messages?limit=100&before="+before);cancelled(cancel);if(!page.is_array()||page.size()>100)throw std::runtime_error("Invalid message page");if(page.empty())break;
                    for(auto& m:page){auto mid=id(m.at("id"));auto numeric=snow(mid);if(dms&&(id(m.at("channel_id"))!=channel||has_guild(m)))throw std::runtime_error("DM message channel mismatch");if(numeric>=snow(before)||!ids.insert(mid).second)throw std::runtime_error("Message pagination did not advance");oldest=std::min(oldest,numeric);++scanned;if(numeric<snow(from_id)){done=true;continue;}save.push_back(m);}
                }else{
                    // Keyset paging stays below Discord's offset limit, even for users with over 10,000 hits.
                    const auto route=(dms?"channels/"+channel:"guilds/"+guild)+"/messages/search?author_id="+author+(dms?std::string():"&channel_id="+channel)+"&sort_by=timestamp&sort_order=desc&max_id="+before+"&offset=0&limit=25"+(dms?(snow(from)>1?"&min_id="+std::to_string(snow(from)-1):std::string()):"&include_nsfw=true");
                    Json page;bool ready=false;
                    for(int attempt=0;attempt<6;++attempt){
                        cancelled(cancel);page=get(route+(attempt?"&attempts="+std::to_string(attempt):std::string()));cancelled(cancel);
                        if(!page.is_object())throw std::runtime_error("Invalid Discord search response; saved messages kept");
                        if(page.value("code",0)!=110000){ready=true;break;}
                        if(attempt==5)break;
                        double delay=1.0;if(page.contains("retry_after")){if(!page["retry_after"].is_number())throw std::runtime_error("Invalid search indexing retry delay");delay=page["retry_after"].get<double>();}
                        if(!std::isfinite(delay)||delay<0||delay>60)throw std::runtime_error("Discord search indexing is not ready; retry collection later");
                        if(progress)progress("Discord search indexing: waiting before retry for user "+author);
                        pause_for(delay==0?5.0:std::max(delay,0.05),cancel);
                    }
                    if(!ready)throw std::runtime_error("Discord search indexing did not finish; retry collection later (saved messages kept)");
                    if(page.contains("doing_deep_historical_index")&&(!page["doing_deep_historical_index"].is_boolean()||page["doing_deep_historical_index"].get<bool>()))throw std::runtime_error("Discord is indexing older search results; collection is incomplete, retry later");
                    if(!page.contains("total_results")||!page["total_results"].is_number_integer()||page["total_results"].get<int64_t>()<0||!page.contains("messages")||!page["messages"].is_array()||page["messages"].size()>25)throw std::runtime_error("Invalid Discord search page; saved messages kept");
                    auto total=page["total_results"].get<int64_t>();auto& groups=page["messages"];
                    if(groups.empty()){if(total!=0)throw std::runtime_error("Discord returned incomplete search results; retry later");break;}
                    for(auto& group:groups){
                        if(!group.is_array()||group.empty())throw std::runtime_error("Invalid Discord search message group");
                        for(auto& m:group){
                            if(!m.is_object()||!m.contains("hit")||!m["hit"].is_boolean())throw std::runtime_error("Invalid Discord search hit marker");
                            if(!m["hit"].get<bool>())continue;
                            auto mid=id(m.at("id"));auto numeric=snow(mid);
                            auto message_channel=id(m.at("channel_id"));
                            if(id(m.at("author").at("id"))!=author||(dms?(has_guild(m)||message_channel!=channel):(m.contains("guild_id")&&id(m.at("guild_id"))!=guild)))throw std::runtime_error("Discord search returned a different conversation, server or author; saved messages kept");
                            if(numeric>=snow(before)||!ids.insert(mid).second)throw std::runtime_error("Discord search pagination did not advance");
                            oldest=std::min(oldest,numeric);
                            if(numeric<snow(from)){done=true;continue;}
                            // A parent-channel search can also include its threads; only checked exact channels are saved.
                            if(message_channel==channel)save.push_back(m);
                        }
                    }
                    if(ids.empty()||ids.size()>25||static_cast<int64_t>(ids.size())>total)throw std::runtime_error("Discord returned incomplete search matches; retry later");
                    // Search omits reactions. Retain any reactions captured by earlier full-history collection.
                    for(auto& m:save)if(!m.contains("reactions")){auto cached=store.message(m["id"]);if(!cached.is_null()&&cached["message"].contains("reactions"))m["reactions"]=cached["message"]["reactions"];}
                    scanned+=static_cast<int64_t>(ids.size());
                }
                cancelled(cancel);store.save_messages(guild,channel,save,run);collected+=static_cast<int64_t>(save.size());
                for(auto& m:save)for(auto& a:chat_message_attachments(m)){
                    cancelled(cancel);auto mid=id(m["id"]),aid=id(a.at("id"));auto filename=a.value("filename",std::string());
                    if(!attachment_allowed(a,options)){store.attachment(mid,aid,"","Filtered by type or size");continue;}
                    auto ext=Path(wide(filename)).extension().wstring();if(ext.size()>12||!std::all_of(ext.begin(),ext.end(),[](wchar_t c){return c==L'.'||(c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||(c>=L'0'&&c<=L'9');}))ext=L".bin";
                    auto path=files/wide(guild)/wide(channel)/(wide(mid+"-"+aid)+ext);
                    try{if(!store.saved_attachment(mid,aid,path,options.max_bytes)){download(a.at("url"),path,options.max_bytes);++downloaded;}store.attachment(mid,aid,utf8(path.wstring()),"Saved");}
                    catch(const std::exception& e){if(cancel)throw;store.attachment(mid,aid,"",e.what());++file_errors;}
                }
                before=std::to_string(oldest);
            }
            report_progress();
            cancelled(cancel);
            if(options.check_deletions){for(auto& missing:store.missing(channel,from,upper,run,author)){
                cancelled(cancel);if(progress)progress("Verifying missing message "+missing);
                auto save_verified=[&](const Json& m){cancelled(cancel);if((dms&&(id(m.at("id"))!=missing||id(m.at("channel_id"))!=channel||has_guild(m)))||(!author.empty()&&(id(m.at("id"))!=missing||id(m.at("channel_id"))!=channel||id(m.at("author").at("id"))!=author)))throw std::runtime_error("Deletion verification target mismatch; saved messages kept");store.save_messages(guild,channel,Json::array({m}),run);};
                try{auto m=get("channels/"+channel+"/messages/"+missing);save_verified(m);}
                catch(const ChatError& e){
                    cancelled(cancel);
                    if(e.status==404&&e.code==10008){store.deleted(missing);++deleted_count;}
                    else if(e.status==403){
                        // User sessions can read history while the single-message endpoint is forbidden.
                        // A nonempty around page proves history is readable and includes the target if it exists.
                        auto nearby=get("channels/"+channel+"/messages?limit=100&around="+missing);
                        cancelled(cancel);
                        if(!nearby.is_array()||nearby.size()>100)throw std::runtime_error("Invalid deletion verification page; saved messages kept");
                        if(nearby.empty())throw std::runtime_error("Deletion unverified: history is empty or unreadable and the individual message check was denied; saved messages kept");
                        bool found=false;std::set<std::string> nearby_ids;
                        for(auto& m:nearby){auto mid=id(m.at("id"));if(id(m.at("channel_id"))!=channel||(dms&&has_guild(m))||!nearby_ids.insert(mid).second)throw std::runtime_error("Invalid deletion verification messages; saved messages kept");if(mid==missing)found=true;}
                        if(found){for(auto& m:nearby)if(id(m.at("id"))==missing)save_verified(m);}
                        else{store.deleted(missing);++deleted_count;}
                    }else throw;
                }
            }}
            cancelled(cancel);if(author.empty()&&!live){store.checkpoint(channel,account,from_id,upper);result["checkpoint_updated"]=true;}result["status"]="complete";
        }catch(const std::exception& e){result["status"]=cancel?"cancelled":"failed";result["error"]=e.what();}
        result["messages_received"]=collected;result["messages_scanned"]=scanned;result["files_downloaded"]=downloaded;result["file_errors"]=file_errors;result["deletions_confirmed"]=deleted_count;results.push_back(result);
        if(cancel)break;if(result.value("error",std::string()).find("Session expired")!=std::string::npos||result.value("error",std::string()).find("rate limit persisted")!=std::string::npos)break;
    }
    return {{"results",results},{"selected_channels",channels.size()},{"processed_channels",results.size()},{"cancelled",cancel.load()},{"author_id",author},{"all_history",dms?options.from_date.empty():(!author.empty()&&!options.check_deletions)}};
}
}
