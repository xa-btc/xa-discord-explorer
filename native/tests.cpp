#include "core.h"
#include "session.h"
#include "gateway.h"
#include "audit.h"
#include <iostream>
#include <functional>
#include <stdexcept>
#include <fstream>

using namespace sml;
static Json example(std::string sid,std::string name,std::vector<std::string> users,Time at) {
    Json members=Json::array();for(auto& user:users)members.push_back({{"user_id",user},{"username","User "+user.substr(user.size()-1)}});
    return {{"server_id",sid},{"server_name",name},{"complete",true},{"captured_at",stamp(at)},{"members",members}};
}
Json demo_initial(Time at) { return Json::array({example("100","Server A",{"1001","1002"},at),example("200","Server B",{"1003"},at),example("300","Server C",{},at)}); }
Json demo_leave(Time at) { return example("100","Server A",{"1002"},at); }

static void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void rejects(const std::function<void()>& action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}require(failed,"Expected rejection");}
static void test_audit_log() {
    std::atomic<bool> cancel=false;
    int pages=0;
    auto servers=audit_servers([&](const std::string& path)->Json {
        if(path=="users/@me")return {{"id","7"},{"username","Audit test user"}};
        if(++pages==1) {
            require(path=="users/@me/guilds?limit=200&after=0","Wrong initial guild route");
            Json batch=Json::array();
            for(int i=1;i<=200;++i)batch.push_back({{"id",std::to_string(i)},{"name","Guild"},
                {"permissions",i==8?"8":i==128?"128":"1"}});
            return batch;
        }
        require(path=="users/@me/guilds?limit=200&after=200","Wrong guild cursor");
        return Json::array({{{"id","201"},{"name","Owner"},{"owner",true}},{{"id","202"},{"name","No permission"}}});
    },"7",cancel);
    require(pages==2&&servers.size()==3&&servers[0]["id"]=="128"&&servers[1]["id"]=="8"&&servers[2]["id"]=="201",
        "Audit guild permission filtering or pagination failed");
    rejects([&]{audit_servers([](const std::string&)->Json{return {{"id","7"},{"bot",true}};},"7",cancel);});
    rejects([&]{audit_servers([](const std::string&)->Json{return {{"id","8"}};},"7",cancel);});

    const uint64_t first=1227398400000000000ULL;
    auto entry=[](uint64_t entry_id,int type)->Json {
        return {{"id",std::to_string(entry_id)},{"action_type",type},{"user_id","7"},{"target_id","8"},
            {"options",{{"channel_id","42"},{"message_id","99"}}}};
    };
    const std::string route="guilds/128/audit-logs?limit=100";
    std::vector<Json> published;
    const AuditPage save_page=[&](Json page){published.push_back(std::move(page));};
    pages=0;
    scan_audit_log([&](const std::string& path)->Json {
        if(++pages==1) {
            require(path==route,"Audit request must fetch all activity without an action filter");
            Json batch=Json::array();
            for(uint64_t i=0;i<100;++i)batch.push_back(entry(first-i,75));
            batch[1]["user_id"]="6";
            batch[1]["target_id"]="9";
            batch[2].erase("target_id");
            return {{"audit_log_entries",batch},{"users",Json::array({{{"id","7"},{"username","moderator"}},
                {{"id","6"},{"username",nullptr}},{{"id","8"},{"username","original-author"}}})}};
        }
        require(published.size()==static_cast<size_t>(pages-1),"Fetched the next audit page before publishing the previous one");
        if(pages==2) {
            require(path==route+"&before="+std::to_string(first-99),"Wrong audit cursor");
            auto unknown=entry(first-100,75);unknown["action_type"]=nullptr;
            auto missing=entry(first-101,75);missing.erase("action_type");
            return {{"audit_log_entries",Json::array({entry(first-99,75),unknown,missing,entry(first-102,74)})}};
        }
        if(pages==3) {
            require(path==route+"&before="+std::to_string(first-102),"Short audit page prematurely ended pagination");
            auto pin=entry(first-104,74);pin["options"]["message_id"]="invalid";
            return {{"audit_log_entries",Json::array({entry(first-103,72),pin})}};
        }
        require(pages==4&&path==route+"&before="+std::to_string(first-104),"Wrong final audit cursor");
        return {{"audit_log_entries",Json::array()}};
    },"128",cancel,save_page);
    require(pages==4&&published.size()==3&&published[0].size()==100&&published[1].size()==3&&published[2].size()==2,
        "Audit streaming lost entries or published duplicates or an empty page");
    const auto& unpin=published[0][0];
    require(unpin["actor"]=="moderator"&&unpin["actor_id"]=="7"&&unpin["entry"]["target_id"]=="8"&&
        unpin["link"]=="https://discord.com/channels/128/42/99"&&published[0][1]["actor"]=="6",
        "Unpin details, actor resolution or direct message link failed");
    require(unpin["at"].get<std::string>().starts_with("2024-"),"Audit time is not derived from the entry snowflake");
    require(unpin["author_id"]=="8"&&unpin["author"]=="original-author"&&published[0][1]["author"]=="9"&&
        published[0][2]["author"]==""&&published[1][2]["author"]=="original-author"&&published[2][0]["author_id"]=="",
        "Audit pin author did not resolve separately from the moderator or a deleted-message entry acquired an author");
    require(published[1][0]["action_type"]==-1&&published[1][0]["entry"]["action_type"].is_null()&&
        published[1][1]["action_type"]==-1&&!published[1][1]["entry"].contains("action_type")&&
        published[1][0]["link"]==""&&published[1][2]["link"]==unpin["link"]&&published[1][2]["actor"]=="moderator"&&
        published[2][0]["link"]==""&&published[2][1]["link"]==""&&audit_action_name(-1)=="Unknown action"&&
        audit_action_name(75)=="Message unpinned"&&audit_action_name(999)=="Action 999","Unexpected audit actions or links");

    published.clear();
    scan_audit_log([&](const std::string& path)->Json {
        require(path==route,"Audit request used the wrong server or an action filter");
        return {{"audit_log_entries",Json::array()}};
    },"128",cancel,save_page);
    require(published.empty(),"Empty audit response produced entries");
    try {
        scan_audit_log([&](const std::string& path)->Json {
            if(path==route)return {{"audit_log_entries",Json::array({entry(first,75)})}};
            throw ChatError(403,50013,"denied");
        },"128",cancel,save_page);
        throw std::logic_error("Expected permission denial");
    } catch(const std::runtime_error& e) {
        require(std::string(e.what()).find("View Audit Log")!=std::string::npos,"Missing server audit access error");
    }
    require(published.size()==1&&published[0].size()==1,"A later audit failure discarded the published page");

    published.clear();
    rejects([&]{scan_audit_log([&](const std::string&)->Json {
        return {{"audit_log_entries",Json::array({entry(first,75),entry(first,75)})}};
    },"128",cancel,save_page);});
    require(published.size()==1&&published[0].size()==1,"Repeated cursor produced duplicate audit rows");

    pages=0;
    published.clear();
    rejects([&]{scan_audit_log([&](const std::string&)->Json {
        ++pages;return {{"audit_log_entries",Json::array({entry(first,75)})}};
    },"128",cancel,[&](Json page){save_page(std::move(page));cancel=true;});});
    require(pages==1&&published.size()==1,"Audit cancellation fetched or discarded another page");
}

