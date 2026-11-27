#include "gateway.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <algorithm>
#include <limits>

namespace sml {
namespace {
Json member_record(const Json& member) {
    auto& user=member.at("user");auto uid=id(user.at("id"));auto name=user.at("username").get<std::string>();
    if(wide(name).size()>100)throw std::runtime_error("Invalid member username from Gateway");
    return {{"user_id",uid},{"username",name}};
}
uint64_t permissions(const Json& value) {
    if(value.is_string())return std::stoull(value.get<std::string>());
    return value.get<uint64_t>();
}
struct Handle {
    HINTERNET h{};
    explicit Handle(HINTERNET value):h(value){if(!h)throw std::runtime_error("Could not open Discord Gateway connection");}
    ~Handle(){if(h)WinHttpCloseHandle(h);}
    Handle(const Handle&)=delete;
};
class Socket {
    std::atomic<HINTERNET> handle_;
    std::mutex send_mutex_,wait_mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopped_{false},waiting_ack_{false},failed_{false};
    const std::atomic<bool>& cancel_;
    std::atomic<long long> sequence_{-1},interval_{0};
    std::thread watchdog_;
public:
    explicit Socket(HINTERNET handle,const std::atomic<bool>& cancel):handle_(handle),cancel_(cancel) {
        if(!handle)throw std::runtime_error("Discord Gateway upgrade failed");
        watchdog_=std::thread([this]{
            auto last=std::chrono::steady_clock::now();
            std::unique_lock lock(wait_mutex_);
            while(!wake_.wait_for(lock,std::chrono::milliseconds(200),[this]{return stopped_.load();})) {
                auto time=std::chrono::steady_clock::now();
                if(cancel_){close();return;}
                if(interval_>0&&time-last>=std::chrono::milliseconds(interval_.load())) {
                    if(waiting_ack_.exchange(true)){failed_=true;close();return;}
                    try{heartbeat();}catch(...){failed_=true;close();return;}
                    last=time;
                }
            }
        });
    }
    ~Socket(){stopped_=true;wake_.notify_all();if(watchdog_.joinable())watchdog_.join();close();}
    void close(){std::lock_guard lock(send_mutex_);auto h=handle_.exchange(nullptr);if(h)WinHttpCloseHandle(h);}
    bool failed() const{return failed_;}
    void sequence(long long seq){sequence_=seq;}
    void hello(long long ms){if(ms<1000||ms>120000)throw std::runtime_error("Invalid Gateway heartbeat interval");interval_=ms;}
    void ack(){waiting_ack_=false;}
    void heartbeat(){auto seq=sequence_.load();send({{"op",1},{"d",seq<0?Json(nullptr):Json(seq)}});}
    void send(const Json& data) {
        auto text=data.dump();DWORD result;
        {std::lock_guard lock(send_mutex_);auto h=handle_.load();result=h?WinHttpWebSocketSend(h,WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,text.data(),static_cast<DWORD>(text.size())):ERROR_INVALID_HANDLE;}
        SecureZeroMemory(text.data(),text.size());
        if(result!=NO_ERROR)throw std::runtime_error("Discord Gateway send failed");
    }
    Json receive() {
        std::string text;char buffer[65536];
        while(true) {
            DWORD count=0;WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};auto h=handle_.load();
            auto result=h?WinHttpWebSocketReceive(h,buffer,sizeof(buffer),&count,&type):ERROR_INVALID_HANDLE;
            if(result==ERROR_WINHTTP_TIMEOUT&&handle_.load())continue;
            if(result!=NO_ERROR)throw std::runtime_error("Discord Gateway receive interrupted");
            if(type==WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)throw std::runtime_error("Discord closed the user Gateway session; reconnect the account and try again");
            if(type!=WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE&&type!=WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                throw std::runtime_error("Discord returned an unsupported Gateway message format");
            text.append(buffer,count);if(text.size()>50*1024*1024)throw std::runtime_error("Gateway message exceeded the size limit");
            if(type==WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)break;
        }
        try{return Json::parse(text);}catch(...){throw std::runtime_error("Discord returned invalid Gateway JSON");}
    }
};
}
GatewayMembers::GatewayMembers(std::string server,std::string nonce):server_(id(server)),nonce_(std::move(nonce)){}
Json GatewayMembers::request() const {
    return {{"op",8},{"d",{{"guild_id",Json::array({server_})},{"query",""},{"limit",0},{"presences",false},{"nonce",nonce_}}}};
}
Json GatewayMembers::sidebar_request() {
    if(channel_.empty())throw std::runtime_error("No shared member sidebar is available in a channel accessible to this account");
    sidebar_=true;page_ready_=false;
    return {{"op",14},{"d",{{"guild_id",server_},{"typing",true},{"activities",false},{"threads",false},
        {"channels",{{channel_,Json::array({Json::array({next_range_,next_range_+99})})}}}}}};
}
void GatewayMembers::event(const std::string& type,const Json& data) {
    if(type=="READY") {
        size_t guild_index=0;
        for(auto guild:data.at("guilds")) {
            size_t index=guild_index++;
            if(id(guild.at("id"))!=server_)continue;
            if(data.contains("merged_members")&&index<data["merged_members"].size()) {
                if(!guild.contains("members"))guild["members"]=Json::array();
                for(auto member:data["merged_members"][index]) {
                    if(!member.contains("user")&&member.contains("user_id")) {
                        auto uid=id(member["user_id"]);
                        if(data.contains("user")&&id(data["user"]["id"])==uid)member["user"]=data["user"];
                        else for(auto& user:data.value("users",Json::array()))if(id(user.at("id"))==uid){member["user"]=user;break;}
                    }
                    if(member.contains("user")&&!member["user"].is_null())guild["members"].push_back(member);
                }
            }
            found_=true;name_=guild.contains("properties")?guild.at("properties").value("name",std::string()):guild.value("name",std::string());
            expected_=guild.value("member_count",int64_t(-1));
            if(guild.contains("members"))for(auto& member:guild["members"]){auto record=member_record(member);visible_[record["user_id"].get<std::string>()]=record;}
            // Only use a channel that this account itself can view, including role-gated channels.
            if(data.contains("user")&&guild.contains("roles")) {
                auto self=id(data.at("user").at("id"));std::set<std::string> mine;
                bool known=false;for(auto& member:guild.value("members",Json::array()))if(id(member.at("user").at("id"))==self) {
                    known=true;for(auto& role:member.value("roles",Json::array()))mine.insert(id(role));
                }
                uint64_t everyone=0,own=0;for(auto& role:guild["roles"]){auto rid=id(role.at("id"));auto p=permissions(role.at("permissions"));if(rid==server_)everyone=p;if(mine.contains(rid))own|=p;}
                own|=everyone;auto properties=guild.value("properties",Json::object());bool owner=properties.value("owner_id",guild.value("owner_id",std::string()))==self;
                bool admin=owner||(own&8)!=0;
                if(owner||known)full_permission_=(admin||(own&(2|4|(1ULL<<28))))?1:0;
                for(auto& channel:guild.value("channels",Json::array())) {
                    if(channel.value("type",-1)!=0&&channel.value("type",-1)!=5)continue;
                    uint64_t access=own,role_allow=0,role_deny=0,member_allow=0,member_deny=0;
                    auto overwrites=channel.value("permission_overwrites",Json::array());
                    for(auto& overwrite:overwrites)if(id(overwrite.at("id"))==server_){auto allow=permissions(overwrite.at("allow")),deny=permissions(overwrite.at("deny"));access=(access&~deny)|allow;}
                    for(auto& overwrite:overwrites){auto rid=id(overwrite.at("id"));auto allow=permissions(overwrite.at("allow")),deny=permissions(overwrite.at("deny"));
                        if(mine.contains(rid)){role_allow|=allow;role_deny|=deny;}if(rid==self){member_allow|=allow;member_deny|=deny;}}
                    access=(((access&~role_deny)|role_allow)&~member_deny)|member_allow;
                    if(admin||(known&&(access&1024))){channel_=id(channel.at("id"));break;}
                }
            }
        }
        return;
    }
    if(!data.contains("guild_id")||id(data["guild_id"])!=server_)return;
    if(type=="GUILD_MEMBER_LIST_UPDATE"&&sidebar_) {
        auto list=data.value("id",std::string());if(!list_id_.empty()&&list!=list_id_)return;
        bool matching=false;
        for(auto& op:data.at("ops"))if(op.value("op",std::string())=="SYNC"&&op.at("range").at(0)==next_range_)matching=true;
        if(!matching)return;
        if(list_id_.empty())list_id_=list;
        int64_t limit=0;for(auto& group:data.at("groups")){auto count=group.at("count").get<int64_t>();if(count<0)throw std::runtime_error("Invalid sidebar group size");limit+=count+1;}
        if(limit>std::numeric_limits<int>::max()-100)throw std::runtime_error("Sidebar exceeds the supported collection size");
        sidebar_limit_=static_cast<int>(limit);
        for(auto& op:data["ops"]) {
            if(op.value("op",std::string())!="SYNC"||op.at("range").at(0)!=next_range_)continue;
            auto& items=op.at("items");if(!items.is_array()||items.size()>100)throw std::runtime_error("Invalid sidebar range response");
            for(auto& item:items)if(item.contains("member")){auto record=member_record(item["member"]);visible_[record["user_id"].get<std::string>()]=record;}
            if(visible_.size()>5000000)throw std::runtime_error("Member collection exceeded the size limit");
            next_range_+=100;sidebar_done_=next_range_>=sidebar_limit_;page_ready_=!sidebar_done_;break;
        }
        return;
    }
    if(type=="GUILD_MEMBER_ADD"||type=="GUILD_MEMBER_REMOVE")changed_=true;
    if(type=="GUILD_MEMBER_REMOVE") {visible_.erase(id(data.at("user").at("id")));return;}
    if(type=="GUILD_MEMBER_ADD") {auto record=member_record(data);visible_[record["user_id"].get<std::string>()]=record;return;}
    if(type!="GUILD_MEMBERS_CHUNK"||!data.contains("nonce")||data["nonce"]!=nonce_)return;
    int count=data.at("chunk_count").get<int>(),index=data.at("chunk_index").get<int>();
    if(count<1||count>100000||index<0||index>=count||(chunk_count_!=-1&&count!=chunk_count_))throw std::runtime_error("Inconsistent Gateway member chunks; saved roster was kept");
    auto members=data.at("members");if(!members.is_array()||members.size()>1000)throw std::runtime_error("Invalid Gateway member chunk");
    Json records=Json::array();for(auto& member:members){auto record=member_record(member);records.push_back(record);visible_[record["user_id"].get<std::string>()]=record;}
    if(visible_.size()>5000000)throw std::runtime_error("Member collection exceeded the 5,000,000-member in-memory limit");
    if(chunks_.contains(index)&&chunks_.at(index)!=records)throw std::runtime_error("Conflicting Gateway member chunk");
    chunk_count_=count;chunks_[index]=std::move(records);
}
bool GatewayMembers::finished() const {return (chunk_count_>0&&chunks_.size()==static_cast<size_t>(chunk_count_))||(sidebar_&&sidebar_done_);}
CollectionProgress GatewayMembers::progress() const {
    return {visible_.size(),expected_,chunks_.size(),chunk_count_,sidebar_?sidebar_done_?"Visible sidebar collected (partial)":"Reading visible member sidebar - range "+std::to_string(next_range_):finished()?"Verifying member list":chunks_.empty()?"Initial cache received; no roster chunks yet":"Receiving members"};
}
Json GatewayMembers::snapshot(const std::string& reason) const {
    if(!found_)throw std::runtime_error("The selected account did not receive this server in its Gateway session");
    std::map<std::string,Json> members=visible_;bool complete=chunk_count_>0&&chunks_.size()==static_cast<size_t>(chunk_count_)&&!changed_;
    if(complete) {
        members.clear();for(auto& [index,chunk]:chunks_)for(auto& record:chunk) {
            if(!members.emplace(record["user_id"].get<std::string>(),record).second)throw std::runtime_error("Duplicate member across Gateway chunks; saved roster was kept");
        }
        if(expected_>=0&&members.size()!=static_cast<size_t>(expected_))complete=false;
    }
    if(members.empty()&&!complete)throw std::runtime_error("Discord returned no usable members for this account. Existing saved roster was kept.");
    Json list=Json::array();for(auto& [uid,member]:members)list.push_back(member);
    auto note=complete?"All requested member chunks received":sidebar_?"Visible member sidebar only; offline or hidden members may be omitted":changed_?"Membership changed during collection":finished()?"Returned member count differs from the server count":reason.empty()?"Discord did not return every requested member chunk":reason;
    return {{"server_id",server_},{"server_name",name_.empty()?server_:name_},{"complete",complete},{"captured_at",stamp(now())},{"members",list},{"source","user_gateway"},{"coverage_note",note},{"expected_members",expected_}};
}
Json extract_server_counts(const Json& ready,const std::vector<std::string>& servers) {
    const auto& guilds=ready.at("guilds");if(!guilds.is_array())throw std::runtime_error("Gateway returned an invalid server list");
    std::map<std::string,const Json*> index;for(const auto& guild:guilds)if(guild.is_object()&&guild.contains("id"))index[id(guild["id"])]=&guild;
    auto captured=stamp(now());Json results=Json::array();std::set<std::string> seen;
    for(const auto& requested:servers){auto sid=id(requested);if(!seen.insert(sid).second)continue;
        try{
            if(!index.contains(sid))throw std::runtime_error("Selected account is not a member of this server");
            const auto& guild=*index.at(sid);
            if(!guild.contains("member_count")||!guild["member_count"].is_number_integer()||guild["member_count"].get<int64_t>()<0)throw std::runtime_error("Discord did not report a member count for this server");
            auto name=guild.value("name",std::string());if(name.empty()&&guild.contains("properties"))name=guild["properties"].value("name",sid);
            results.push_back({{"server_id",sid},{"server_name",name.empty()?sid:name},{"server_total",guild["member_count"]},{"captured_at",captured},{"counts_only",true}});
        }catch(const std::exception& e){results.push_back({{"server_id",sid},{"error",e.what()},{"status","failed"}});}
    }
    return results;
}
Json collect_gateway_members(const std::string& token,const std::string& server,const std::atomic<bool>& cancel,const ProgressCallback& progress,bool counts_only,const std::vector<std::string>& count_servers) {
    auto check_cancel=[&]{if(cancel.load())throw std::runtime_error("Collection cancelled. Existing saved roster was kept.");};
    check_cancel();if(progress)progress(CollectionProgress{});
    unsigned char random[12];if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)!=0)throw std::runtime_error("Could not create member request ID");
    std::string nonce;for(auto b:random){nonce+="0123456789abcdef"[b>>4];nonce+="0123456789abcdef"[b&15];}
    GatewayMembers members(server,nonce);
    Handle session(WinHttpOpen(L"XADiscordExplorer/0.5",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    WinHttpSetTimeouts(session.h,10000,10000,10000,15000);
    Handle connection(WinHttpConnect(session.h,L"gateway.discord.gg",INTERNET_DEFAULT_HTTPS_PORT,0));
    check_cancel();
    Handle request(WinHttpOpenRequest(connection.h,L"GET",L"/?v=9&encoding=json",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(request.h,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects))||!WinHttpSetOption(request.h,WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,nullptr,0))throw std::runtime_error("Could not configure Gateway WebSocket");
    if(!WinHttpSendRequest(request.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,nullptr,0,0,0)||!WinHttpReceiveResponse(request.h,nullptr))throw std::runtime_error("Could not reach Discord Gateway");
    check_cancel();
    DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(request.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr)||status!=101)throw std::runtime_error("Discord Gateway did not accept the WebSocket connection");
    Socket socket(WinHttpWebSocketCompleteUpgrade(request.h,0),cancel);bool identified=false,requested=false;
    auto next_sidebar_send=std::chrono::steady_clock::time_point::min();
    auto send_sidebar=[&] {
        // One outstanding page; response time counts toward the 750 ms send interval.
        while(std::chrono::steady_clock::now()<next_sidebar_send){
            check_cancel();auto remaining=next_sidebar_send-std::chrono::steady_clock::now();
            if(remaining>std::chrono::steady_clock::duration::zero())std::this_thread::sleep_for(std::min(remaining,std::chrono::steady_clock::duration(std::chrono::milliseconds(50))));
        }
        check_cancel();socket.send(members.sidebar_request());
        next_sidebar_send=std::chrono::steady_clock::now()+std::chrono::milliseconds(750);
    };
    auto start_request=[&] {
        if(members.full_request_allowed())socket.send(members.request());
        else send_sidebar();
        requested=true;if(progress)progress(members.progress());
    };
    if(progress){CollectionProgress state;state.stage="Authenticating user session";progress(state);}
    try {
        while(true) {
            check_cancel();
            auto packet=socket.receive();if(packet.contains("s")&&!packet["s"].is_null())socket.sequence(packet["s"].get<long long>());
            int op=packet.at("op").get<int>();
            if(op==10) {
                if(identified)throw std::runtime_error("Unexpected repeated Gateway greeting");
                socket.hello(packet.at("d").at("heartbeat_interval").get<long long>());
                socket.send({{"op",2},{"d",{{"token",token},{"capabilities",0},{"properties",{{"os","Windows"},{"browser","XA Discord Explorer"},{"device","XA Discord Explorer"}}},{"compress",false},{"client_state",{{"guild_versions",Json::object()}}}}}});identified=true;
            } else if(op==11){socket.ack();
                if(!requested&&members.found()) {
                    if(!members.full_request_allowed()&&!members.can_read_sidebar())return members.snapshot("Full roster unavailable to this account; no accessible member sidebar");
                    start_request();
                }
                // An acknowledged heartbeat proves the connection is alive. If the
                // full request yielded no chunks, use the visible user member list.
                else if(requested&&!members.using_sidebar()&&members.progress().chunks==0&&members.can_read_sidebar()){send_sidebar();if(progress)progress(members.progress());}
            }else if(op==1)socket.heartbeat();
            else if(op==7||op==9)throw std::runtime_error("Discord invalidated or requested a restart of this Gateway session; try again manually");
            else if(op==0) {
                auto type=packet.at("t").get<std::string>();
                if(counts_only&&type=="READY") {
                    check_cancel();
                    auto results=extract_server_counts(packet.at("d"),count_servers.empty()?std::vector<std::string>{server}:count_servers);
                    if(!count_servers.empty())return results;
                    if(results[0].contains("error"))throw std::runtime_error(results[0]["error"].get<std::string>());
                    return results[0];
                }
                members.event(type,packet.at("d"));
                if(type=="READY"&&!members.found())throw std::runtime_error("Selected account is not a member of this server");
                // READY completes identification; do not wait for an optional supplemental event or heartbeat.
                if((type=="READY"||type=="READY_SUPPLEMENTAL")&&!requested&&members.found()) {
                    if(!members.full_request_allowed()&&!members.can_read_sidebar())return members.snapshot("Full roster unavailable to this account; no accessible member sidebar");
                    start_request();
                }
                if(members.using_sidebar()&&members.next_page_ready()) {
                    if(progress)progress(members.progress());
                    send_sidebar();
                }
                if(requested&&progress)progress(members.progress());
                if(members.finished())return members.snapshot();
            }
        }
    }catch(...) {
        if(cancel.load()) {
            if(members.found()&&members.progress().members>0){auto result=members.snapshot();result["complete"]=false;result["cancelled"]=true;result["coverage_note"]="Collection cancelled; only members received before cancellation are included";return result;}
            throw std::runtime_error("Collection cancelled. Existing saved roster was kept.");
        }
        if(socket.failed())throw std::runtime_error("Discord Gateway connection stopped responding to heartbeats. Existing saved roster was kept.");
        throw;
    }
}
}
