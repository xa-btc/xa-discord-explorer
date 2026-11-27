constexpr int AuditAccountLabel=400,AuditAccount=401,AuditRefresh=402,AuditGuildLabel=403,AuditGuild=404,
    AuditFilters=405,AuditScan=406,AuditCancel=407,AuditTable=408,AuditStatus=409,AuditDetails=410,AuditOpen=411,
    AuditFilterLabel=412,AuditAll=413,AuditNone=414,AuditCount=415,AuditPreviewLabel=416,AuditPreview=417,
    AuditCopyLink=418,AuditMessageSearch=419,AuditSearchLabel=420;
constexpr int audit_controls[]={AuditAccountLabel,AuditAccount,AuditRefresh,AuditGuildLabel,AuditGuild,
    AuditFilters,AuditScan,AuditCancel,AuditTable,AuditStatus,AuditDetails,AuditOpen,AuditFilterLabel,AuditAll,AuditNone,AuditCount,AuditPreviewLabel,AuditPreview,
    AuditCopyLink,AuditMessageSearch,AuditSearchLabel};
constexpr UINT AuditDone=WM_APP+5;
constexpr UINT AuditUpdated=WM_APP+6;
constexpr UINT AuditPreviewDone=WM_APP+7;
constexpr UINT_PTR AuditFilterTimer=4;
bool audit_running=false,audit_refreshing=false,audit_rebuilding_filters=false;
bool audit_rebuilding_rows=false;
std::wstring audit_message_query;
bool audit_filter_pending=false,audit_superseded=false,audit_default_checked=true;
using AuditActions=std::vector<int>;
AuditActions audit_active_actions;
std::optional<AuditActions> audit_cached_actions;
size_t audit_pages_received=0;
std::vector<Json> audit_pending_pages;
std::string audit_pending_status;
std::string audit_loaded_account;
std::vector<std::string> audit_account_ids;
std::set<int> audit_action_overrides;
std::set<int> audit_extra_actions;
std::vector<size_t> audit_visible_rows;
Json audit_guilds=Json::array(),audit_rows=Json::array();
struct AuditResult {
    Json data;
    std::string account,error;
};
AuditResult audit_result;

const Json* audit_row(HWND window,int index=-1) {
    if(index==-1)index=ListView_GetNextItem(GetDlgItem(window,AuditTable),-1,LVNI_SELECTED);
    return index>=0&&static_cast<size_t>(index)<audit_visible_rows.size()?&audit_rows[audit_visible_rows[index]]:nullptr;
}

std::string audit_account(HWND window) {
    auto index=SendMessageW(GetDlgItem(window,AuditAccount),CB_GETCURSEL,0,0);
    return index>=0&&static_cast<size_t>(index)<audit_account_ids.size()?audit_account_ids[index]:"";
}

std::string audit_guild(HWND window) {
    auto index=SendMessageW(GetDlgItem(window,AuditGuild),CB_GETCURSEL,0,0);
    return index>=0&&static_cast<size_t>(index)<audit_guilds.size()?audit_guilds[index]["id"].get<std::string>():"";
}

bool audit_checked(int action) {
    return audit_default_checked!=audit_action_overrides.contains(action);
}

// No value means None. An empty action list requests Discord's unfiltered feed.
std::optional<AuditActions> audit_selected_actions() {
    AuditActions checked;
    for(const auto& [action,name]:audit_action_names())if(audit_checked(action))checked.push_back(action);
    for(int action:audit_extra_actions)if(audit_checked(action))checked.push_back(action);
    if(checked.empty())return std::nullopt;
    if(checked.size()==audit_action_names().size()+audit_extra_actions.size())return AuditActions{};
    std::sort(checked.begin(),checked.end());
    // Entries with no action ID can only be discovered through the unfiltered feed.
    if(checked.front()<0)return AuditActions{};
    return checked;
}

bool audit_cache_covers(const AuditActions& requested) {
    if(!audit_cached_actions)return false;
    const auto& cached=*audit_cached_actions;
    return cached.empty()||(!requested.empty()&&std::includes(cached.begin(),cached.end(),requested.begin(),requested.end()));
}

void audit_stop_filter_timer(HWND window) {
    KillTimer(window,AuditFilterTimer);
    audit_filter_pending=false;
}

