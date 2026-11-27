#include "audit.h"
#include <algorithm>
#include <charconv>
#include <iterator>
#include <limits>

namespace sml {
namespace {
constexpr uint64_t administrator=1ULL<<3, view_audit_log=1ULL<<7;
constexpr uint64_t discord_epoch_ms=1420070400000ULL;

void check_cancel(const std::atomic<bool>& cancel) {
    if(cancel)throw std::runtime_error("Audit request cancelled.");
}

std::string optional_id(const Json& object,const char* key) {
    auto field=object.find(key);
    if(field==object.end()||field->is_null())return {};
    try {return id(*field);}
    catch(const std::runtime_error&) {return {};}
}

std::string string_value(const Json& object,const char* key) {
    auto field=object.find(key);
    return field!=object.end()&&field->is_string()?field->get<std::string>():std::string();
}

void append_preview_line(std::string& text,const std::string& line) {
    if(line.empty())return;
    if(!text.empty())text+='\n';
    text+=line;
}

bool read_permission_bits(const Json& value,uint64_t& bits) {
    if(value.is_number_unsigned()) {
        bits=value.get<uint64_t>();
        return true;
    }
    if(value.is_number_integer()&&value.get<int64_t>()>=0) {
        bits=static_cast<uint64_t>(value.get<int64_t>());
        return true;
    }
    if(!value.is_string())return false;
    const auto& text=value.get_ref<const std::string&>();
    auto parsed=std::from_chars(text.data(),text.data()+text.size(),bits);
    return parsed.ec==std::errc()&&parsed.ptr==text.data()+text.size();
}

bool can_view_audit(const Json& guild) {
    if(guild.value("owner",false))return true;
    const auto field=guild.find("permissions");
    if(field==guild.end()||field->is_null())return false;
    uint64_t bits=0;
    if(!read_permission_bits(*field,bits))
        throw std::runtime_error("Discord returned invalid server permissions.");
    return (bits&(administrator|view_audit_log))!=0;
}
}

Json audit_servers(const ChatGet& get,const std::string& account,const std::atomic<bool>& cancel) {
    check_cancel(cancel);
    auto profile=get("users/@me");
    check_cancel(cancel);
    if(!profile.is_object()||profile.value("bot",false)||id(profile.at("id"))!=id(account))
        throw std::runtime_error("Reconnect the selected Discord account in View Servers.");
    Json servers=Json::array();
    uint64_t after=0;
    for(;;) {
        check_cancel(cancel);
        auto page=get("users/@me/guilds?limit=200&after="+std::to_string(after));
        check_cancel(cancel);
        if(!page.is_array()||page.size()>200)throw std::runtime_error("Invalid Discord server list.");
        auto next=after;
        std::set<uint64_t> seen;
        for(const auto& guild:page) {
            auto server=id(guild.at("id"));
            auto number=std::stoull(server);
            if(number<=after||!seen.insert(number).second)
                throw std::runtime_error("Discord server pagination did not advance.");
            next=std::max(next,static_cast<uint64_t>(number));
            if(can_view_audit(guild))servers.push_back({{"id",server},{"name",guild.at("name").get<std::string>()}});
        }
        if(page.size()<200)break;
        after=next;
    }
    std::sort(servers.begin(),servers.end(),[](const Json& a,const Json& b){
        if(a["name"]!=b["name"])return a["name"]<b["name"];
        return a["id"]<b["id"];
    });
    return servers;
}

Json audit_message_preview(const ChatGet& get,const std::string& guild,const std::string& channel,const std::string& message) {
    const auto server_id=id(guild),channel_id=id(channel),message_id=id(message);
    Json nearby;
    try {
        // User sessions read messages through channel history, which enforces channel access and history permissions.
        nearby=get("channels/"+channel_id+"/messages?around="+message_id+"&limit=1");
    } catch(const ChatError& error) {
        if(error.status==403)return {{"status","forbidden"},{"text","This account cannot read this message."},{"author",""},{"author_id",""}};
        if(error.status==404)return {{"status","not_found"},{"text","Discord could not find the original message or channel."},{"author",""},{"author_id",""}};
        throw;
    }
    if(!nearby.is_array())throw std::runtime_error("Discord returned an invalid message history response.");
    const Json* match=nullptr;
    for(const auto& candidate:nearby) {
        if(id(candidate.at("channel_id"))!=channel_id||
           (candidate.contains("guild_id")&&id(candidate.at("guild_id"))!=server_id))
            throw std::runtime_error("Discord returned a message outside the selected audit entry.");
        if(id(candidate.at("id"))==message_id)match=&candidate;
    }
    if(!match)return {{"status","unavailable"},{"text","The original message is unavailable in this account's channel history."},
        {"author",""},{"author_id",""}};
    const auto& data=*match;
    const auto& author=data.at("author");
    auto author_id=id(author.at("id"));
    auto name=string_value(author,"username");
    auto text=chat_message_text(data);
    // The chat text parser includes forwarded snapshots. Outer embeds and files have their own fields.
    auto embeds=data.find("embeds");
    if(embeds!=data.end()&&embeds->is_array())for(const auto& embed:*embeds) {
        append_preview_line(text,string_value(embed,"title"));
        append_preview_line(text,string_value(embed,"description"));
        auto fields=embed.find("fields");
        if(fields==embed.end()||!fields->is_array())continue;
        for(const auto& field:*fields) {
            append_preview_line(text,string_value(field,"name"));
            append_preview_line(text,string_value(field,"value"));
        }
    }
    auto attachments=data.find("attachments");
    if(attachments!=data.end()&&attachments->is_array())for(const auto& attachment:*attachments) {
        auto filename=string_value(attachment,"filename");
        if(!filename.empty())append_preview_line(text,"Attachment: "+filename);
    }
    if(text.empty())text="This message has no text to display.";
    return {{"status","available"},{"text",text},{"author",name.empty()?author_id:name},{"author_id",author_id}};
}

namespace {
void append_item(std::string& list,const std::string& item,const char* separator) {
    if(item.empty())return;
    if(!list.empty())list+=separator;
    list+=item;
}

std::string permission_names(uint64_t bits) {
    // Discord permission flags by bit index: https://docs.discord.com/developers/topics/permissions
    static constexpr const char* names[]={
        "Create Invites","Kick Members","Ban Members","Administrator","Manage Channels","Manage Server",
        "Add Reactions","View Audit Log","Priority Speaker","Video","View Channel","Send Messages",
        "Send Text-to-Speech Messages","Manage Messages","Embed Links","Attach Files","Read Message History",
        "Mention Everyone","Use External Emojis","View Server Insights","Connect","Speak","Mute Members",
        "Deafen Members","Move Members","Use Voice Activity","Change Nickname","Manage Nicknames","Manage Roles",
        "Manage Webhooks","Manage Expressions","Use Application Commands","Request to Speak","Manage Events",
        "Manage Threads","Create Public Threads","Create Private Threads","Use External Stickers",
        "Send Messages in Threads","Use Activities","Moderate Members","View Creator Monetization Analytics",
        "Use Soundboard","Create Expressions","Create Events","Use External Sounds","Send Voice Messages",
        nullptr,"Set Voice Channel Status","Send Polls","Use External Apps","Pin Messages","Bypass Slowmode"
    };
    std::string list;
    for(size_t bit=0;bits;++bit,bits>>=1) {
        if(bits&1) {
            const char* name=bit<std::size(names)?names[bit]:nullptr;
            append_item(list,name?name:"Permission bit "+std::to_string(bit),", ");
        }
    }
    return list;
}

std::string role_names(const Json& roles) {
    std::string list;
    for(const auto& role:roles) {
        auto name=string_value(role,"name");
        if(name.empty()) {
            auto role_id=optional_id(role,"id");
            if(!role_id.empty())name="Role "+role_id;
        }
        append_item(list,name,", ");
    }
    return list;
}

std::string audit_field_label(std::string key) {
    if(key=="nick")return "Nickname";
    if(key=="communication_disabled_until")return "Timeout until";
    if(key=="mute")return "Server muted";
    if(key=="deaf")return "Server deafened";
    std::replace(key.begin(),key.end(),'_',' ');
    if(!key.empty()&&key[0]>='a'&&key[0]<='z')key[0]-='a'-'A';
    return key;
}

std::string audit_scalar(const Json& value) {
    if(value.is_null())return "(none)";
    if(value.is_string()) {
        const auto& text=value.get_ref<const std::string&>();
        return text.empty()?"(empty)":text;
    }
    if(value.is_boolean())return value.get<bool>()?"Yes":"No";
    return value.dump();
}

std::string audit_change_summary(const Json& entry,int type,const std::string& subject) {
    auto changes=entry.find("changes");
    if(changes==entry.end()||!changes->is_array())return {};
    const bool overwrite=type>=13&&type<=15;
    std::string summary;
    const Json missing;
    for(const auto& change:*changes) {
        auto key=string_value(change,"key");
        if(key.empty())continue;
        const auto& before=change.contains("old_value")?change.at("old_value"):missing;
        const auto& after=change.contains("new_value")?change.at("new_value"):missing;
        if(type==25&&(key=="$add"||key=="$remove")) {
            // Discord normally sends both kinds of role delta in new_value.
            const auto& roles=after.is_array()?after:before;
            if(roles.is_array()) {
                auto names=role_names(roles);
                if(!names.empty())append_item(summary,(key=="$add"?"Added roles: ":"Removed roles: ")+names,". ");
            }
            continue;
        }
        if((overwrite&&(key=="allow"||key=="deny"))||(type>=30&&type<=32&&key=="permissions")) {
            uint64_t old_bits=0,new_bits=0;
            if((before.is_null()||read_permission_bits(before,old_bits))&&
               (after.is_null()||read_permission_bits(after,new_bits))) {
                // Losing an explicit deny or allow does not determine effective channel permissions.
                const char* added=key=="deny"?"Denied ":key=="allow"?"Allowed ":"Added permissions: ";
                const char* removed=key=="deny"?"No longer denied ":key=="allow"?"No longer allowed ":"Removed permissions: ";
                if(auto bits=new_bits&~old_bits)append_item(summary,added+permission_names(bits),". ");
                if(auto bits=old_bits&~new_bits)append_item(summary,removed+permission_names(bits),". ");
                continue;
            }
        }
        if(before==after)continue;
        auto label=audit_field_label(key);
        if(!before.is_primitive()||!after.is_primitive())append_item(summary,label+" changed",". ");
        else if(before.is_null())append_item(summary,label+": "+audit_scalar(after),". ");
        else append_item(summary,label+": "+audit_scalar(before)+" → "+audit_scalar(after),". ");
    }
    if(summary.empty())return {};
    if(overwrite&&!subject.empty())summary=subject+": "+summary;
    if(summary.back()!='.')summary+='.';
    return summary;
}

struct AuditRow {
    uint64_t id;
    Json value;
};

struct AuditCursor {
    std::string route;
    int action=-1;
    uint64_t before=0;
    bool finished=false;
    std::vector<AuditRow> buffered;
    size_t next=0;
};

std::string audit_username(const std::string& user_id,const std::map<std::string,std::string>& usernames) {
    auto found=usernames.find(user_id);
    return found==usernames.end()?user_id:found->second;
}

bool member_target_action(int type) {
    // A prune represents a group, whereas individual member and AutoMod actions can identify a user.
    return type==20||(type>=22&&type<=28)||(type>=143&&type<=146);
}

void read_audit_page(AuditCursor& cursor,const ChatGet& get,const std::string& guild,
                     const std::atomic<bool>& cancel,std::map<std::string,std::string>& usernames) {
    check_cancel(cancel);
    auto route=cursor.route;
    if(cursor.before)route+="&before="+std::to_string(cursor.before);
    Json response;
    try {
        response=get(route);
    } catch(const ChatError& e) {
        if(e.status==403)throw std::runtime_error("Discord denied audit log access. Check this account's View Audit Log permission and refresh servers.");
        throw;
    }
    check_cancel(cancel);
    auto& entries=response.at("audit_log_entries");
    if(!entries.is_array()||entries.size()>100)throw std::runtime_error("Invalid Discord audit log page.");
    if(entries.empty()) {
        cursor.finished=true;
        return;
    }
    auto users=response.find("users");
    if(users!=response.end()&&users->is_array()) {
        for(const auto& user:*users) {
            auto uid=optional_id(user,"id");
            auto name=string_value(user,"username");
            if(!uid.empty()&&!name.empty())usernames[uid]=name;
        }
    }
    std::vector<AuditRow> page;
    auto oldest=cursor.before;
    for(auto& entry:entries) {
        check_cancel(cancel);
        auto number=std::stoull(id(entry.at("id")));
        if(cursor.before&&number>=cursor.before)continue;
        if(number==oldest)continue;
        if(oldest&&number>oldest)throw std::runtime_error("Discord returned audit entries out of order.");
        oldest=number;
        auto action=entry.find("action_type");
        int type=-1;
        if(action!=entry.end()&&action->is_number_integer()&&*action>=0&&*action<=std::numeric_limits<int>::max())
            type=action->get<int>();
        if(cursor.action>=0&&type!=cursor.action) {
            if(type<0)continue; // Discord occasionally omits the action type. Never mislabel that row as a pin.
            throw std::runtime_error("Discord returned an audit entry outside the requested action.");
        }
        auto actor=optional_id(entry,"user_id");
        auto name=actor.empty()?"Unknown":audit_username(actor,usernames);
        const bool is_pin_event=type==74||type==75;
        const bool is_overwrite=type>=13&&type<=15;
        auto options=entry.find("options");
        std::string author_id,author,channel;
        if(is_pin_event||member_target_action(type)) {
            author_id=optional_id(entry,"target_id");
            author=audit_username(author_id,usernames);
        }
        if(is_overwrite) {
            channel=optional_id(entry,"target_id");
            if(options!=entry.end()) {
                auto kind=options->find("type");
                if(kind!=options->end()&&(*kind=="0"||*kind==0)) {
                    author_id=optional_id(*options,"id");
                    author=string_value(*options,"role_name");
                    if(author.empty())author=author_id==guild?"@everyone":author_id;
                } else if(kind!=options->end()&&(*kind=="1"||*kind==1)) {
                    author_id=optional_id(*options,"id");
                    author=audit_username(author_id,usernames);
                }
            }
        } else if(options!=entry.end())channel=optional_id(*options,"channel_id");
        auto message=options==entry.end()?std::string():optional_id(*options,"message_id");
        std::string link;
        if(!channel.empty()&&!message.empty()&&is_pin_event)
            link="https://discord.com/channels/"+guild+"/"+channel+"/"+message;
        auto at=static_cast<Time>(((number>>22)+discord_epoch_ms)*1000);
        page.push_back({number,{{"at",stamp(at).substr(0,19)+" UTC"},{"action_type",type},{"actor",name},{"actor_id",actor},
            {"author_id",author_id},{"author",author},
            {"channel",channel},{"message",message},{"link",link},
            {"change_summary",is_pin_event?std::string():audit_change_summary(entry,type,author)},{"entry",std::move(entry)}}});
    }
    if(oldest==cursor.before)throw std::runtime_error("Discord audit log pagination did not advance.");
    cursor.before=oldest;
    cursor.buffered=std::move(page);
    cursor.next=0;
}

void publish_audit_rows(std::vector<AuditCursor>& cursors,uint64_t floor,const AuditPage& publish) {
    std::vector<AuditRow> ready;
    for(auto& cursor:cursors) {
        while(cursor.next<cursor.buffered.size()&&cursor.buffered[cursor.next].id>=floor)
            ready.push_back(std::move(cursor.buffered[cursor.next++]));
    }
    std::sort(ready.begin(),ready.end(),[](const AuditRow& a,const AuditRow& b){return a.id>b.id;});
    Json page=Json::array();
    uint64_t previous=0;
    for(auto& row:ready) {
        if(row.id==previous)continue;
        previous=row.id;
        page.push_back(std::move(row.value));
        if(page.size()==100) {
            publish(std::move(page));
            page=Json::array();
        }
    }
    if(!page.empty())publish(std::move(page));
}
}

void scan_audit_log(const ChatGet& get,const std::string& server,const std::atomic<bool>& cancel,
                    const AuditPage& publish,const std::vector<int>& actions) {
    const auto guild=id(server);
    const auto route="guilds/"+guild+"/audit-logs?limit=100";
    std::vector<AuditCursor> cursors;
    if(actions.empty())cursors.push_back({route});
    for(int action:std::set<int>(actions.begin(),actions.end())) {
        if(action<0)throw std::invalid_argument("An unknown audit action cannot be requested by ID.");
        cursors.push_back({route+"&action_type="+std::to_string(action),action});
    }
    std::map<std::string,std::string> usernames;
    for(;;) {
        uint64_t floor=0;
        try {
            for(auto& cursor:cursors) {
                if(!cursor.finished&&cursor.next==cursor.buffered.size())
                    read_audit_page(cursor,get,guild,cancel,usernames);
                // Every unseen entry in this stream is older than its own cursor.
                if(!cursor.finished)floor=std::max(floor,cursor.before);
            }
        } catch(...) {
            publish_audit_rows(cursors,0,publish); // Keep completed pages when another selected action fails.
            throw;
        }
        publish_audit_rows(cursors,floor,publish);
        if(!floor)return;
    }
}

const std::map<int,std::string>& audit_action_names() {
    // https://docs.discord.com/developers/resources/audit-log#audit-log-events
    static const std::map<int,std::string> names={
        {1,"Server updated"},
        {10,"Channel created"},{11,"Channel updated"},{12,"Channel deleted"},
        {13,"Channel permissions added"},{14,"Channel permissions updated"},{15,"Channel permissions deleted"},{16,"Channels reordered"},
        {20,"Member kicked"},{21,"Members pruned"},{22,"Member banned"},{23,"Member unbanned"},{24,"Member updated"},
        {25,"Member roles updated"},{26,"Voice member moved"},{27,"Voice member disconnected"},{28,"Bot added"},
        {30,"Role created"},{31,"Role updated"},{32,"Role deleted"},{33,"Roles reordered"},
        {40,"Invite created"},{41,"Invite updated"},{42,"Invite deleted"},
        {50,"Webhook created"},{51,"Webhook updated"},{52,"Webhook deleted"},
        {60,"Emoji created"},{61,"Emoji updated"},{62,"Emoji deleted"},
        {72,"Message deleted"},{73,"Messages bulk deleted"},{74,"Message pinned"},{75,"Message unpinned"},
        {80,"Integration added"},{81,"Integration updated"},{82,"Integration removed"},
        {83,"Stage instance created"},{84,"Stage instance updated"},{85,"Stage instance deleted"},
        {90,"Sticker created"},{91,"Sticker updated"},{92,"Sticker deleted"},
        {100,"Scheduled event created"},{101,"Scheduled event updated"},{102,"Scheduled event cancelled"},
        {110,"Thread created"},{111,"Thread updated"},{112,"Thread deleted"},
        {121,"Command permissions updated"},
        {130,"Soundboard sound created"},{131,"Soundboard sound updated"},{132,"Soundboard sound deleted"},
        {140,"AutoMod rule created"},{141,"AutoMod rule updated"},{142,"AutoMod rule deleted"},
        {143,"AutoMod message blocked"},{144,"AutoMod message flagged"},{145,"AutoMod member timed out"},{146,"AutoMod member quarantined"},
        {150,"Monetization request created"},{151,"Monetization terms accepted"},
        {163,"Onboarding question created"},{164,"Onboarding question updated"},{165,"Onboarding question deleted"},
        {166,"Onboarding created"},{167,"Onboarding updated"},
        {190,"Server Guide created"},{191,"Server Guide updated"},{192,"Voice channel status set"},{193,"Voice channel status deleted"},
    };
    return names;
}

std::string audit_action_name(int action) {
    if(action==-1)return "Unknown action";
    const auto& names=audit_action_names();
    auto found=names.find(action);
    return found==names.end()?"Action "+std::to_string(action):found->second;
}
}