static void test_audit_change_rows() {
    const Json entries=Json::parse(R"([
        {"id":"804","action_type":25,"user_id":"7","target_id":"8","changes":[
            {"key":"$add","new_value":[{"id":"41","name":"Archivist"},{"id":"42","name":"Editor"}]},
            {"key":"$remove","new_value":[{"id":"43","name":"Visitor"}]}]},
        {"id":"803","action_type":14,"user_id":"7","target_id":"52",
         "options":{"id":"128","role_name":"@everyone","type":"0"},
         "changes":[{"key":"deny","new_value":"1024","old_value":"2048"}]},
        {"id":"802","action_type":14,"user_id":"7","target_id":"52",
         "options":{"id":"8","type":1},
         "changes":[{"key":"allow","old_value":"1024","new_value":"9223372036854776832"}]},
        {"id":"801","action_type":24,"user_id":"7","target_id":"9",
         "changes":[{"key":"nick","old_value":"Old nickname","new_value":"New nickname"}]}
    ])");
    std::atomic<bool> cancel=false;
    Json rows;
    const std::string route="guilds/128/audit-logs?limit=100";
    scan_audit_log([&](const std::string& path)->Json {
        if(path==route)return {{"audit_log_entries",entries},{"users",Json::array({
            {{"id","7"},{"username","moderator"}},{{"id","8"},{"username","affected-member"}}})}};
        require(path==route+"&before=801","Resolving audit changes made an unexpected request");
        return {{"audit_log_entries",Json::array()}};
    },"128",cancel,[&](Json page){rows=std::move(page);});
    require(rows.size()==entries.size(),"Audit changes dropped entries");
    const auto& roles=rows.at(0);
    require(roles.at("actor")=="moderator"&&roles.at("author")=="affected-member"&&roles.at("author_id")=="8",
        "Role update conflated the moderator and affected member");
    require(roles.at("change_summary")=="Added roles: Archivist, Editor. Removed roles: Visitor.",
        "Role update lost a role or reversed the added and removed lists");
    const auto& everyone=rows.at(1);
    require(everyone.at("author")=="@everyone"&&everyone.at("channel")=="52"&&everyone.at("entry")==entries[1],
        "Role overwrite lost the affected role, channel or original entry");
    require(everyone.at("change_summary")=="@everyone: Denied View Channel. No longer denied Send Messages.",
        "Role overwrite did not distinguish a new denial from the removal of a previous denial");
    const auto& member=rows.at(2);
    const auto& permissions=member.at("change_summary").get_ref<const std::string&>();
    require(member.at("author")=="affected-member"&&member.at("author_id")=="8"&&
        permissions.find("Permission bit 63")!=std::string::npos&&permissions.find("View Channel")==std::string::npos,
        "Member overwrite used the channel as a user, lost a high permission bit or reported an unchanged permission");
    const auto& nick=rows.at(3);
    require(nick.at("author")=="9"&&nick.at("change_summary").get_ref<const std::string&>().find("Nickname: Old nickname → New nickname")!=std::string::npos,
        "Member change lost the target ID fallback or the direction of the nickname update");
}

