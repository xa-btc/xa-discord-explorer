// Window-thread state. Only the active HTTP worker uses the client and writes its reply.
using AuditMessageKey=std::pair<std::string,std::string>; // channel, message
struct AuditMessageRecord {
    Json result;
    std::string cell;
    std::chrono::steady_clock::time_point retry_after;
};
std::string audit_message_account,audit_message_guild;
std::map<AuditMessageKey,AuditMessageRecord> audit_message_cache;
std::map<AuditMessageKey,std::vector<size_t>> audit_message_indices;
std::vector<AuditMessageKey> audit_message_queue;
std::set<AuditMessageKey> audit_message_queued;
size_t audit_message_next=0,audit_message_indexed=0;
std::optional<AuditMessageKey> audit_message_priority;
std::unique_ptr<ChatClient> audit_message_client;
std::thread audit_preview_thread;
std::atomic<bool> audit_preview_cancel=false;
bool audit_preview_closing=false;
AuditMessageKey audit_message_inflight;
Json audit_message_reply;
std::string audit_rendered_preview;

bool audit_messages_idle() {
    return !audit_preview_thread.joinable()&&audit_message_next==audit_message_queue.size();
}

bool audit_is_message(const Json& row) {
    auto type=row.at("action_type").get<int>();
    return type==74||type==75;
}

std::optional<AuditMessageKey> audit_message_key(const Json& row) {
    if(!audit_is_message(row)||row.at("link").get_ref<const std::string&>().empty())return {};
    return AuditMessageKey{row.at("channel").get<std::string>(),row.at("message").get<std::string>()};
}

const std::string& audit_message_body(const Json& row) {
    static const std::string empty;
    if(!audit_is_message(row)) {
        auto summary=row.find("change_summary");
        return summary!=row.end()?summary->get_ref<const std::string&>():empty;
    }
    auto key=audit_message_key(row);
    if(!key)return empty;
    auto found=audit_message_cache.find(*key);
    if(found==audit_message_cache.end()||found->second.result.at("status")!="available")return empty;
    return found->second.result.at("text").get_ref<const std::string&>();
}

std::string audit_single_line(const std::string& text) {
    std::string cell;
    cell.reserve(text.size());
    for(char c:text) {
        if(c=='\r'||c=='\n'||c=='\t') {
            if(!cell.empty()&&cell.back()!=' ')cell+=' ';
        } else cell+=c;
    }
    return cell;
}

std::string audit_message_cell(const Json& row) {
    if(!audit_is_message(row))return audit_single_line(audit_message_body(row));
    auto key=audit_message_key(row);
    if(!key)return "Message unavailable";
    auto found=audit_message_cache.find(*key);
    return found==audit_message_cache.end()?"Loading...":found->second.cell;
}

bool audit_message_needs_fetch(const AuditMessageKey& key) {
    auto found=audit_message_cache.find(key);
    return found==audit_message_cache.end()||std::chrono::steady_clock::now()>=found->second.retry_after;
}

void audit_reset_message_queue() {
    audit_message_queue.clear();
    audit_message_queued.clear();
    audit_message_next=0;
}

void audit_pause_messages() {
    audit_preview_cancel=true;
    audit_message_priority.reset();
    audit_reset_message_queue();
}

void audit_reset_message_rows(HWND,bool changed_server) {
    audit_message_priority.reset();
    audit_reset_message_queue();
    audit_message_indices.clear();
    audit_message_indexed=0;
    if(changed_server) {
        audit_preview_cancel=true;
        audit_message_cache.clear();
        audit_message_account.clear();
        audit_message_guild.clear();
        if(!audit_preview_thread.joinable())audit_message_client.reset();
    }
}

bool audit_message_scope_ready(HWND window) {
    if(audit_preview_closing||close_requested||TabCtrl_GetCurSel(GetDlgItem(window,Tabs))!=4)return false;
    auto account=audit_account(window),guild=audit_guild(window);
    if(account.empty()||guild.empty()||account!=audit_loaded_account||!sessions.contains(account))return false;
    if(account!=audit_message_account||guild!=audit_message_guild) {
        audit_reset_message_rows(window,true);
        audit_message_account=std::move(account);
        audit_message_guild=std::move(guild);
    }
    return true;
}

void audit_set_preview(HWND window,const std::string& text) {
    if(audit_rendered_preview==text)return;
    audit_rendered_preview=text;
    std::string display;
    display.reserve(text.size());
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='\n'&&(i==0||text[i-1]!='\r'))display+='\r';
        display+=text[i];
    }
    set(GetDlgItem(window,AuditPreview),display);
}

void audit_copy_author(Json& row,const Json& result) {
    if(result.at("status")=="available"&&!result.at("author").get_ref<const std::string&>().empty()) {
        row["author"]=result.at("author");
        row["author_id"]=result.at("author_id");
    }
}