void audit_update_enabled(HWND window) {
    auto account=audit_account(window);
    EnableWindow(GetDlgItem(window,AuditAccount),!busy&&!audit_account_ids.empty());
    EnableWindow(GetDlgItem(window,AuditRefresh),!busy&&!account.empty());
    EnableWindow(GetDlgItem(window,AuditGuild),!busy&&!audit_guilds.empty());
    EnableWindow(GetDlgItem(window,AuditScan),!busy&&!account.empty()&&account==audit_loaded_account&&!audit_guild(window).empty()&&audit_selected_actions().has_value());
    EnableWindow(GetDlgItem(window,AuditCancel),audit_filter_pending||(audit_running&&!cancel_collection));
    auto row=audit_row(window);
    bool has_link=row&&!(*row)["link"].get_ref<const std::string&>().empty();
    EnableWindow(GetDlgItem(window,AuditOpen),has_link);
    EnableWindow(GetDlgItem(window,AuditCopyLink),has_link);
}

void audit_message_rows_changed(HWND window);
#include "audit_message_ui.inc"

void audit_details(HWND window) {
    auto row=audit_row(window);
    std::string text;
    if(row) {
        auto link=row->at("link").get<std::string>();
        if(!link.empty())text=link+"\r\n\r\n";
        auto data=row->at("entry").dump(2);
        for(char c:data){if(c=='\n')text+='\r';text+=c;}
    }
    set(GetDlgItem(window,AuditDetails),text);
    audit_select_preview(window,row);
    audit_update_enabled(window);
}

void audit_update_count(HWND window) {
    set(GetDlgItem(window,AuditCount),std::to_string(audit_visible_rows.size())+" of "+std::to_string(audit_rows.size())+" entries shown");
}

bool audit_text_contains(const std::string& text) {
    if(text.empty())return false;
    auto source=wide(text);
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT,FIND_FROMSTART|NORM_IGNORECASE,
        source.data(),static_cast<int>(source.size()),audit_message_query.data(),static_cast<int>(audit_message_query.size()),
        nullptr,nullptr,nullptr,0)>=0;
}

bool audit_row_matches(const Json& row) {
    if(!audit_checked(row.at("action_type").get<int>()))return false;
    if(audit_message_query.empty())return true;
    return audit_text_contains(row.at("author").get_ref<const std::string&>())||audit_text_contains(audit_message_body(row));
}

void audit_apply_filters(HWND window,bool preserve=false) {
    auto list=GetDlgItem(window,AuditTable);
    std::vector<size_t> visible;
    for(size_t i=0;i<audit_rows.size();++i)if(audit_row_matches(audit_rows[i]))visible.push_back(i);
    if(!preserve||visible!=audit_visible_rows) {
        const int selected=ListView_GetNextItem(list,-1,LVNI_SELECTED);
        const size_t old_row=preserve&&selected>=0&&static_cast<size_t>(selected)<audit_visible_rows.size()?audit_visible_rows[selected]:audit_rows.size();
        audit_rebuilding_rows=true;
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        audit_visible_rows=std::move(visible);
        ListView_SetItemCountEx(list,static_cast<int>(audit_visible_rows.size()),LVSICF_NOSCROLL);
        auto item=std::lower_bound(audit_visible_rows.begin(),audit_visible_rows.end(),old_row);
        if(item!=audit_visible_rows.end()&&*item==old_row)
            ListView_SetItemState(list,static_cast<int>(item-audit_visible_rows.begin()),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        audit_rebuilding_rows=false;
    }
    InvalidateRect(list,nullptr,TRUE);
    audit_update_count(window);
    audit_details(window);
}

void audit_message_rows_changed(HWND window) {
    if(!audit_message_query.empty())audit_apply_filters(window,true);
    else InvalidateRect(GetDlgItem(window,AuditTable),nullptr,FALSE);
}

void audit_fill_filters(HWND window) {
    std::map<int,std::string> names=audit_action_names();
    for(int action:audit_extra_actions)names.emplace(action,audit_action_name(action));
    std::vector<std::pair<int,std::string>> sorted(names.begin(),names.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second<b.second;});
    auto list=GetDlgItem(window,AuditFilters);
    audit_rebuilding_filters=true;
    SendMessageW(list,WM_SETREDRAW,FALSE,0);
    ListView_DeleteAllItems(list);
    for(const auto& [action,name]:sorted) {
        auto label=wide(name);
        LVITEMW item{};
        item.mask=LVIF_TEXT|LVIF_PARAM;
        item.iItem=ListView_GetItemCount(list);
        item.pszText=label.data();
        item.lParam=action;
        int index=ListView_InsertItem(list,&item);
        if(index>=0)ListView_SetCheckState(list,index,audit_checked(action));
    }
    SendMessageW(list,WM_SETREDRAW,TRUE,0);
    InvalidateRect(list,nullptr,TRUE);
    audit_rebuilding_filters=false;
}