static void test_audit_message_preview() {
    Json message={{"id","99"},{"channel_id","42"},{"guild_id","128"},{"author",{{"id","8"},{"username","writer"}}},
        {"content","Line one\nLine two"}};
    int reads=0;
    const ChatGet get=[&](const std::string& route){
        ++reads;
        require(route=="channels/42/messages?around=99&limit=1","Preview must use the user-session history route with the exact message ID");
        return Json::array({message});
    };
    auto preview=audit_message_preview(get,"128","42","99");
    require(reads==1&&preview["status"]=="available"&&preview["author"]=="writer"&&preview["author_id"]=="8"&&
        preview["text"]=="Line one\nLine two","Audit preview lost a message's text or original author");

    message.erase("guild_id");
    message["content"]="";
    message["embeds"]=Json::array({{{"title","Release notes"},{"description","New build"},
        {"fields",Json::array({{{"name","Version"},{"value","1.2"}}})}}});
    message["attachments"]=Json::array({{{"id","10"},{"filename","notes.txt"}}});
    message["message_snapshots"]=Json::array({{{"message",{{"content","Forwarded detail"}}}}});
    auto text=audit_message_preview(get,"128","42","99").at("text").get<std::string>();
    require(text=="[Forwarded message]\nForwarded detail\nRelease notes\nNew build\nVersion\n1.2\nAttachment: notes.txt",
        "Audit preview lost forwarded content, embed fields or attachment names");
    message.erase("embeds");
    message.erase("message_snapshots");
    require(audit_message_preview(get,"128","42","99")["text"]=="Attachment: notes.txt","Attachment-only preview missing");
    message.erase("attachments");
    message["author"].erase("username");
    preview=audit_message_preview(get,"128","42","99");
    require(preview["author"]=="8"&&preview["status"]=="available"&&!preview["text"].get<std::string>().empty(),
        "Empty messages and unknown author names need a readable preview");

    message["guild_id"]="129";
    rejects([&]{audit_message_preview(get,"128","42","99");});
    message.erase("guild_id");
    message["channel_id"]="43";
    rejects([&]{audit_message_preview(get,"128","42","99");});
    message["channel_id"]="42";
    message["id"]="100";
    message["content"]="neighbor's private text";
    preview=audit_message_preview(get,"128","42","99");
    require(preview["status"]=="unavailable"&&preview["author_id"]==""&&
        preview["text"].get<std::string>().find("neighbor")==std::string::npos,"Preview substituted a nearby message");
    const auto neighbor=message;
    message["id"]="99";
    message["content"]="exact match";
    preview=audit_message_preview([&](const std::string&)->Json{return Json::array({neighbor,message});},"128","42","99");
    require(preview["status"]=="available"&&preview["text"]=="exact match","Preview picked the first nearby item over the exact message");
    preview=audit_message_preview([](const std::string&){return Json::array();},"128","42","99");
    require(preview["status"]=="unavailable"&&preview["author"]=="","Empty history was misclassified or replaced with stale data");
    rejects([&]{audit_message_preview([](const std::string&){return Json::object();},"128","42","99");});
    const auto before=reads;
    rejects([&]{audit_message_preview(get,"128","42","../99");});
    require(before==reads,"Malformed audit message link triggered a lookup");
    for(int status:{403,404}) {
        int attempts=0;
        preview=audit_message_preview([&](const std::string& route)->Json{
            ++attempts;
            require(route=="channels/42/messages?around=99&limit=1","Unexpected route after channel history denial");
            throw ChatError(status,0,"unavailable");
        },"128","42","99");
        require(preview["status"]==(status==403?"forbidden":"not_found")&&preview["author_id"]==""&&
            !preview["text"].get<std::string>().empty()&&attempts==1,"Missing or unreadable channel history needs a single scoped lookup");
    }
    rejects([&]{audit_message_preview([](const std::string&)->Json{throw ChatError(429,0,"rate limited");},"128","42","99");});
}

