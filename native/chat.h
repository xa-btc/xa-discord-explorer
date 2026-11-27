#pragma once
#include "core.h"
#include <atomic>
#include <functional>
#include <memory>
#include <set>
namespace sml {
struct ChatOptions {
    std::string from_date;
    std::string author_id;
    // Fixed start of a live session; empty for manual history collection.
    std::string live_from;
    bool since=true,check_deletions=false;
    std::set<std::string> types={"pictures","zip","txt","lua","json","py","files"};
    int64_t max_bytes=10*1024*1024;
};
struct ChatError:std::runtime_error {
    int status,code;
    ChatError(int status,int code,const std::string& message):std::runtime_error(message),status(status),code(code){}
};
using ChatProgress=std::function<void(const std::string&)>;
using ChatGet=std::function<Json(const std::string&)>;
using ChatDownload=std::function<void(const std::string&,const Path&,int64_t)>;
class ChatClient {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    ChatClient(const std::string& token,const std::atomic<bool>& cancel,ChatProgress progress={});
    ~ChatClient();
    Json get(const std::string& route);
    void download(const std::string& url,const Path& path,int64_t max_bytes);
};
class ChatStore {
    sqlite3* db_{};
    void reindex_forwards();
public:
    explicit ChatStore(const Path& path);
    ~ChatStore();
    ChatStore(const ChatStore&)=delete;
    void channels(const std::string& guild,const Json& rows);
    Json channels(const std::string& guild,const std::string& account);
    Json state(const std::string& channel,const std::string& account);
    std::string latest_message(const std::string& channel);
    void save_messages(const std::string& guild,const std::string& channel,const Json& messages,const std::string& run);
    std::vector<std::string> missing(const std::string& channel,const std::string& lower,const std::string& upper,const std::string& run,const std::string& author="");
    void deleted(const std::string& message);
    void checkpoint(const std::string& channel,const std::string& account,const std::string& lower,const std::string& upper);
    bool saved_attachment(const std::string& message,const std::string& attachment,const Path& path,int64_t max_bytes);
    void attachment(const std::string& message,const std::string& attachment,const std::string& path,const std::string& status);
    Json messages(const std::string& guild,const std::string& channel,const std::string& query,bool suggestions,bool deleted_only,size_t offset=0,const std::string& author="");
    Json message_location(const std::string& guild,const std::string& message);
    Json message(const std::string& message);
    Json detection_patterns();
    void set_detection_patterns(const Json& patterns);
    void review(const std::string& message,const std::string& state);
};
std::string chat_date_id(const std::string& date);
std::string chat_from_id(const ChatOptions& options,bool dms);
std::string chat_time_id(Time at);
Json default_detection_patterns();
std::string suggestion_reason(const std::string& text,const Json& patterns=default_detection_patterns());
std::string chat_message_text(const Json& message);
Json chat_message_attachments(const Json& message);
std::string chat_message_suggestion(const Json& message,const Json& patterns);
std::string attachment_type(const std::string& filename);
bool attachment_allowed(const Json& attachment,const ChatOptions& options);
Json list_chat_channels(const ChatGet& get,const std::string& guild,const std::atomic<bool>& cancel,const ChatProgress& progress={}, bool include_threads=true);
Json list_dm_channels(const ChatGet& get,const std::atomic<bool>& cancel,const ChatProgress& progress={});
Json collect_chat(ChatStore& store,const ChatGet& get,const ChatDownload& download,const Path& files,const std::string& account,const std::string& guild,const std::vector<std::string>& channels,const ChatOptions& options,const std::atomic<bool>& cancel,const ChatProgress& progress={});
}