void audit_run_next_message(HWND window) {
    if(audit_preview_thread.joinable()||!audit_message_scope_ready(window))return;
    std::optional<AuditMessageKey> next;
    if(audit_message_priority&&audit_message_needs_fetch(*audit_message_priority))next=audit_message_priority;
    while(!next&&audit_message_next<audit_message_queue.size()) {
        const auto& key=audit_message_queue[audit_message_next++];
        if(audit_message_needs_fetch(key))next=key;
    }
    if(!next) {
        audit_reset_message_queue();
        return;
    }
    audit_message_inflight=*next;
    audit_preview_cancel=false;
    std::string token;
    if(!audit_message_client)token=sessions.at(audit_message_account).at("token").get<std::string>();
    audit_preview_thread=std::thread([window,guild=audit_message_guild,key=std::move(*next),token=std::move(token)]()mutable {
        try {
            if(!audit_message_client)audit_message_client=std::make_unique<ChatClient>(token,audit_preview_cancel);
            auto get=[](const std::string& route){return audit_message_client->get(route);};
            audit_message_reply=audit_message_preview(get,guild,key.first,key.second);
        } catch(const std::exception& e) {
            audit_message_reply={{"status","error"},{"text","Could not load this message. "+std::string(e.what())}};
        }
        SecureZeroMemory(token.data(),token.size());
        PostMessageW(window,AuditPreviewDone,0,0);
    });
}

void audit_queue_message_rows(HWND window,size_t first) {
    if(!audit_message_scope_ready(window))return;
    if(first==0)audit_reset_message_queue();
    for(size_t i=std::min(first,audit_message_indexed);i<audit_rows.size();++i) {
        auto& row=audit_rows[i];
        auto key=audit_message_key(row);
        if(!key)continue;
        if(i>=audit_message_indexed) {
            audit_message_indices[*key].push_back(i);
            auto found=audit_message_cache.find(*key);
            if(found!=audit_message_cache.end())audit_copy_author(row,found->second.result);
        }
        if(!audit_checked(row.at("action_type").get<int>())||!audit_message_needs_fetch(*key))continue;
        if(audit_message_queued.insert(*key).second)audit_message_queue.push_back(*key);
    }
    audit_message_indexed=audit_rows.size();
    audit_run_next_message(window);
}

void audit_select_preview(HWND window,const Json* row) {
    audit_message_priority.reset();
    if(audit_preview_closing)return;
    if(!row) {
        audit_set_preview(window,"Select an audit entry to preview it.");
        return;
    }
    if(!audit_is_message(*row)) {
        const auto& summary=audit_message_body(*row);
        audit_set_preview(window,summary.empty()?"No change summary available for this entry.":summary);
        return;
    }
    if(!audit_message_scope_ready(window)) {
        audit_set_preview(window,"Select an audit entry to preview it.");
        return;
    }
    audit_message_priority=audit_message_key(*row);
    if(!audit_message_priority) {
        audit_set_preview(window,"This audit entry has no message link.");
        return;
    }
    auto found=audit_message_cache.find(*audit_message_priority);
    if(found==audit_message_cache.end())audit_set_preview(window,"Loading message...");
    else audit_set_preview(window,found->second.result.at("text").get_ref<const std::string&>());
    audit_run_next_message(window);
}

void audit_finish_preview(HWND window) {
    if(!audit_preview_thread.joinable())return;
    audit_preview_thread.join();
    auto reply=std::move(audit_message_reply);
    if(audit_preview_cancel||audit_preview_closing) {
        audit_message_client.reset();
    } else {
        auto state=reply.at("status").get<std::string>();
        std::string cell;
        auto retry=std::chrono::steady_clock::time_point::max();
        if(state=="available") {
            retry=std::chrono::steady_clock::now()+std::chrono::minutes(5);
            cell=audit_single_line(reply.at("text").get_ref<const std::string&>());
        } else if(state=="forbidden") {
            cell="Access denied";
            retry=std::chrono::steady_clock::now()+std::chrono::minutes(1);
        } else if(state=="unavailable") {
            cell="Message unavailable";
            retry=std::chrono::steady_clock::now()+std::chrono::minutes(1);
        } else if(state=="not_found")cell="Message unavailable";
        else {
            cell="Could not load message";
            retry=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        }
        auto& record=audit_message_cache[audit_message_inflight];
        record={std::move(reply),std::move(cell),retry};
        auto rows=audit_message_indices.find(audit_message_inflight);
        if(rows!=audit_message_indices.end()) {
            for(size_t index:rows->second)audit_copy_author(audit_rows.at(index),record.result);
            if(audit_message_priority==audit_message_inflight)
                audit_set_preview(window,record.result.at("text").get_ref<const std::string&>());
            audit_message_rows_changed(window);
        }
    }
    audit_run_next_message(window);
}

void audit_stop_preview() {
    audit_preview_closing=true;
    audit_pause_messages();
    if(audit_preview_thread.joinable())audit_preview_thread.join();
    audit_message_client.reset();
}