static void test_filtered_audit_log() {
    std::atomic<bool> cancel=false;
    const std::string base="guilds/128/audit-logs?limit=100&action_type=";
    const auto pins=base+"74",unpins=base+"75";
    auto entry=[](int id,const Json& action) {
        return Json{{"id",std::to_string(id)},{"action_type",action},{"options",{{"channel_id","42"},{"message_id","99"}}}};
    };
    auto response=[](std::initializer_list<Json> rows) {
        return Json{{"audit_log_entries",std::vector<Json>(rows)}};
    };
    std::vector<int> ids;
    const AuditPage collect=[&](Json rows) {
        require(!rows.empty()&&rows.size()<=100,"Filtered audit callback received an invalid page size");
        for(const auto& row:rows)ids.push_back(std::stoi(row.at("entry").at("id").get<std::string>()));
    };
    std::vector<std::string> requests;
    scan_audit_log([&](const std::string& path)->Json {
        requests.push_back(path);
        if(path==pins)return response({entry(900,74),entry(500,74)});
        if(path==unpins)return response({entry(850,75),entry(700,75)});
        if(path==unpins+"&before=700") {
            require(ids==std::vector<int>{900,850,700},"Filtered scan waited for older pages before publishing the newest results");
            return response({entry(700,75),entry(600,75),entry(300,75)});
        }
        if(path==pins+"&before=500")return response({entry(450,nullptr),entry(350,74)});
        if(path==pins+"&before=350"||path==unpins+"&before=300")return response({});
        throw std::logic_error("Filtered scan requested unrelated history or used the wrong action cursor");
    },"128",cancel,collect,{75,74,74});
    require(requests.size()==6&&ids==std::vector<int>{900,850,700,600,500,350,300},
        "Selected pin and unpin streams were not deduplicated and merged newest first");

    ids.clear();
    scan_audit_log([&](const std::string& path)->Json {
        if(path==base+"999")return response({entry(800,nullptr)});
        if(path==base+"999&before=800")return response({entry(700,999)});
        require(path==base+"999&before=700","Missing action type stalled a filtered cursor");
        return response({});
    },"128",cancel,collect,{999});
    require(ids==std::vector<int>{700},"Unknown numeric action was omitted or a null action was misclassified");
    rejects([&]{scan_audit_log([&](const std::string&)->Json {return response({entry(300,72)});},"128",cancel,collect,{74});});
    require(ids==std::vector<int>{700},"Unselected action leaked into a targeted audit request");
    int calls=0;
    rejects([&]{scan_audit_log([&](const std::string&)->Json {++calls;return response({});},"128",cancel,collect,{-1});});
    require(calls==0,"Invalid action initiated a network request");

    ids.clear();
    try {
        scan_audit_log([&](const std::string& path)->Json {
            if(path==pins)return response({entry(800,74),entry(20,74)});
            require(path==unpins,"Unexpected request after a filtered stream failed");
            throw ChatError(403,50013,"denied");
        },"128",cancel,collect,{74,75});
        throw std::logic_error("Filtered audit scan completed after permission denial");
    } catch(const std::runtime_error& e) {
        require(std::string(e.what()).find("View Audit Log")!=std::string::npos,"Filtered scan lost the permission error");
    }
    require(ids==std::vector<int>{800,20},"Permission failure on another action discarded the available matches");

    ids.clear();
    requests.clear();
    rejects([&]{scan_audit_log([&](const std::string& path)->Json {
        requests.push_back(path);
        if(path==pins)return response({entry(800,74),entry(20,74)});
        if(path==unpins)return response({entry(600,75)});
        throw std::logic_error("Cancellation requested an older filtered page");
    },"128",cancel,[&](Json rows){collect(std::move(rows));cancel=true;},{74,75});});
    require(ids==std::vector<int>{800,600,20}&&requests.size()==2,"Cancellation lost downloaded matches or continued scanning");
}

