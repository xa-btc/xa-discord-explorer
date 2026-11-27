#pragma once
#include "core.h"
#include <set>
#include <atomic>
#include <functional>

namespace sml {
struct CollectionProgress {
    size_t members=0;
    int64_t total=-1;
    size_t chunks=0;
    int total_chunks=-1;
    std::string stage="Connecting";
};
using ProgressCallback=std::function<void(const CollectionProgress&)>;
// Pure event accumulator: no credentials, network or database writes.
class GatewayMembers {
    std::string server_, nonce_, name_;
    std::map<std::string, Json> visible_;
    std::map<int, Json> chunks_;
    int chunk_count_=-1;
    int64_t expected_=-1;
    bool changed_=false, found_=false;
    int full_permission_=-1;
    std::string channel_,list_id_;
    bool sidebar_=false,sidebar_done_=false;
    int next_range_=0,sidebar_limit_=-1;
    bool page_ready_=false;
public:
    GatewayMembers(std::string server, std::string nonce);
    void event(const std::string& type, const Json& data);
    bool found() const {return found_;}
    bool finished() const;
    CollectionProgress progress() const;
    Json request() const;
    bool full_request_allowed() const {return full_permission_!=0;}
    bool can_read_sidebar() const {return !channel_.empty();}
    bool using_sidebar() const {return sidebar_;}
    bool next_page_ready() const {return page_ready_;}
    Json sidebar_request();
    Json snapshot(const std::string& reason="") const;
};
Json collect_gateway_members(const std::string& token, const std::string& server, const std::atomic<bool>& cancel, const ProgressCallback& progress={}, bool counts_only=false, const std::vector<std::string>& count_servers={});
Json extract_server_counts(const Json& ready, const std::vector<std::string>& servers);
}