void audit_clear_rows(HWND window,bool changed_server=false) {
    audit_reset_message_rows(window,changed_server);
    ListView_SetItemState(GetDlgItem(window,AuditTable),-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    audit_visible_rows.clear();
    audit_rows=Json::array();
    if(changed_server&&!audit_message_query.empty())set(GetDlgItem(window,AuditMessageSearch),"");
    if(changed_server&&!audit_extra_actions.empty()) {
        audit_extra_actions.clear();
        audit_fill_filters(window);
    }
    audit_apply_filters(window);
}

void audit_display_pages(HWND window) {
    std::vector<Json> pages;
    std::string progress;
    {
        std::lock_guard lock(completed_mutex);
        pages.swap(audit_pending_pages);
        progress.swap(audit_pending_status);
    }
    if(audit_superseded)return;
    if(!pages.empty()) {
        if(!audit_pages_received)audit_clear_rows(window);
        auto first_new=audit_rows.size();
        bool filters_changed=false;
        for(auto& page:pages) {
            ++audit_pages_received;
            for(auto& row:page) {
                int action=row.at("action_type").get<int>();
                if(!audit_action_names().contains(action)&&audit_extra_actions.insert(action).second)filters_changed=true;
                audit_rows.push_back(std::move(row));
            }
        }
        if(filters_changed)audit_fill_filters(window);
        audit_queue_message_rows(window,first_new);
        for(size_t i=first_new;i<audit_rows.size();++i)if(audit_row_matches(audit_rows[i]))audit_visible_rows.push_back(i);
        auto list=GetDlgItem(window,AuditTable);
        ListView_SetItemCountEx(list,static_cast<int>(audit_visible_rows.size()),LVSICF_NOSCROLL|LVSICF_NOINVALIDATEALL);
        InvalidateRect(list,nullptr,FALSE);
        audit_update_count(window);
        if(progress.empty())progress="Received "+std::to_string(audit_rows.size())+" entries. Checking for older activity...";
    }
    if(!progress.empty()&&!cancel_collection)set(GetDlgItem(window,AuditStatus),progress);
}

void audit_clear_scope(HWND window) {
    audit_stop_filter_timer(window);
    audit_cached_actions.reset();
    audit_loaded_account.clear();
    audit_guilds=Json::array();
    SendMessageW(GetDlgItem(window,AuditGuild),CB_RESETCONTENT,0,0);
    audit_clear_rows(window,true);
}

void audit_sync_accounts(HWND window) {
    auto combo=GetDlgItem(window,AuditAccount);
    if(!combo)return;
    auto previous=audit_account(window);
    auto selected=previous;
    if(!sessions.contains(selected)) {
        auto index=SendMessageW(GetDlgItem(window,Accounts),CB_GETCURSEL,0,0);
        selected=index>=0&&static_cast<size_t>(index)<account_ids.size()?account_ids[index]:"";
    }
    SendMessageW(combo,CB_RESETCONTENT,0,0);
    audit_account_ids=account_ids;
    for(size_t i=0;i<audit_account_ids.size();++i) {
        const auto& uid=audit_account_ids[i];
        auto label=wide(sessions.at(uid).at("username").get<std::string>()+" ("+uid+")");
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
        if(uid==selected)SendMessageW(combo,CB_SETCURSEL,i,0);
    }
    if(previous!=selected) {
        audit_clear_scope(window);
        set(GetDlgItem(window,AuditStatus),selected.empty()?"Connect a Discord account in View Servers.":"Refresh servers to check audit log access.");
    }
    audit_update_enabled(window);
}

void audit_begin(HWND window,bool refresh) {
    if(busy)return;
    audit_stop_filter_timer(window);
    auto account=audit_account(window);
    auto guild=audit_guild(window);
    if(account.empty()||!sessions.contains(account)||(!refresh&&(guild.empty()||audit_loaded_account!=account))) {
        set(GetDlgItem(window,AuditStatus),account.empty()?"Connect a Discord account in View Servers.":"Select a server before scanning.");
        return;
    }
    auto actions=audit_selected_actions();
    if(!refresh&&!actions){
        set(GetDlgItem(window,AuditStatus),"Select at least one action to search.");
        return;
    }
    audit_active_actions=actions.value_or(AuditActions{});
    if(!refresh)audit_cached_actions.reset();
    auto token=sessions.at(account).at("token").get<std::string>();
    cancel_collection=false;
    audit_refreshing=refresh;
    audit_superseded=false;
    audit_pages_received=0;
    network_thread=std::thread([window,refresh,guild,account,actions=audit_active_actions,token=std::move(token)]()mutable {
        AuditResult result;
        result.account=account;
        try {
            auto report=[window](const std::string& message) {
                {std::lock_guard lock(completed_mutex);audit_pending_status=message;}
                PostMessageW(window,AuditUpdated,0,0);
            };
            ChatClient client(token,cancel_collection,report);
            auto get=[&](const std::string& route){return client.get(route);};
            if(refresh)result.data=audit_servers(get,account,cancel_collection);
            else scan_audit_log(get,guild,cancel_collection,[window](Json page) {
                {std::lock_guard lock(completed_mutex);audit_pending_pages.push_back(std::move(page));}
                PostMessageW(window,AuditUpdated,0,0);
            },actions);
        } catch(const ChatError& e) {
            if(e.status==401)result.error="Discord session expired. Reconnect the account in View Servers.";
            else result.error="Discord request failed (HTTP "+std::to_string(e.status)+").";
        } catch(const std::exception& e) {
            result.error=e.what();
        }
        SecureZeroMemory(token.data(),token.size());
        audit_result=std::move(result);
        PostMessageW(window,AuditDone,0,0);
    });
    busy=audit_running=true;
    EnableWindow(GetDlgItem(window,Connect),FALSE);
    EnableWindow(GetDlgItem(window,Accounts),FALSE);
    update_accounts(window);
    chat_update_enabled(window);
    set(GetDlgItem(window,AuditStatus),refresh?"Loading servers with View Audit Log permission...":
        audit_active_actions.empty()?"Fetching all audit activity...":"Fetching selected actions from Discord...");
}

void audit_filters_changed(HWND window) {
    audit_apply_filters(window);
    audit_queue_message_rows(window,0);
    audit_stop_filter_timer(window);
    if(audit_guild(window).empty()||audit_loaded_account!=audit_account(window)||(audit_refreshing&&audit_running))return;
    auto desired=audit_selected_actions();
    if(audit_running&&!audit_superseded) {
        if(desired&&*desired==audit_active_actions&&!cancel_collection)return;
        audit_superseded=true;
        cancel_collection=true;
    }
    if(!desired) {
        set(GetDlgItem(window,AuditStatus),"Select at least one action to search.");
    } else if(!audit_running&&audit_cache_covers(*desired)) {
        set(GetDlgItem(window,AuditStatus),"Showing matches from the completed scan.");
    } else {
        audit_filter_pending=true;
        SetTimer(window,AuditFilterTimer,350,nullptr);
        set(GetDlgItem(window,AuditStatus),"Updating the search for the checked actions...");
    }
    audit_update_enabled(window);
}

void audit_filter_tick(HWND window) {
    if(!audit_filter_pending||busy)return;
    audit_begin(window,false);
}

void audit_finish(HWND window) {
    if(!audit_running)return;
    if(network_thread.joinable())network_thread.join();
    audit_display_pages(window);
    busy=audit_running=false;
    auto result=std::move(audit_result);
    std::string outcome;
    if(audit_superseded) {
        // Checkbox or Cancel already set the status for the replacement search.
    } else if(cancel_collection||!result.error.empty()) {
        outcome=cancel_collection?"Audit request cancelled.":result.error;
        if(!audit_refreshing) {
            if(audit_pages_received)outcome+=" Partial results: "+std::to_string(audit_rows.size())+" entries received.";
            else if(audit_rows.empty())outcome+=" No audit page was received.";
            else outcome+=" No new page was received. Showing the previous scan.";
        }
    }
    else if(audit_refreshing) {
        auto selected=audit_guild(window);
        audit_loaded_account=result.account;
        audit_guilds=std::move(result.data);
        auto combo=GetDlgItem(window,AuditGuild);
        SendMessageW(combo,CB_RESETCONTENT,0,0);
        for(size_t i=0;i<audit_guilds.size();++i) {
            const auto& row=audit_guilds[i];
            auto label=wide(row["name"].get<std::string>()+" ("+row["id"].get<std::string>()+")");
            SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(row["id"]==selected)SendMessageW(combo,CB_SETCURSEL,i,0);
        }
        if(selected.empty()||audit_guild(window)!=selected) {
            audit_cached_actions.reset();
            audit_clear_rows(window,true);
        }
        outcome=audit_guilds.empty()?"No servers with View Audit Log permission."
            :std::to_string(audit_guilds.size())+" eligible server(s). Select a server to scan.";
    } else {
        if(!audit_pages_received)audit_clear_rows(window);
        audit_cached_actions=audit_active_actions;
        outcome=audit_rows.empty()?"Discord returned no matching audit entries in the last 45 days."
            :"Scan complete. "+std::to_string(audit_rows.size())+" entries received.";
    }
    if(!outcome.empty())set(GetDlgItem(window,AuditStatus),outcome);
    EnableWindow(GetDlgItem(window,Connect),TRUE);
    EnableWindow(GetDlgItem(window,Accounts),TRUE);
    update_accounts(window);
    chat_update_enabled(window);
    if(close_requested)DestroyWindow(window);
}

void audit_open(HWND window,int index=-1) {
    auto row=audit_row(window,index);
    if(!row)return;
    auto link=wide(row->at("link").get<std::string>());
    if(!link.empty()&&reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",link.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
        set(GetDlgItem(window,AuditStatus),"Could not open the message link. You can copy it from the details.");
}

void audit_copy_message_link(HWND window) {
    auto row=audit_row(window);
    if(!row)return;
    auto link=wide(row->at("link").get<std::string>());
    if(link.empty())return;
    const auto bytes=(link.size()+1)*sizeof(wchar_t);
    auto storage=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(storage) {
        if(auto buffer=GlobalLock(storage)) {
            std::memcpy(buffer,link.c_str(),bytes);
            GlobalUnlock(storage);
            if(OpenClipboard(window)) {
                bool copied=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,storage);
                CloseClipboard();
                if(copied) {
                    set(GetDlgItem(window,AuditStatus),"Message link copied.");
                    return; // The clipboard owns the storage.
                }
            }
        }
        GlobalFree(storage);
    }
    set(GetDlgItem(window,AuditStatus),"Could not copy the message link. Try again.");
}

bool audit_command(HWND window,int cid,int notification) {
    switch(cid) {
    case AuditRefresh:if(notification==BN_CLICKED)audit_begin(window,true);return true;
    case AuditScan:if(notification==BN_CLICKED)audit_begin(window,false);return true;
    case AuditCancel:
        if(notification==BN_CLICKED) {
            audit_stop_filter_timer(window);
            if(audit_running)cancel_collection=true;
            audit_update_enabled(window);
            set(GetDlgItem(window,AuditStatus),audit_running&&!audit_superseded?"Cancelling audit request...":"Search cancelled.");
        }
        return true;
    case AuditAccount:
        if(notification==CBN_SELCHANGE&&!busy) {
            audit_clear_scope(window);
            audit_begin(window,true);
        }
        return true;
    case AuditGuild:
        if(notification==CBN_SELCHANGE&&!busy) {
            audit_stop_filter_timer(window);
            audit_cached_actions.reset();
            audit_clear_rows(window,true);
            if(!audit_guild(window).empty())set(GetDlgItem(window,AuditStatus),"Ready to scan the selected server.");
        }
        return true;
    case AuditAll:case AuditNone:
        if(notification==BN_CLICKED) {
            auto list=GetDlgItem(window,AuditFilters);
            audit_default_checked=cid==AuditAll;
            audit_action_overrides.clear();
            audit_rebuilding_filters=true;
            for(int i=0;i<ListView_GetItemCount(list);++i) {
                ListView_SetCheckState(list,i,cid==AuditAll);
            }
            audit_rebuilding_filters=false;
            audit_filters_changed(window);
        }
        return true;
    case AuditOpen:if(notification==BN_CLICKED)audit_open(window);return true;
    case AuditCopyLink:if(notification==BN_CLICKED)audit_copy_message_link(window);return true;
    case AuditMessageSearch:
        if(notification==EN_CHANGE) {
            audit_message_query=wide(value(GetDlgItem(window,AuditMessageSearch)));
            audit_apply_filters(window,true);
        }
        return true;
    default:return false;
    }
}

void audit_notify(HWND window,NMHDR* info) {
    if(info->idFrom==AuditFilters) {
        if(info->code!=LVN_ITEMCHANGED||audit_rebuilding_filters)return;
        auto change=reinterpret_cast<const NMLISTVIEW*>(info);
        if(change->iItem<0||!((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK))return;
        auto action=static_cast<int>(change->lParam);
        bool checked=ListView_GetCheckState(info->hwndFrom,change->iItem)!=FALSE;
        if(checked==audit_default_checked)audit_action_overrides.erase(action);
        else audit_action_overrides.insert(action);
        audit_filters_changed(window);
        return;
    }
    if(info->code==LVN_GETDISPINFOW) {
        auto& item=reinterpret_cast<NMLVDISPINFOW*>(info)->item;
        if(item.iItem<0||!(item.mask&LVIF_TEXT)||item.cchTextMax<=0)return;
        auto row=audit_row(window,item.iItem);
        if(!row)return;
        std::string text;
        switch(item.iSubItem) {
        case 0:text=row->at("at").get<std::string>();break;
        case 1:text=audit_action_name(row->at("action_type").get<int>());break;
        case 2:text=row->at("actor").get<std::string>();break;
        case 3:text=row->value("author",std::string());break;
        case 4:text=audit_message_cell(*row);break;
        case 5:text=row->at("channel").get<std::string>();break;
        case 6:text=row->at("message").get<std::string>();break;
        }
        auto label=wide(text);
        lstrcpynW(item.pszText,label.c_str(),item.cchTextMax);
    } else if(info->code==LVN_ITEMCHANGED) {
        auto change=reinterpret_cast<const NMLISTVIEW*>(info);
        if(!audit_rebuilding_rows&&((change->uOldState^change->uNewState)&LVIS_SELECTED))audit_details(window);
    } else if(info->code==NM_DBLCLK) {
        auto index=reinterpret_cast<const NMITEMACTIVATE*>(info)->iItem;
        if(index>=0)audit_open(window,index);
    }
}

void audit_create(HWND window) {
    control(window,L"STATIC",L"Account",0,AuditAccountLabel);
    control(window,L"BUTTON",L"Refresh servers",WS_TABSTOP,AuditRefresh);
    control(window,L"STATIC",L"Server",0,AuditGuildLabel);
    for(int cid:{AuditAccount,AuditGuild}) {
        auto combo=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP|WS_VSCROLL,cid);
        SetWindowSubclass(combo,dark_control,2,0);
    }
    control(window,L"BUTTON",L"Scan audit log",WS_TABSTOP,AuditScan);
    control(window,L"BUTTON",L"Cancel",WS_TABSTOP,AuditCancel);
    control(window,L"STATIC",L"Connect a Discord account in View Servers.",0,AuditStatus);
    control(window,L"STATIC",L"Action filters",0,AuditFilterLabel);
    control(window,L"BUTTON",L"All",WS_TABSTOP,AuditAll);
    control(window,L"BUTTON",L"None",WS_TABSTOP,AuditNone);
    auto filters=control(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_NOCOLUMNHEADER|LVS_SINGLESEL|WS_BORDER|WS_TABSTOP,AuditFilters);
    ListView_SetExtendedListViewStyle(filters,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(filters,surface);
    ListView_SetTextBkColor(filters,surface);
    ListView_SetTextColor(filters,ink);
    LVCOLUMNW filter_column{};
    filter_column.mask=LVCF_WIDTH;
    filter_column.cx=260;
    ListView_InsertColumn(filters,0,&filter_column);
    auto list=control(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_OWNERDATA|LVS_SINGLESEL|WS_BORDER|WS_TABSTOP,AuditTable);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(list,surface);
    ListView_SetTextBkColor(list,surface);
    ListView_SetTextColor(list,ink);
    SetWindowTheme(ListView_GetHeader(list),L"",L"");
    SetWindowSubclass(ListView_GetHeader(list),dark_control,1,0);
    int i=0;
    for(auto title:{L"Date (UTC)",L"Action",L"Performed by",L"From user",L"Message text",L"Channel / thread ID",L"Message ID"}) {
        LVCOLUMNW col{};
        col.mask=LVCF_TEXT|LVCF_WIDTH;
        col.cx=150;
        col.pszText=const_cast<LPWSTR>(title);
        ListView_InsertColumn(list,i++,&col);
    }
    control(window,L"BUTTON",L"Open message",WS_TABSTOP,AuditOpen);
    control(window,L"BUTTON",L"Copy message link",WS_TABSTOP,AuditCopyLink);
    control(window,L"STATIC",L"0 of 0 entries shown",0,AuditCount);
    control(window,L"STATIC",L"Find message",0,AuditSearchLabel);
    auto search=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,AuditMessageSearch);
    SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Text, change, user or role"));
    control(window,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP,AuditDetails);
    control(window,L"STATIC",L"Message preview",0,AuditPreviewLabel);
    control(window,L"EDIT",L"Select an audit entry to preview it.",ES_MULTILINE|ES_READONLY|WS_VSCROLL|WS_TABSTOP,AuditPreview);
    audit_fill_filters(window);
}

void audit_show(HWND window,bool show) {
    for(int cid:audit_controls)ShowWindow(GetDlgItem(window,cid),show?SW_SHOW:SW_HIDE);
    audit_select_preview(window,show?audit_row(window):nullptr);
    if(show) {
        audit_queue_message_rows(window,0);
        audit_update_enabled(window);
        auto account=audit_account(window);
        if(!account.empty()&&account!=audit_loaded_account&&!busy)audit_begin(window,true);
    } else audit_pause_messages();
}

void audit_layout(HWND window,int w,int h,double scale) {
    auto px=[=](int size){return static_cast<int>(size*scale);};
    auto move=[&](int cid,int x,int y,int width,int height){
        MoveWindow(GetDlgItem(window,cid),px(x),px(y),px(width),px(height),TRUE);
    };
    move(AuditAccountLabel,24,59,84,24);
    move(AuditAccount,110,54,300,260);
    move(AuditRefresh,420,53,155,30);
    move(AuditGuildLabel,24,101,84,24);
    move(AuditGuild,110,96,w-370,300);
    move(AuditScan,w-248,95,132,30);
    move(AuditCancel,w-106,95,82,30);
    move(AuditStatus,24,138,w-48,24);
    const int filter_width=288;
    move(AuditFilterLabel,24,175,120,22);
    move(AuditAll,168,169,62,28);
    move(AuditNone,238,169,74,28);
    move(AuditFilters,24,204,filter_width,std::max(65,h-527));
    ListView_SetColumnWidth(GetDlgItem(window,AuditFilters),0,px(filter_width-22));
    const int results_width=w-348;
    move(AuditTable,324,169,results_width,std::max(100,h-492));
    move(AuditOpen,24,h-312,132,30);
    move(AuditCopyLink,164,h-312,154,30);
    move(AuditCount,330,h-308,220,24);
    move(AuditSearchLabel,555,h-308,96,24);
    move(AuditMessageSearch,656,h-312,w-680,30);
    move(AuditDetails,24,h-274,w-48,72);
    move(AuditPreviewLabel,24,h-194,w-48,20);
    move(AuditPreview,24,h-170,w-48,109);
    const int name_width=std::clamp((results_width-510)/2,105,155);
    const int message_width=std::max(245,results_width-279-2*name_width);
    int widths[]={150,125,name_width,name_width,message_width,157,157};
    for(int i=0;i<7;++i)ListView_SetColumnWidth(GetDlgItem(window,AuditTable),i,px(widths[i]));
}