int main() {
    Path folder=std::filesystem::temp_directory_path()/(L"sml-native-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(now()));
    int passed=0;
    try {
        test_audit_log();++passed;
        test_audit_change_rows();++passed;
        test_audit_message_preview();++passed;
        test_filtered_audit_log();++passed;
        auto at=now();auto initial=demo_initial(at-60000000);
        require(digest("Server A",{{"1001","User 1"},{"1002","User 2"}})=="2a3cfc59a2cebef5759a32dc899314dff9784a747c9b2d1c27e8b02ed4778021","Python database digest compatibility");++passed;
        {
            Database db(folder/L"members.sqlite3");
            auto imported=db.apply(initial,at);require(imported.size()==3,"Initial import");
            require(db.preview("1001","300",at)["common_servers"][0]["server_name"]=="Server A","Initial A match");
            require(db.preview("1003","300",at)["common_servers"][0]["server_name"]=="Server B","B match");++passed;
            require(db.preview("1001","100",at)["common_servers"].empty(),"Target must be excluded");++passed;
            auto repeated=db.apply(initial,at);require(repeated[0]["status"]=="unchanged","Replay should be unchanged");
            std::reverse(initial[0]["members"].begin(),initial[0]["members"].end());require(db.apply(initial,at)[0]["status"]=="unchanged","Ordering must not matter");++passed;
            require(db.apply(demo_leave(at),at)[0]["removed"]==1,"One removal expected");
            auto removed=db.preview("<@1001>","300",at);require(removed["common_servers"].empty(),"Removed member still matches");
            require(!removed["discord_message"]["embeds"][0].contains("fields"),"Empty match should omit field");
            require(db.preview("1002","300",at)["common_servers"].size()==1,"Other member must remain");++passed;
            rejects([&]{db.apply(initial,at);});require(db.preview("1001","300",at)["common_servers"].empty(),"Older import resurrected member");++passed;
            rejects([&]{db.apply(demo_initial(at),at);});++passed;
            auto partial=demo_leave(at+1000000);partial["complete"]="false";rejects([&]{db.apply(partial,at);});++passed;
            auto bad=demo_leave(at+1000000);bad["members"].push_back(bad["members"][0]);rejects([&]{db.apply(bad,at);});++passed;
            auto multi=demo_leave(at);multi["server_id"]="999";
            rejects([&]{db.apply(Json::array({multi,initial[0]}),at);});require(db.servers(at).size()==3,"Transaction did not roll back");++passed;
            auto empty=demo_leave(at+1000000);empty["members"]=Json::array();db.apply(empty,at+1000000);
            require(db.preview("1002","300",at)["common_servers"].empty(),"Empty snapshot failed to clear A");
            require(db.preview("1003","300",at)["common_servers"].size()==1,"Empty A changed B");++passed;
            require(db.preview("1003","300",at+25LL*3600000000)["common_servers"].empty(),"Stale match exposed");++passed;
            rejects([&]{parse_time("2026-02-30T00:00:00Z");});rejects([&]{parse_time("2026-10-07T00:00:00");});
            require(parse_time("2026-10-07T03:00:00Z")==parse_time("2026-10-06T22:00:00-05:00"),"Timezone mismatch");
            require(parse_time(stamp(at))==at,"Timestamp round trip");++passed;
            rejects([&]{id(true);});rejects([&]{id("18446744073709551616");});require(id("18446744073709551615")=="18446744073709551615","Large ID changed");++passed;
        }
        {
            Database db(folder/L"members.sqlite3");
            require(db.preview("1001","300",at)["common_servers"].empty(),"Removal lost on restart");
            require(db.preview("1003","300",at)["common_servers"].size()==1,"Membership lost on restart");++passed;
            auto large=demo_leave(at+2000000);large["members"]=Json::array();
            for(uint64_t i=0;i<3001;++i)large["members"].push_back({{"user_id",std::to_string(1000000000000000000ULL+i)}});
            db.apply(large,at+2000000);require(db.preview("1000000000000003000","300",at)["common_servers"].size()==1,"3001-member import incomplete");++passed;
            write_json(folder/L"inbox"/L"a.json",large);write_json(folder/L"inbox"/L"bad.json",Json::object());
            auto report=scan(db,folder/L"inbox");require(report.size()==2&&report[1].contains("error"),"Inbox error isolation failed");++passed;
        }
        {
            Database db(folder/L"sessions.sqlite3");db.apply(demo_initial(at),at);
            Json account={{"user_id","1001"},{"username","Owned test account"},{"captured_at",stamp(at)},
                {"guilds",Json::array({{{"server_id","100"},{"server_name","Server A"}}})}};
            db.apply_account(account);require(db.preview("1001","300",at)["common_servers"].size()==1,"Connected account match missing");++passed;
            auto left=account;left["guilds"]=Json::array();left["captured_at"]=stamp(at+1);db.apply_account(left);
            require(db.preview("1001","300",at)["common_servers"].empty(),"Old manual roster resurrected account after leave");
            require(db.preview("1002","300",at)["common_servers"].size()==1,"Account update changed another member");++passed;
            rejects([&]{db.apply_account(account);});require(db.preview("1001","300",at)["common_servers"].empty(),"Old account snapshot restored membership");++passed;
            account["captured_at"]=stamp(at+2);db.apply_account(account);db.forget_account("1001");
            require(db.preview("1001","300",at)["common_servers"].empty(),"Forgotten account fell back to old imported membership");++passed;
            Database reopened(folder/L"sessions.sqlite3");require(reopened.preview("1001","300",at)["common_servers"].empty(),"Account removal lost on restart");++passed;
        }
        {
            Json secret={{"1001",{{"username","Test user"},{"token","fake-unit-test-token"}}}};
            auto file=folder/L".auth"/L"sessions.bin";save_sessions(file,secret);require(load_sessions(file)==secret,"Session cache round trip");
            std::ifstream stream(file,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(stream)),{});stream.close();
            require(bytes.find("fake-unit-test-token")==std::string::npos,"Session was stored as plaintext");++passed;
            bytes[bytes.size()/2]^=0x40;{std::ofstream corrupt(file,std::ios::binary);corrupt.write(bytes.data(),bytes.size());}
            rejects([&]{load_sessions(file);});++passed;
            save_sessions(file,Json::object());require(load_sessions(file).empty(),"Forgotten session not cleared");++passed;
        }
        {
            int calls=0;
            auto account=fetch_account([&](const std::string& path)->Json {
                ++calls;
                if(path=="users/@me")return {{"id","1001"},{"username","Test user"}};
                if(path=="users/@me/guilds?limit=200&after=0") {
                    Json batch=Json::array();for(int i=1;i<=200;++i)batch.push_back({{"id",std::to_string(i)},{"name","Server "+std::to_string(i)}});return batch;
                }
                require(path=="users/@me/guilds?limit=200&after=200","Wrong pagination cursor");
                return Json::array({{{"id","201"},{"name","Last server"}}});
            });
            require(account["guilds"].size()==201&&calls==3,"Did not collect complete account server list");++passed;
            rejects([&]{fetch_account([](const std::string& path)->Json {
                if(path=="users/@me")return {{"id","1001"},{"username","Test user"}};
                throw std::runtime_error("Network interrupted");
            });});++passed;
            rejects([&]{fetch_account([](const std::string&)->Json {return {{"id","1001"},{"username","Bot"},{"bot",true}};});});++passed;
        }


        {
            Json guild={{"id","900"},{"properties",{{"name","Owned server"}}},{"member_count",2}};
            guild["members"]=Json::array({Json{{"user",{{"id","1"},{"username","First"}}}}});
            auto ready=Json{{"guilds",Json::array({guild})}};
            auto chunk=[](int index,const std::string& uid){return Json{{"guild_id","900"},{"nonce","request"},{"chunk_index",index},{"chunk_count",2},
                {"members",Json::array({{{"user",{{"id",uid},{"username","Account "+uid}}}}})}};};
            GatewayMembers state("900","request");state.event("READY",ready);
            require(state.progress().members==1&&state.progress().total==2&&state.progress().chunks==0,"Initial progress from READY");++passed;
            require(state.found()&&state.request()["op"]==8&&state.request()["d"]["guild_id"][0]=="900"&&state.request()["d"]["limit"]==0,"User Gateway member request");++passed;
            auto partial=state.snapshot("timeout");require(!partial["complete"].get<bool>()&&partial["members"].size()==1,"READY subset must remain partial");++passed;
            auto wrong=chunk(0,"1");wrong["nonce"]="unrelated";state.event("GUILD_MEMBERS_CHUNK",wrong);
            require(!state.finished(),"Unrelated nonce accepted");wrong=chunk(0,"1");wrong["guild_id"]="901";state.event("GUILD_MEMBERS_CHUNK",wrong);require(!state.finished(),"Unrelated guild accepted");++passed;
            state.event("GUILD_MEMBERS_CHUNK",chunk(1,"2"));require(!state.finished(),"Premature completion");
            state.event("GUILD_MEMBERS_CHUNK",chunk(1,"2"));require(!state.finished(),"Repeated chunk counted twice");
            state.event("GUILD_MEMBERS_CHUNK",chunk(0,"1"));auto complete=state.snapshot();
            require(state.progress().members==2&&state.progress().chunks==2&&state.progress().total_chunks==2,"Unique member and chunk progress");++passed;
            require(state.finished()&&complete["complete"]==true&&complete["members"].size()==2,"Out-of-order chunks did not assemble");++passed;
            Database db(folder/L"gateway.sqlite3");db.apply(complete);require(db.preview("2","901")["common_servers"].size()==1,"Gateway roster not saved");
            rejects([&]{db.apply(partial);});require(db.preview("2","901")["common_servers"].size()==1,"Partial Gateway result erased saved member");++passed;
            state.event("GUILD_MEMBER_REMOVE",{{"guild_id","900"},{"user",{{"id","2"}}}});require(state.snapshot()["complete"]==false,"Membership change must invalidate completeness");++passed;
            GatewayMembers conflict("900","request");conflict.event("READY",ready);conflict.event("GUILD_MEMBERS_CHUNK",chunk(0,"1"));
            rejects([&]{conflict.event("GUILD_MEMBERS_CHUNK",chunk(0,"3"));});++passed;
            auto inconsistent=chunk(1,"2");inconsistent["chunk_count"]=3;rejects([&]{conflict.event("GUILD_MEMBERS_CHUNK",inconsistent);});++passed;
            GatewayMembers duplicate("900","request");duplicate.event("READY",ready);duplicate.event("GUILD_MEMBERS_CHUNK",chunk(0,"1"));duplicate.event("GUILD_MEMBERS_CHUNK",chunk(1,"1"));rejects([&]{duplicate.snapshot();});++passed;
            GatewayMembers mismatch("900","request");auto larger=ready;larger["guilds"][0]["member_count"]=3;mismatch.event("READY",larger);mismatch.event("GUILD_MEMBERS_CHUNK",chunk(0,"1"));mismatch.event("GUILD_MEMBERS_CHUNK",chunk(1,"2"));require(mismatch.snapshot()["complete"]==false,"Count mismatch marked complete");++passed;
            GatewayMembers absent("901","request");absent.event("READY",ready);rejects([&]{absent.snapshot();});++passed;
        }
        {
            std::atomic<bool> cancelled{true};bool reported=false;
            rejects([&]{collect_gateway_members("fake-token","900",cancelled,[&](const CollectionProgress&){reported=true;});});
            require(!reported,"Pre-cancelled collection opened transport or reported connection progress");++passed;
        }

        {
            Json user={{"id","1"},{"username","Owned account"}};
            Json guild={{"id","900"},{"properties",{{"name","Large server"}}},{"member_count",51839},
                {"roles",Json::array({{{"id","900"},{"permissions","1024"}}})},
                {"channels",Json::array({{{"id","901"},{"type",0},{"permission_overwrites",Json::array()}}})}};
            Json ready={{"user",user},{"guilds",Json::array({guild})},
                {"merged_members",Json::array({Json::array({{{"user_id","1"},{"roles",Json::array()}}})})}};
            GatewayMembers state("900","sidebar");state.event("READY",ready);
            require(!state.full_request_allowed()&&state.can_read_sidebar(),"Regular account should use public sidebar");++passed;
            auto request=state.sidebar_request();require(request["op"]==14&&request["d"]["channels"]["901"][0][0]==0,"Initial sidebar range");++passed;
            auto page=[](int start,int count){Json items=Json::array();for(int n=0;n<count;++n)items.push_back({{"member",{{"user",{{"id",std::to_string(start+n+2)},{"username","Member"}}}}}});
                return Json{{"guild_id","900"},{"id","everyone"},{"groups",Json::array({{{"id","online"},{"count",149}}})},
                    {"ops",Json::array({{{"op","SYNC"},{"range",Json::array({start,start+99})},{"items",items}}})}};};
            state.event("GUILD_MEMBER_LIST_UPDATE",page(0,100));require(state.next_page_ready()&&!state.finished()&&state.progress().members==101,"First sidebar page did not advance");++passed;
            require(state.sidebar_request()["d"]["channels"]["901"][0][0]==100,"Second sidebar request not paginated");
            state.event("GUILD_MEMBER_LIST_UPDATE",page(0,100));require(!state.next_page_ready(),"Old sidebar response advanced current range");++passed;
            state.event("GUILD_MEMBER_LIST_UPDATE",page(100,49));require(state.finished()&&state.progress().members==150&&state.snapshot()["complete"]==false,"Sidebar must end at visible groups and remain partial");++passed;
            auto denied=ready;denied["guilds"][0]["channels"][0]["permission_overwrites"]=Json::array({{{"id","1"},{"type",1},{"deny","1024"},{"allow","0"}}});
            auto gated=ready;gated["guilds"][0]["roles"][0]["permissions"]="0";
            gated["guilds"][0]["roles"].push_back({{"id","902"},{"permissions","1024"}});
            gated["merged_members"][0][0]["roles"]=Json::array({"902"});
            GatewayMembers allowed_role("900","n");allowed_role.event("READY",gated);
            require(allowed_role.can_read_sidebar(),"Role-visible channel excluded by everyone permission");++passed;
            gated["guilds"][0]["channels"][0]["permission_overwrites"]=Json::array({{{"id","1"},{"type",1},{"deny","1024"},{"allow","0"}}});
            GatewayMembers denied_role("900","n");denied_role.event("READY",gated);
            require(!denied_role.can_read_sidebar(),"Member deny bypassed for role-visible channel");++passed;
            GatewayMembers blocked("900","n");blocked.event("READY",denied);require(!blocked.can_read_sidebar(),"Denied channel selected");++passed;
            auto owner=ready;owner["guilds"][0]["properties"]["owner_id"]="1";GatewayMembers admin("900","n");admin.event("READY",owner);require(admin.full_request_allowed(),"Nested server owner lost full-list eligibility");++passed;
        }

        {
            auto t=now();Database db(folder/L"partial.sqlite3");
            auto full=demo_initial(t-25LL*3600000000)[0];db.apply(full,t);
            auto partial=demo_leave(t);partial["complete"]=false;
            partial["members"]=Json::array({{{"user_id","1002"},{"username","Updated"}},{{"user_id","1004"},{"username","New"}}});
            auto result=db.apply(partial,t);
            require(result[0]["removed"]==0&&result[0]["added"]==1&&db.servers(t)[0]["member_count"]==3,"Partial merge removed or duplicated known members");++passed;
            require(db.servers(t)[0]["coverage"]=="Partial cache","Partial coverage missing");++passed;
            require(db.preview("1001","300",t)["common_servers"].empty(),"Partial import refreshed an unseen stale member");
            require(db.preview("1004","300",t)["common_servers"].size()==1,"New partial member not available");++passed;
            require(db.apply(partial,t)[0]["status"]=="unchanged","Partial replay not idempotent");++passed;
            auto empty=partial;empty["captured_at"]=stamp(t+1);empty["members"]=Json::array();db.apply(empty,t+1);
            require(db.servers(t)[0]["member_count"]==3,"Empty partial deleted members");++passed;
            Database reopened(folder/L"partial.sqlite3");require(reopened.servers(t)[0]["member_count"]==3&&reopened.servers(t)[0]["coverage"]=="Partial cache","Partial data lost on restart");++passed;
            auto complete=partial;complete["complete"]=true;complete["captured_at"]=stamp(t+2);complete["members"]=Json::array();db.apply(complete,t+2);
            require(db.servers(t)[0]["member_count"]==0&&db.servers(t)[0]["complete"]==true,"Complete snapshot did not replace partial cache");++passed;
        }

        {
            auto t=now();Database db(folder/L"browsing.sqlite3");
            Json snapshot={{"server_id","900"},{"server_name","Browse server"},{"complete",false},{"captured_at",stamp(t)},
                {"members",Json::array({{{"user_id","9"},{"username","Zeta"}},{{"user_id","10"},{"username","100% name"}},{{"user_id","100"},{"username","Alpha"}}})}};
            db.apply(snapshot,t);db.apply_account({{"user_id","9"},{"username","Zeta"},{"captured_at",stamp(t)},{"guilds",Json::array({{{"server_id","900"},{"server_name","Browse server"}}})}});
            require(db.servers(t).size()==1&&db.servers(t)[0]["member_count"]==3,"Duplicate server rows or double-counted connected member");++passed;
            auto rows=db.members_page("",1,false);require(rows["total"]==3&&rows["rows"][0]["user_id"]=="9"&&rows["rows"][1]["user_id"]=="10","Member IDs must sort numerically");++passed;
            require(db.members_page("",1,true)["rows"][0]["user_id"]=="100","Descending member IDs");
            require(db.members_page("%",0,false)["total"]==1,"Member search wildcard not escaped");++passed;
            require(db.members_page("browse",0,false)["total"]==3&&db.member_servers("9").size()==1,"Member server search/detail mismatch");++passed;
            snapshot["captured_at"]=stamp(t+1);snapshot["members"]=Json::array();for(int n=1;n<=100001;++n)snapshot["members"].push_back({{"user_id",std::to_string(n)},{"username","Member"}});
            db.apply(snapshot,t+1);require(db.members_page("",1,false,500)["rows"].size()==500&&db.members_page("",1,false,100000)["rows"].size()==1,"Large cache or member pagination failed");++passed;
            GatewayMembers gateway("900","large");gateway.event("READY",{{"guilds",Json::array({{{"id","900"},{"name","Large server"},{"member_count",2000000},{"members",Json::array()}}})}});
            require(gateway.found()&&gateway.progress().total==2000000,"Large server total blocked collection");++passed;
        }
        {
            auto path=folder/L"counts.sqlite3";auto t=now();
            Json saved={{"server_id","800"},{"server_name","Counts"},{"captured_at",stamp(t)},{"server_total",2000000}};
            {Database db(path);db.save_server_count(saved);require(db.servers()[0]["server_total"]==2000000&&db.members_page("",0,false)["total"]==0,"Count created fake members");++passed;
             auto older=saved;older["captured_at"]=stamp(t-1);older["server_total"]=1;db.save_server_count(older);require(db.servers()[0]["server_total"]==2000000,"Older count overwrote newer count");++passed;}
            {Database db(path);require(db.servers()[0]["server_total"]==2000000,"Count lost on restart");++passed;}
        }
        {
            Database db(folder/L"scan-options.sqlite3");auto t=now();constexpr Time day=86400000000LL;
            db.apply(example("700","Age test",{"1","2"},t-8*day),t);
            db.save_server_count({{"server_id","700"},{"server_name","Age test"},{"server_total",5},{"captured_at",stamp(t)}});
            require(db.scan_due("700",false,7,t)&&!db.scan_due("700",true,7,t),"Scan ages not independent");++passed;
            for(int days:{1,2,7,14,30}){require(db.scan_due("999",false,days,t)&&db.scan_due("999",true,days,t),"Never scanned omitted");require(db.scan_due("700",true,days,t+days*day),"Exact cutoff not due");}
            require(!db.scan_due("700",false,14,t)&&db.scan_due("700",true,0,t),"Age filter or All incorrect");++passed;
            db.apply(example("700","Age test",{"2"},t-2),t,false);
            require(db.member_servers("1").size()==1&&db.member_servers("1")[0]["last_seen"]==stamp(t-8*day),"No removal lost or refreshed absent member");++passed;
            auto partial=example("700","Age test",{},t-1);partial["complete"]=false;db.apply(partial,t,true);
            require(db.members_page("",0,false)["total"]==2,"Partial scan removed members");++passed;
            db.apply(example("700","Age test",{"2"},t),t,true);
            require(db.member_servers("1").empty()&&db.members_page("",0,false)["total"]==1,"Complete scan did not remove departed member");++passed;
        }
        {
            auto counts=extract_server_counts({{"guilds",Json::array({{{"id","10"},{"name","Ten"},{"member_count",0}},{{"id","20"},{"properties",{{"name","Twenty"}}},{"member_count",2000000}},{{"id","30"},{"name","Unknown"}}})}}, {"20","10","30","40","20"});
            require(counts.size()==4&&counts[0]["server_total"]==2000000&&counts[1]["server_total"]==0,"Count batch extraction or deduplication failed");++passed;
            require(counts[0]["captured_at"]==counts[1]["captured_at"]&&counts[0]["server_name"]=="Twenty","Count timestamp/name mismatch");++passed;
            require(counts[2].contains("error")&&counts[3].contains("error")&&!counts[2].contains("server_total"),"Missing count became zero");++passed;
        }
        require(std::filesystem::equivalent(folder.parent_path(),std::filesystem::temp_directory_path()) && folder.filename().wstring().starts_with(L"sml-native-test-"),"Unsafe temporary cleanup path");
        std::filesystem::remove_all(folder);
        std::cout<<"PASS: "<<passed<<" native checks (A/B/C, removal, restart, stale/partial/conflicting imports, transaction rollback, 3001 members, inbox).\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL after "<<passed<<" checks: "<<e.what()<<"\nTest data: "<<utf8(folder.wstring())<<'\n';return 1;}
}
