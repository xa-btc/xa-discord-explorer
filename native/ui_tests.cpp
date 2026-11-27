// Exercise the real window handlers with synthetic state in an isolated workspace.
// No session cache is loaded and no network work is started.
#include <windows.h>
#include <string>
namespace {
std::wstring copied_text;
bool clipboard_open=false;
int clipboard_writes=0;
HGLOBAL clipboard_transfer=nullptr;
BOOL WINAPI test_open_clipboard(HWND){clipboard_open=true;return TRUE;}
BOOL WINAPI test_empty_clipboard(){return clipboard_open?TRUE:FALSE;}
HANDLE WINAPI test_set_clipboard_data(UINT format,HANDLE data) {
    if(!clipboard_open||format!=CF_UNICODETEXT||!data)return nullptr;
    auto allocation=static_cast<HGLOBAL>(data);
    auto* text=static_cast<const wchar_t*>(GlobalLock(allocation));
    if(!text)return nullptr;
    copied_text=text;
    GlobalUnlock(allocation);
    ++clipboard_writes;
    clipboard_transfer=allocation;
    return data;
}
BOOL WINAPI test_close_clipboard(){
    if(clipboard_transfer)GlobalFree(clipboard_transfer); // The fake clipboard owns successful transfers.
    clipboard_transfer=nullptr;
    clipboard_open=false;
    return TRUE;
}
}
#define OpenClipboard test_open_clipboard
#define EmptyClipboard test_empty_clipboard
#define SetClipboardData test_set_clipboard_data
#define CloseClipboard test_close_clipboard
#define collect_server_counts test_collect_server_counts
#define collect_members test_collect_members
#define collect_chat test_collect_chat
#define audit_servers test_audit_servers
#define scan_audit_log test_scan_audit_log
#define audit_message_preview test_audit_message_preview
#include "app.cpp"
#undef CloseClipboard
#undef SetClipboardData
#undef EmptyClipboard
#undef OpenClipboard
#undef audit_message_preview
#undef scan_audit_log
#undef audit_servers
#undef collect_chat
#undef collect_members
#undef collect_server_counts
#include <iostream>

namespace {
std::vector<std::string> scanned_in_test;
bool wait_for_test_cancel=false;
std::atomic<int> live_test_cycles=0;
std::vector<std::vector<int>> audit_requests;
std::atomic<bool> hold_next_audit_page=false;
std::atomic<int> preview_requests=0;
std::atomic<bool> hold_preview=false;
std::string pin_message_id="98";
}
namespace sml {
Json test_audit_message_preview(const ChatGet&,const std::string& guild,const std::string& channel,const std::string& message) {
    ++preview_requests;
    if(guild!="900"||channel!="42")throw std::runtime_error("Preview requested a different server or channel");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(hold_preview&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if(hold_preview)throw std::runtime_error("UI blocked while changing the selected preview");
    if(message=="99")return {{"status","available"},{"author","Original writer"},{"author_id","8"},{"text","Unpinned message body"}};
    if(message=="98")return {{"status","available"},{"author","Pinned writer"},{"author_id","9"},{"text","Pinned message body\nOrchard notes"}};
    if(message=="100")return {{"status","available"},{"author","Other writer"},{"author_id","10"},{"text","Previous server preview body"}};
    throw std::runtime_error("Preview requested an unrelated message");
}
Json test_audit_servers(const ChatGet&,const std::string& account,const std::atomic<bool>&) {
    if(account=="2")return Json::array({{{"id","903"},{"name","Second account guild"}}});
    if(account!="1")throw std::runtime_error("Wrong audit account");
    return Json::array({{{"id","900"},{"name","Audit guild"}},{{"id","901"},{"name","Other guild"}}});
}
void test_scan_audit_log(const ChatGet&,const std::string& guild,const std::atomic<bool>& cancel,const AuditPage& publish,const std::vector<int>& actions) {
    if(guild!="900")throw std::runtime_error("Wrong audit server");
    audit_requests.push_back(actions);
    Json unpin={{"at","2026-11-21T12:00:00 UTC"},{"actor","Moderator"},{"channel","42"},{"message","99"},
        {"author","Original writer"},{"author_id","8"},{"action_type",75},{"link","https://discord.com/channels/900/42/99"},
        {"entry",{{"id","300"},{"action_type",75},{"user_id","7"},{"target_id","8"}}}};
    Json pin=unpin;
    pin["entry"]["id"]="299";
    pin["action_type"]=pin["entry"]["action_type"]=74;
    pin["message"]=pin_message_id;
    pin["author"]=pin_message_id=="98"?"Pinned writer":"Other writer";
    pin["author_id"]=pin["entry"]["target_id"]=pin_message_id=="98"?"9":"10";
    pin["link"]="https://discord.com/channels/900/42/"+pin_message_id;
    Json unknown={{"at","2026-11-21T11:00:00 UTC"},{"actor","Moderator"},{"channel","42"},{"message",""},
        {"author",""},{"author_id",""},{"action_type",999},{"link",""},{"entry",{{"id","298"},{"action_type",999},{"user_id","7"}}}};
    auto earlier_unpin=unpin;
    earlier_unpin["entry"]["id"]="297";
    Json role_update={{"at","2026-11-21T10:00:00 UTC"},{"actor","Moderator"},{"channel",""},{"message",""},
        {"author","Orchid member"},{"author_id","11"},{"action_type",25},{"link",""},
        {"change_summary","Added role: Archivist. Removed role: Visitor."},
        {"entry",{{"id","296"},{"action_type",25},{"user_id","7"},{"target_id","11"}}}};
    if(hold_next_audit_page.exchange(false)) {
        if(!actions.empty())throw std::runtime_error("Expected the held scan to request all activity");
        auto newest=unpin;
        newest["entry"]["id"]="301";
        publish(Json::array({std::move(newest)}));
        auto timeout=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!cancel&&std::chrono::steady_clock::now()<timeout)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if(!cancel)throw std::runtime_error("UI did not cancel the waiting audit scan");
        throw std::runtime_error("Audit request cancelled.");
    }
    Json page=Json::array();
    for(auto& row:{unpin,pin,unknown,earlier_unpin,role_update}) {
        int action=row.at("action_type").get<int>();
        if(actions.empty()||std::find(actions.begin(),actions.end(),action)!=actions.end())page.push_back(row);
    }
    if(!page.empty())publish(std::move(page));
}
Json test_collect_chat(ChatStore& store,const ChatGet&,const ChatDownload&,const Path&,const std::string&,const std::string& guild,const std::vector<std::string>& channels,const ChatOptions& options,const std::atomic<bool>&,const ChatProgress&){
    if(options.live_from.empty()||options.check_deletions||!options.author_id.empty()||!options.from_date.empty()||channels!=std::vector<std::string>{"950","951"})throw std::runtime_error("Live worker options or selected channels incorrect: "+Json(channels).dump());
    int cycle=++live_test_cycles;auto mid=std::to_string(40000+cycle);
    store.save_messages(guild,"950",Json::array({{{"id",mid},{"channel_id","950"},{"author",{{"id","3"},{"username","Live test"}}},{"content","Live arrival "+mid},{"timestamp",stamp(now())},{"attachments",Json::array()}}}),"live-test");
    return {{"results",Json::array({{{"channel_id","950"},{"status","complete"},{"messages_received",1}}})}};
}
int count_connections=0;
Json test_collect_server_counts(const std::string&,const std::vector<std::string>& servers,const std::atomic<bool>& cancel,const ProgressCallback&) {
    ++count_connections;if(cancel)throw std::runtime_error("Count scan cancelled");
    Json result=Json::array();for(auto& server:servers)result.push_back({{"server_id",server},{"server_name","Count test"},{"captured_at",stamp(now())},{"server_total",2000000},{"counts_only",true}});
    return result;
}
Json test_collect_members(const std::string&,const std::string& server,const std::atomic<bool>& cancel,const ProgressCallback& progress) {
    scanned_in_test.push_back(server);
    if(progress)progress({1,10,0,-1,"Mock member response"});
    if(wait_for_test_cancel){while(!cancel)std::this_thread::sleep_for(std::chrono::milliseconds(5));throw std::runtime_error("Collection cancelled");}
    if(server=="900")throw std::runtime_error("Selected account is not a member of this server");
    return {{"server_id",server},{"server_name","Batch test"},{"complete",false},{"captured_at",stamp(now())},
        {"members",Json::array({{{"user_id","3"},{"username","Owned account"}}})}};
}
}

int main() {
    HWND window{};
    try {
        workspace=std::filesystem::temp_directory_path()/(L"sml-ui-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(now()));
        db_path=workspace/L"members.sqlite3";inbox=workspace/L"inbox";
        database=std::make_unique<Database>(db_path);
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES|ICC_PROGRESS_CLASS|ICC_TAB_CLASSES};InitCommonControlsEx(&common);
        background=CreateSolidBrush(bg);surface_brush=CreateSolidBrush(surface);
        auto instance=GetModuleHandleW(nullptr);
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=L"SmlProgressTests";cls.hbrBackground=background;
        RegisterClassW(&cls);
        window=CreateWindowExW(0,cls.lpszClassName,L"Isolated progress test",WS_OVERLAPPEDWINDOW,0,0,1160,950,nullptr,nullptr,instance,nullptr);
        auto require=[](bool pass,const char* message){if(!pass)throw std::runtime_error(message);};
        require(window!=nullptr,"Could not create test window");
        require(!IsWindowEnabled(GetDlgItem(window,Cancel)),"Cancel must start disabled");
        busy=collecting=true;cancel_collection=false;collection_started=std::chrono::steady_clock::now()-std::chrono::seconds(75);
        collection_progress={25,100,1,4,"Receiving members"};
        EnableWindow(GetDlgItem(window,Cancel),TRUE);
        SendMessageW(window,WM_TIMER,2,0);
        auto label=value(GetDlgItem(window,ProgressText));
        require(label.find("25 / 100 members")!=std::string::npos,"Progress count not displayed");
        require(label.find("1:15")!=std::string::npos,"Elapsed time not displayed");
        require(SendMessageW(GetDlgItem(window,Progress),PBM_GETPOS,0,0)==250,"Known-total progress bar incorrect");
        require(!cancel_collection&&collecting,"Collection expired after a minute");
        collection_progress.total=-1;SendMessageW(window,WM_TIMER,2,0);
        require((GetWindowLongPtrW(GetDlgItem(window,Progress),GWL_STYLE)&PBS_MARQUEE)!=0,"Unknown total needs marquee");
        collection_progress.total=100;SendMessageW(window,WM_COMMAND,Cancel,0);
        require(cancel_collection&&!IsWindowEnabled(GetDlgItem(window,Cancel)),"Cancel action did not stop collection");
        // Even if a complete worker result was queued before Cancel, cancellation wins.
        database->apply(Json{{"server_id","900"},{"server_name","UI test"},{"complete",true},{"captured_at",stamp(now()-1000000)},
            {"members",Json::array({{{"user_id","2"},{"username","Previously known"}}})}});
        collected={{"server_id","900"},{"server_name","UI test"},{"complete",true},{"captured_at",stamp(now())},
            {"members",Json::array({{{"user_id","1"},{"username","Owned account"}}})}};
        collection_error.clear();SendMessageW(window,CollectionDone,0,0);
        require(!busy&&!collecting,"Completion did not reset controls");
        require(database->servers()[0]["member_count"]==2&&database->servers()[0]["coverage"]=="Partial cache","Cancelled partial did not merge safely");
        auto partial=read_json(workspace/L"reports"/L"member-list-900-partial.json");
        require(partial["complete"]==false&&partial["cancelled"]==true,"Cancelled result not exported as partial");
        require(value(GetDlgItem(window,ProgressText)).find("Cancelled")!=std::string::npos,"Cancellation outcome missing");
        // Checkboxes persist across search and collapse duplicate server IDs.
        database->apply(Json{{"server_id","901"},{"server_name","Second server"},{"complete",false},{"captured_at",stamp(now())},{"members",Json::array()}});
        database->apply_account(Json{{"user_id","1"},{"username","Owned account"},{"captured_at",stamp(now())},{"guilds",Json::array({{{"server_id","900"},{"server_name","UI test"}}})}});
        update_table();set(GetDlgItem(window,Search),"900");SendMessageW(window,WM_COMMAND,SelectAll,0);
        require(checked_servers.size()==1&&checked_servers.contains("900"),"Filtered select all did not deduplicate servers");
        set(GetDlgItem(window,Search),"901");require(checked_servers.contains("900"),"Search lost hidden selection");
        SendMessageW(window,WM_COMMAND,SelectAll,0);require(checked_servers.size()==2,"Select all did not retain hidden checks");
        SendMessageW(window,WM_COMMAND,ClearAll,0);require(checked_servers.empty(),"Clear all left hidden servers checked");
        set(GetDlgItem(window,Search),"");SendMessageW(window,WM_COMMAND,SelectAll,0);
        require(checked_servers.size()==2,"Duplicate coverage rows caused duplicate scans");
        // Real queue/completion handlers with a fake collector; no authentication or network.
        sessions={{"1",{{"username","Owned account"},{"token","fake-ui-test-token"}}}};update_accounts(window);
        auto drain=[&]{
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(busy&&std::chrono::steady_clock::now()<deadline){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            require(!busy,"Batch did not finish");
        };
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),4);switch_view(window);
        require(TabCtrl_GetItemCount(GetDlgItem(window,Tabs))==5,"Audit Log tab missing");
        require((GetWindowLongPtrW(GetDlgItem(window,AuditTable),GWL_STYLE)&WS_VISIBLE)!=0&&
            (GetWindowLongPtrW(table,GWL_STYLE)&WS_VISIBLE)==0,"Audit tab visibility incorrect");
        require(!IsWindowEnabled(GetDlgItem(window,AuditScan)),"Audit scan enabled without a server");
        drain();
        auto guild=GetDlgItem(window,AuditGuild);
        require(audit_account(window)=="1"&&SendMessageW(guild,CB_GETCOUNT,0,0)==2&&
            !IsWindowEnabled(GetDlgItem(window,AuditScan)),"Audit connection failed or selected a server implicitly");
        SendMessageW(guild,CB_SETCURSEL,0,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(AuditGuild,CBN_SELCHANGE),0);
        require(IsWindowEnabled(GetDlgItem(window,AuditScan)),"Selecting a server did not enable audit scanning");
        hold_preview=true;
        SendMessageW(window,WM_COMMAND,AuditScan,0);drain();
        auto audit_list=GetDlgItem(window,AuditTable);
        require(ListView_GetItemCount(audit_list)==5&&audit_requests==std::vector<std::vector<int>>{{}},
            "All actions did not use one unfiltered scan");
        auto filters=GetDlgItem(window,AuditFilters);
        auto set_audit_action=[&](int action,bool checked) {
            for(int i=0;i<ListView_GetItemCount(filters);++i) {
                LVITEMW item{};item.mask=LVIF_PARAM;item.iItem=i;
                if(ListView_GetItem(filters,&item)&&item.lParam==action) {
                    ListView_SetCheckState(filters,i,checked);
                    return;
                }
            }
            throw std::runtime_error("Missing audit action checkbox "+std::to_string(action));
        };
        auto pump_audit_until=[&](auto ready,const char* failure) {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
            do {
                MSG msg{};
                while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                if(ready())return;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while(std::chrono::steady_clock::now()<deadline);
            throw std::runtime_error(failure);
        };
        auto first_audit_id=[&] {
            const Json* row=audit_row(window,0);
            return row?row->at("entry").at("id").get<std::string>():std::string();
        };
        auto search_messages=GetDlgItem(window,AuditMessageSearch);
        pump_audit_until([&]{return preview_requests>0;},"Reading pin messages did not start in the background");
        set(search_messages,"aRcHiViSt");
        require(ListView_GetItemCount(audit_list)==1&&first_audit_id()=="296","Role change text was not searchable while pins loaded");
        set(search_messages,"oRcHiD mEmBeR");
        require(ListView_GetItemCount(audit_list)==1&&first_audit_id()=="296","From user search did not find the affected member");
        set(search_messages,"oRcHaRd nOtEs");
        hold_preview=false;
        pump_audit_until([&]{return audit_messages_idle()&&ListView_GetItemCount(audit_list)==1&&first_audit_id()=="299";},
            "Message text arriving after a search did not reveal the matching audit row");
        require(preview_requests==2&&audit_requests.size()==1,"Message hydration fetched duplicate targets, non-pin rows, or audit history again");
        set(search_messages,"pInNeD WrItEr");
        require(ListView_GetItemCount(audit_list)==1&&first_audit_id()=="299","From user search did not find the original sender");
        set(search_messages,"");
        require(ListView_GetItemCount(audit_list)==5,"Clearing message search did not restore every audit entry");
        for(int i=0;i<ListView_GetItemCount(filters);++i)
            require(ListView_GetCheckState(filters,i),"Filter did not default to checked");
        set_audit_action(75,false);
        auto idle_test_deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(650);
        pump_audit_until([&]{return !busy&&std::chrono::steady_clock::now()>=idle_test_deadline;},
            "Cached filter did not stay idle");
        require(ListView_GetItemCount(audit_list)==3&&first_audit_id()=="299"&&
            audit_requests.size()==1,"Unchecking unpins did not filter the completed scan locally");
        SendMessageW(window,WM_COMMAND,AuditNone,0);
        require(ListView_GetItemCount(audit_list)==0&&!IsWindowEnabled(GetDlgItem(window,AuditScan)),
            "None did not hide all entries and disable scanning");
        SendMessageW(window,WM_COMMAND,AuditScan,0);
        require(!busy&&audit_requests.size()==1,"Empty action selection started a network scan");
        SendMessageW(window,WM_COMMAND,AuditAll,0);
        require(ListView_GetItemCount(audit_list)==5&&audit_requests.size()==1,"All did not restore the cached entries");
        auto audit_cell=[&](int row,int column) {
            wchar_t label[128]{};
            NMLVDISPINFOW display{};
            display.hdr={audit_list,AuditTable,LVN_GETDISPINFOW};
            display.item.mask=LVIF_TEXT;
            display.item.iItem=row;
            display.item.iSubItem=column;
            display.item.pszText=label;
            display.item.cchTextMax=static_cast<int>(std::size(label));
            SendMessageW(window,WM_NOTIFY,AuditTable,reinterpret_cast<LPARAM>(&display));
            return std::wstring(label);
        };
        require(audit_cell(0,1)==L"Message unpinned","Audit action label was not rendered");
        require(audit_cell(0,2)==L"Moderator"&&audit_cell(0,3).find(L"Original writer")!=std::wstring::npos&&
            audit_cell(1,3).find(L"Pinned writer")!=std::wstring::npos,"Audit rows did not distinguish message authors from the actor");
        require(audit_cell(0,4)==L"Unpinned message body"&&audit_cell(1,4).find(L"Orchard notes")!=std::wstring::npos&&
            audit_cell(1,4).find_first_of(L"\r\n")==std::wstring::npos&&audit_cell(3,4)==audit_cell(0,4),
            "Message text column did not populate every matching row before selection");
        require(audit_cell(0,5)==L"42"&&audit_cell(0,6)==L"99","Adding the message text column displaced the channel or message ID");
        require(audit_cell(4,2)==L"Moderator"&&audit_cell(4,3).find(L"Orchid member")!=std::wstring::npos&&
            audit_cell(4,4)==L"Added role: Archivist. Removed role: Visitor.","Role update row lost its actor, affected member or change summary");
        auto preview_text=[&]{return value(GetDlgItem(window,AuditPreview));};
        ListView_SetItemState(audit_list,0,LVIS_SELECTED,LVIS_SELECTED);
        require(IsWindowEnabled(GetDlgItem(window,AuditOpen))&&
            value(GetDlgItem(window,AuditDetails)).find("https://discord.com/channels/900/42/99")!=std::string::npos,
            "Selected audit row did not expose its message link");
        require(IsWindowEnabled(GetDlgItem(window,AuditCopyLink)),"Copy message link was disabled for a selected unpin");
        SendMessageW(window,WM_COMMAND,AuditCopyLink,0);
        require(clipboard_writes==1&&!clipboard_open&&copied_text==L"https://discord.com/channels/900/42/99","Copy did not write the unpinned message URL as Unicode");
        ListView_SetItemState(audit_list,1,LVIS_SELECTED,LVIS_SELECTED);
        pump_audit_until([&]{return preview_text().find("Pinned message body\r\nOrchard notes")!=std::string::npos;},
            "Selected pin's bottom preview did not preserve the multiline message");
        SendMessageW(window,WM_COMMAND,AuditCopyLink,0);
        require(clipboard_writes==2&&copied_text==L"https://discord.com/channels/900/42/98","Copy used the previous row's message URL");
        require(preview_text().find("Unpinned message body")==std::string::npos&&
            value(GetDlgItem(window,AuditDetails)).find("https://discord.com/channels/900/42/98")!=std::string::npos,
            "Stale unpin preview or details leaked into the selected pin");
        ListView_SetItemState(audit_list,0,LVIS_SELECTED,LVIS_SELECTED);
        pump_audit_until([&]{return preview_text().find("Unpinned message body")!=std::string::npos;},
            "Selecting the unpin did not show its original message text");
        int previous_requests=preview_requests;
        ListView_SetItemState(audit_list,4,LVIS_SELECTED,LVIS_SELECTED);
        require(preview_text()=="Added role: Archivist. Removed role: Visitor."&&
            !IsWindowEnabled(GetDlgItem(window,AuditOpen))&&!IsWindowEnabled(GetDlgItem(window,AuditCopyLink))&&
            preview_requests==previous_requests,"Role update preview lost its summary or treated the entry as a linked message");
        ListView_SetItemState(audit_list,2,LVIS_SELECTED,LVIS_SELECTED);
        pump_audit_until([&]{return audit_messages_idle();},"Preview worker remained active for an unrelated audit action");
        SendMessageW(window,WM_COMMAND,AuditCopyLink,0);
        require(preview_text().find("Unpinned message body")==std::string::npos&&preview_requests==previous_requests&&
            !IsWindowEnabled(GetDlgItem(window,AuditCopyLink))&&clipboard_writes==2,
            "An unrelated audit action fetched or copied a message or displayed a stale preview");
        hold_next_audit_page=true;
        SendMessageW(window,WM_COMMAND,AuditScan,0);
        pump_audit_until([&]{return audit_running&&ListView_GetItemCount(audit_list)==1&&first_audit_id()=="301";},
            "First audit page did not reach the list while the scan was running");
        ListView_SetItemState(audit_list,0,LVIS_SELECTED,LVIS_SELECTED);
        require(audit_cell(0,4)==L"Unpinned message body"&&preview_text().find("Unpinned message body")!=std::string::npos&&
            preview_requests==previous_requests,"A repeated message lost its preview or was fetched again during a scan");
        SendMessageW(window,WM_COMMAND,AuditCancel,0);drain();
        auto partial_status=wide(value(GetDlgItem(window,AuditStatus)));
        for(auto& c:partial_status)c=static_cast<wchar_t>(std::towlower(c));
        require(ListView_GetItemCount(audit_list)==1&&audit_row(window,0)->at("entry").at("id")=="301"&&
            partial_status.find(L"cancel")!=std::wstring::npos&&partial_status.find(L"partial")!=std::wstring::npos,
            "Cancelling after the first page lost its entries or failed to report a partial scan");
        // Begin another unfiltered run, then reproduce None -> pins + unpins while its second page is pending.
        hold_next_audit_page=true;
        SendMessageW(window,WM_COMMAND,AuditScan,0);
        pump_audit_until([&]{return audit_running&&!hold_next_audit_page.load()&&audit_pages_received&&first_audit_id()=="301";},
            "Second broad scan did not begin");
        SendMessageW(window,WM_COMMAND,AuditNone,0);
        require(ListView_GetItemCount(audit_list)==0,"None did not hide displayed rows during a scan");
        set_audit_action(74,true);
        set_audit_action(75,true);
        pump_audit_until([&]{return !busy&&audit_requests.size()>=4;},
            "Checkbox changes did not supersede the active broad scan");
        const std::vector<int> pin_actions={74,75};
        require(audit_requests.size()==4&&audit_requests.back()==pin_actions,
            "Checking pins and unpins did not debounce into exactly one targeted scan");
        require(ListView_GetItemCount(audit_list)==3&&first_audit_id()=="300"&&audit_row(window,1)->at("entry").at("id")=="299",
            "Retargeted scan mixed partial and new rows or lost a selected action");
        SendMessageW(window,WM_COMMAND,AuditScan,0);drain();
        require(audit_requests.size()==5&&audit_requests.back()==pin_actions&&ListView_GetItemCount(audit_list)==3,
            "Manual refresh fetched actions outside the selected set or duplicated results");
        SendMessageW(window,WM_COMMAND,AuditNone,0);
        set_audit_action(10,true);
        pump_audit_until([&]{return !busy&&audit_requests.size()>=6;},"Unchecked coverage was not fetched on filter change");
        require(audit_requests.back()==std::vector<int>{10}&&ListView_GetItemCount(audit_list)==0,
            "Empty action result retained unrelated visible entries");
        pin_message_id="100";
        previous_requests=preview_requests;
        hold_preview=true;
        SendMessageW(window,WM_COMMAND,AuditAll,0);
        pump_audit_until([&]{return !busy&&audit_requests.size()>=7;},"All did not fetch missing audit coverage");
        require(audit_requests.back().empty()&&ListView_GetItemCount(audit_list)==5,
            "All fetched the wrong action set or left incomplete results");
        pump_audit_until([&]{return preview_requests>previous_requests;},"New pinned message body did not start loading in the background");
        ListView_SetItemState(audit_list,1,LVIS_SELECTED,LVIS_SELECTED);
        ListView_SetItemState(audit_list,0,LVIS_SELECTED,LVIS_SELECTED);
        require(preview_text().find("Unpinned message body")!=std::string::npos,"A slow background request blocked a cached preview");
        SendMessageW(guild,CB_SETCURSEL,1,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(AuditGuild,CBN_SELCHANGE),0);
        hold_preview=false;
        pump_audit_until([&]{return audit_messages_idle();},"A stale preview did not finish after switching servers");
        require(ListView_GetItemCount(audit_list)==0&&!IsWindowEnabled(GetDlgItem(window,AuditOpen))&&
            preview_text().find("Previous server preview body")==std::string::npos,
            "Changing servers retained an unrelated audit result");
        sessions["2"]={{"username","Second account"},{"token","fake-second-ui-test-token"}};update_accounts(window);
        SendMessageW(GetDlgItem(window,AuditAccount),CB_SETCURSEL,1,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(AuditAccount,CBN_SELCHANGE),0);drain();
        require(audit_account(window)=="2"&&audit_guilds.size()==1&&audit_guilds[0]["id"]=="903"&&
            !IsWindowEnabled(GetDlgItem(window,AuditScan)),"Audit server list was not scoped to the selected account");
        sessions.erase("2");update_accounts(window);
        require(audit_account(window)=="1"&&audit_guilds.empty(),"Forgotten account retained its audit server scope");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),0);switch_view(window);
        begin_batch(window);drain();
        require(scanned_in_test==std::vector<std::string>({"900","901"}),"Batch order, deduplication or error continuation failed");
        require(batch_results.size()==2&&batch_results[0]["status"]=="failed"&&batch_results[1]["status"]=="partial","Batch summary lost outcomes");
        require(read_json(workspace/L"reports"/L"member-list-901-partial.json")["members"].size()==1,"Batch result was not exported");
        scanned_in_test.clear();wait_for_test_cancel=true;begin_batch(window);cancel_active_collection(window);drain();
        require(scanned_in_test.size()<=1&&batch_index==1&&!batch_active,"Cancellation started another queued server");

        auto before_count=database->member_servers("3");auto before_scans=scanned_in_test;
        checked_servers={"900","901"};auto connections_before=sml::count_connections;SendMessageW(window,WM_COMMAND,ScanCounts,0);drain();
        require(sml::count_connections==connections_before+1,"Count batch used multiple connections");
        require(database->member_servers("3")==before_count&&scanned_in_test==before_scans,"Count scan changed roster or called member collector");
        bool count_saved=false;for(auto& row:database->servers())if(row["server_id"]=="901")count_saved=row["server_total"]==2000000;
        require(count_saved&&batch_results.size()==2,"Count scan not saved");
        auto before_cancel=database->servers();SendMessageW(window,WM_COMMAND,ScanCounts,0);cancel_active_collection(window);drain();
        require(database->servers()==before_cancel,"Cancelled count scan changed saved data");
        SendMessageW(GetDlgItem(window,ScanAge),CB_SETCURSEL,3,0);SendMessageW(GetDlgItem(window,RemoveLeaves),CB_SETCURSEL,1,0);
        SendMessageW(window,WM_COMMAND,MAKEWPARAM(ScanAge,CBN_SELCHANGE),0);
        require(scan_days==7&&remove_server_leaves,"Scan options not selected");scan_days=0;remove_server_leaves=false;load_scan_options();
        require(scan_days==7&&remove_server_leaves,"Scan options not restored");
        checked_servers={"901"};begin_batch(window,true);require(!busy&&batch_skipped==1,"Recent count was scanned");
        begin_batch(window,false);require(!busy&&batch_skipped==1,"Recent roster was scanned");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),1);switch_view(window);
        require(viewing_members&&member_rows.size()>0,"Member tab did not load cached users");
        require((GetWindowLongPtrW(table,GWL_STYLE)&WS_VISIBLE)==0&&(GetWindowLongPtrW(member_table,GWL_STYLE)&WS_VISIBLE)!=0,"Tab visibility incorrect");
        set(GetDlgItem(window,MemberSearch),"3");require(member_rows.size()==1&&member_rows[0]["user_id"]=="3","Member search failed");
        ListView_SetItemState(member_table,0,LVIS_SELECTED,LVIS_SELECTED);require(value(GetDlgItem(window,MemberDetails)).find("901")!=std::string::npos,"Member membership detail missing");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),0);switch_view(window);
        NMLISTVIEW click{};click.hdr.hwndFrom=table;click.hdr.code=LVN_COLUMNCLICK;click.iSubItem=1;
        SendMessageW(window,WM_NOTIFY,Table,reinterpret_cast<LPARAM>(&click));require(server_sort==1&&!server_desc&&server_id_at(0)=="900","Server ID sorting failed");
        SendMessageW(window,WM_NOTIFY,Table,reinterpret_cast<LPARAM>(&click));require(server_desc&&server_id_at(0)=="901","Reverse server ID sorting failed");
        chat_db->channels("901",Json::array({{{"id","950"},{"name","chat"},{"type",0}}}));
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),2);switch_view(window);
        for(size_t i=0;i<chat_server_rows.size();++i)if(chat_server_rows[i]["server_id"]=="901")SendMessageW(GetDlgItem(window,ChatGuild),CB_SETCURSEL,i,0);
        chat_load_channels(window);require(ListView_GetItemCount(GetDlgItem(window,ChatChannels))==1,"Channel tab missing saved channel");
        require((GetWindowLongPtrW(GetDlgItem(window,ChatChannels),GWL_STYLE)&WS_VISIBLE)!=0&&(GetWindowLongPtrW(table,GWL_STYLE)&WS_VISIBLE)==0,"Channel tab visibility wrong");
        SendMessageW(window,WM_COMMAND,ChatAll,0);require(ListView_GetCheckState(GetDlgItem(window,ChatChannels),0),"Select all channels failed");
        SendMessageW(window,WM_COMMAND,ChatClear,0);require(!ListView_GetCheckState(GetDlgItem(window,ChatChannels),0),"Clear channels failed");
        chat_db->channels("901",Json::array({{{"id","950"},{"name","chat"},{"type",0}},{{"id","951"},{"name","General"},{"type",0}}}));
        chat_load_channels(window);set(GetDlgItem(window,ChatChannelSearch),"CHAT");require(chat_channels.size()==1&&chat_channels[0]["id"]=="950","Channel name search failed");
        SendMessageW(window,WM_COMMAND,ChatAll,0);set(GetDlgItem(window,ChatChannelSearch),"951");require(chat_channels.size()==1&&chat_channels[0]["id"]=="951","Channel ID search failed");
        require(chat_collection_channels(window)==std::vector<std::string>{"950"},"Hidden checked channel excluded from collection");
        SendMessageW(window,WM_COMMAND,ChatAll,0);set(GetDlgItem(window,ChatChannelSearch),"no-match");require(chat_channels.empty()&&chat_collection_channels(window).size()==2,"Search lost hidden checks");
        SendMessageW(window,WM_COMMAND,ChatClear,0);require(chat_collection_channels(window).empty(),"Clear all retained hidden checks");
        set(GetDlgItem(window,ChatChannelSearch),"");require(chat_channels.size()==2,"Clearing channel search failed");
        SendMessageW(window,WM_COMMAND,ChatAll,0);chat_selection_scope="different-server";chat_load_channels(window);require(chat_collection_channels(window).empty(),"Channel checks leaked between scopes");
        // Double-click a filtered result to return to its channel's full conversation page.
        auto saved_message=[](int mid,const std::string& channel,const std::string& text){return Json{{"id",std::to_string(mid)},{"channel_id",channel},{"author",{{"id","3"},{"username","Context author"}}},{"content",text},{"timestamp","2026-01-01T12:00:00+00:00"},{"attachments",Json::array()}};};
        Json conversation=Json::array();
        for(int i=0;i<1000;++i){int mid=10000+i;auto row=saved_message(mid,"950",mid==10349||mid==10899?"Could we add a plugin option? contextneedle "+std::to_string(mid):"Context message "+std::to_string(mid));if(mid==10350)row["author"]["id"]="4";conversation.push_back(row);}
        chat_db->save_messages("901","950",conversation,"context-test");
        auto unrelated=saved_message(12000,"951","Unrelated conversation mentioning user 3");unrelated["author"]["id"]="13";
        chat_db->save_messages("901","951",Json::array({unrelated}),"context-test");
        // User-ID collection controls also filter the saved view by exact author ID.
        chat_check(window,ChatChannelOnly,false);chat_check(window,ChatSuggestions,false);chat_check(window,ChatDeleted,false);set(GetDlgItem(window,ChatSearch),"");
        auto from_before_author=value(GetDlgItem(window,ChatFrom));bool since_before_author=checked(window,ChatSince);
        set(GetDlgItem(window,ChatAuthor),"3");
        require(chat_total==999&&!IsWindowEnabled(GetDlgItem(window,ChatFrom))&&!IsWindowEnabled(GetDlgItem(window,ChatSince)),"Author filter included another user or left date/incremental limits enabled");
        chat_check(window,ChatDelete,true);chat_command(window,ChatDelete,BN_CLICKED);
        require(IsWindowEnabled(GetDlgItem(window,ChatFrom))&&!IsWindowEnabled(GetDlgItem(window,ChatSince)),"Author deletion did not enable its required date control");
        chat_check(window,ChatDelete,false);chat_command(window,ChatDelete,BN_CLICKED);
        require(!IsWindowEnabled(GetDlgItem(window,ChatFrom)),"Disabling author deletion did not restore all-history mode");
        set(GetDlgItem(window,ChatAuthor),"13");require(chat_total==1&&chat_messages[0]["id"]=="12000","Author filtering did not use an exact user ID");
        chat_export(window);auto author_export=read_json(workspace/L"reports"/L"messages-901-user-13.json");
        require(author_export.size()==1&&author_export[0]["message"]["author"]["id"]=="13"&&author_export[0]["message"]["id"]=="12000","Author export did not retain only the exact user's messages");
        set(GetDlgItem(window,ChatAuthor),"<@!3>");require(chat_total==999,"Author mention was not normalized");
        set(GetDlgItem(window,ChatAuthor),"invalid user");require(chat_total==0&&chat_messages.empty(),"Invalid author input did not produce an empty saved view");
        set(GetDlgItem(window,ChatAuthor),"3");chat_save_options(window);require(read_json(workspace/L"chat-options.json")["author_id"]=="3","User ID option was not saved");
        busy=true;chat_update_enabled(window);require(!IsWindowEnabled(GetDlgItem(window,ChatAuthor)),"Author input remained editable during collection");busy=false;chat_update_enabled(window);
        set(GetDlgItem(window,ChatAuthor),"");
        require(chat_total==1001&&IsWindowEnabled(GetDlgItem(window,ChatFrom))&&IsWindowEnabled(GetDlgItem(window,ChatSince)),"Clearing the author did not restore all messages and normal date controls");
        require(value(GetDlgItem(window,ChatFrom))==from_before_author&&checked(window,ChatSince)==since_before_author,"Author mode overwrote normal collection settings");
        chat_db->deleted("10349");chat_db->deleted("10899");
        for(size_t i=0;i<chat_channels.size();++i)ListView_SetCheckState(GetDlgItem(window,ChatChannels),static_cast<int>(i),chat_channels[i]["id"]=="951");
        auto collection_before_context=chat_collection_channels(window);require(collection_before_context==std::vector<std::string>{"951"},"Context test collection fixture failed");
        chat_check(window,ChatChannelOnly,false);chat_check(window,ChatSuggestions,true);chat_check(window,ChatDeleted,true);
        set(GetDlgItem(window,ChatChannelSearch),"General");set(GetDlgItem(window,ChatSearch),"contextneedle");set(GetDlgItem(window,ChatAuthor),"3");
        require(chat_messages.size()==2&&chat_messages[0]["id"]=="10899"&&chat_messages[1]["id"]=="10349","Context search fixture order failed");
        auto double_click_message=[&](int row){NMITEMACTIVATE click_message{};click_message.hdr.hwndFrom=GetDlgItem(window,ChatMessages);click_message.hdr.idFrom=ChatMessages;click_message.hdr.code=NM_DBLCLK;click_message.iItem=row;SendMessageW(window,WM_NOTIFY,ChatMessages,reinterpret_cast<LPARAM>(&click_message));};
        ListView_SetItemState(GetDlgItem(window,ChatMessages),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        double_click_message(-1);double_click_message(20);
        require(value(GetDlgItem(window,ChatSearch))=="contextneedle"&&chat_messages.size()==2&&chat_offset==0,"Double-click outside a message changed search results");
        // The activated row must win over the previously selected row.
        double_click_message(1);
        require(value(GetDlgItem(window,ChatSearch)).empty()&&value(GetDlgItem(window,ChatAuthor)).empty()&&!checked(window,ChatSuggestions)&&!checked(window,ChatDeleted),"Context jump did not clear message and author filters");
        require(checked(window,ChatChannelOnly)&&chat_selected_channel(window)=="950"&&value(GetDlgItem(window,ChatChannelSearch)).empty(),"Context jump did not reveal and select the result's channel");
        require(chat_total==1000&&chat_offset==600&&value(GetDlgItem(window,ChatPage)).find("Page 7 / 10")!=std::string::npos,"Context jump did not land on page seven of ten");
        require(chat_messages.size()==100&&chat_messages[49]["id"]=="10350"&&chat_messages[50]["id"]=="10349"&&chat_messages[51]["id"]=="10348","Context jump did not restore surrounding messages");
        require(chat_selected_message(window)=="10349"&&(ListView_GetItemState(GetDlgItem(window,ChatMessages),50,LVIS_FOCUSED)&LVIS_FOCUSED)!=0&&value(GetDlgItem(window,ChatDetail)).find("contextneedle 10349")!=std::string::npos,"Context jump did not focus and display the clicked message");
        require(chat_collection_channels(window)==collection_before_context,"Context jump changed channels checked for collection");
        SendMessageW(window,WM_COMMAND,ChatNext,0);require(chat_offset==700&&chat_messages[0]["id"]=="10299","Context next-page navigation failed");
        SendMessageW(window,WM_COMMAND,ChatPrev,0);require(chat_offset==600&&chat_messages[50]["id"]=="10349","Context previous-page navigation failed");
        // Search can find saved thread messages even while the channel list hides threads.
        chat_db->channels("901",Json::array({{{"id","952"},{"name","Saved thread"},{"type",11},{"parent_id","950"}}}));
        chat_db->save_messages("901","952",Json::array({saved_message(13000,"952","threadneedle context")}),"context-test");
        chat_check(window,ChatChannelOnly,false);chat_check(window,ChatThreads,false);set(GetDlgItem(window,ChatChannelSearch),"General");set(GetDlgItem(window,ChatSearch),"threadneedle");
        require(chat_messages.size()==1&&chat_messages[0]["id"]=="13000","Hidden thread search fixture failed");
        double_click_message(0);
        require(checked(window,ChatThreads)&&checked(window,ChatChannelOnly)&&chat_selected_channel(window)=="952"&&value(GetDlgItem(window,ChatChannelSearch)).empty(),"Context jump did not reveal a hidden thread");
        require(chat_offset==0&&chat_total==1&&chat_selected_message(window)=="13000"&&value(GetDlgItem(window,ChatSearch)).empty(),"Thread context jump did not select its message");
        require(chat_collection_channels(window)==collection_before_context,"Thread context jump changed collection checkboxes");
        // DM controls share browsing behavior while isolating accounts and server options.
        wchar_t dm_label[32]{};
        TCITEMW dm_tab{};dm_tab.mask=TCIF_TEXT;dm_tab.pszText=dm_label;dm_tab.cchTextMax=static_cast<int>(std::size(dm_label));
        require(TabCtrl_GetItem(GetDlgItem(window,Tabs),3,&dm_tab)&&std::wstring(dm_label)==L"View DMs","View DMs tab missing");
        set(GetDlgItem(window,ChatFrom),"2026-01-01");set(GetDlgItem(window,ChatAuthor),"13");chat_save_options(window);
        auto server_chat_options=read_json(workspace/L"chat-options.json");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),3);switch_view(window);
        require(chat_dm&&chat_guild(window)=="@me"&&chat_account(window)=="1","DM view did not select the account's private-message scope");
        require((GetWindowLongPtrW(GetDlgItem(window,ChatGuild),GWL_STYLE)&WS_VISIBLE)==0&&(GetWindowLongPtrW(GetDlgItem(window,ChatThreads),GWL_STYLE)&WS_VISIBLE)==0,"DM view retained server/thread controls");
        require(value(GetDlgItem(window,ChatAuthor)).empty()&&IsWindowEnabled(GetDlgItem(window,ChatFrom)),"Server author filter leaked into DM view or disabled its date");
        require(chat_channels.empty()&&chat_messages.empty(),"Server channel/messages leaked into the DM view");
        set(GetDlgItem(window,ChatFrom),"");set(GetDlgItem(window,ChatAuthor),"3");
        require(IsWindowEnabled(GetDlgItem(window,ChatFrom))&&!IsWindowEnabled(GetDlgItem(window,ChatSince)),"DM author search must retain its date control without using full-history checkpoints");
        chat_save_options(window);
        require(read_json(workspace/L"chat-dm-options.json")["from_date"]==""&&read_json(workspace/L"chat-options.json")==server_chat_options,"Saving DM options changed server options or rejected a blank date");
        set(GetDlgItem(window,ChatAuthor),"");chat_check(window,ChatChannelOnly,false);chat_check(window,ChatSuggestions,false);chat_check(window,ChatDeleted,false);set(GetDlgItem(window,ChatSearch),"");set(GetDlgItem(window,ChatChannelSearch),"");
        chat_db->channels("@me",Json::array({{{"id","960"},{"name","Private friend"},{"type",1}},{{"id","961"},{"name","Group friends"},{"type",3}}}));
        Json direct_conversation=Json::array();for(int i=0;i<205;++i)direct_conversation.push_back(saved_message(21000+i,"960",i==100?"dmneedle private context":"DM surroundings"));
        chat_db->save_messages("@me","960",direct_conversation,"dm-ui");chat_load_channels(window);chat_load_messages(window);
        require(chat_channels.size()==2&&chat_total==205,"DM conversation list or saved-message browsing failed");
        set(GetDlgItem(window,ChatChannelSearch),"Group");SendMessageW(window,WM_COMMAND,ChatAll,0);
        set(GetDlgItem(window,ChatChannelSearch),"Private");require(chat_collection_channels(window)==std::vector<std::string>{"961"},"DM search lost a hidden checked group conversation");
        set(GetDlgItem(window,ChatSearch),"dmneedle");require(chat_messages.size()==1,"DM saved-message search failed");double_click_message(0);
        require(chat_selected_channel(window)=="960"&&chat_selected_message(window)=="21100"&&chat_offset==100&&chat_total==205&&value(GetDlgItem(window,ChatSearch)).empty(),"DM context navigation did not restore the correct conversation page");
        require(value(GetDlgItem(window,ChatDetail)).find("https://discord.com/channels/@me/960/21100")!=std::string::npos,"DM detail used a server message link");
        chat_export(window);auto dm_export=read_json(workspace/L"reports"/L"messages-dm-1.json");
        require(dm_export.size()==205&&dm_export[0]["guild"]=="@me","DM export mixed server data or used the wrong account filename");
        std::ifstream dm_text_file(workspace/L"reports"/L"messages-dm-1.txt");std::string dm_text((std::istreambuf_iterator<char>(dm_text_file)),std::istreambuf_iterator<char>());dm_text_file.close();
        require(dm_text.find("https://discord.com/channels/@me/960/")!=std::string::npos,"DM text export used an invalid Discord link");
        busy=chat_running=true;TabCtrl_SetCurSel(GetDlgItem(window,Tabs),2);switch_view(window);
        require(chat_dm&&TabCtrl_GetCurSel(GetDlgItem(window,Tabs))==3,"Active DM collection allowed its database/view scope to switch");busy=chat_running=false;chat_update_enabled(window);
        sessions["2"]={{"username","Second account"},{"token","another-fake-ui-test-token"}};chat_fill_combos(window);
        auto select_chat_account=[&](const std::string& account){for(size_t i=0;i<chat_accounts.size();++i)if(chat_accounts[i]==account){SendMessageW(GetDlgItem(window,ChatAccount),CB_SETCURSEL,i,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(ChatAccount,CBN_SELCHANGE),0);return;}throw std::runtime_error("Missing fixture account");};
        select_chat_account("2");require(chat_channels.empty()&&chat_messages.empty()&&chat_checked_channels.empty(),"Switching accounts exposed another account's saved DMs or selection");
        chat_db->channels("@me",Json::array({{{"id","960"},{"name","Same group, separate capture"},{"type",3}}}));
        chat_db->save_messages("@me","960",Json::array({saved_message(21100,"960","Account two saved copy")}),"dm-account-two");chat_load_channels(window);chat_load_messages(window);
        require(chat_total==1,"Second account did not have its own DM database");chat_export(window);
        require(read_json(workspace/L"reports"/L"messages-dm-2.json").size()==1,"DM export was not scoped to the second account");
        select_chat_account("1");require(chat_total==205&&chat_db->message("21100")["message"]["content"]=="dmneedle private context","Second account overwrote the first account's DM capture");
        require(chat_database_path("1",true)!=chat_database_path("2",true)&&chat_downloads_path("1",true)!=chat_downloads_path("2",true),"DM storage/download paths were shared between accounts");
        auto kept_sessions=sessions;sessions=Json::object();switch_view(window);
        require(!chat_db&&chat_channels.empty()&&chat_messages.empty(),"No connected account exposed stale private history");
        sessions=kept_sessions;switch_view(window);require(chat_dm&&chat_db&&chat_total==205,"Restoring the connected account did not restore its DM view");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),2);switch_view(window);
        require(!chat_dm&&value(GetDlgItem(window,ChatFrom))=="2026-01-01"&&value(GetDlgItem(window,ChatAuthor))=="13"&&!IsWindowEnabled(GetDlgItem(window,ChatFrom)),"Returning to server channels did not restore independent options");
        require(chat_db->message("21100").is_null()&&!chat_db->message("13000").is_null(),"DM records contaminated server message storage");
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),3);switch_view(window);
        require(chat_dm&&value(GetDlgItem(window,ChatFrom)).empty()&&IsWindowEnabled(GetDlgItem(window,ChatFrom)),"Returning to DMs did not retain a blank date");
        for(int tab:{2,3}){
            TabCtrl_SetCurSel(GetDlgItem(window,Tabs),tab);switch_view(window);
            set(GetDlgItem(window,ChatAuthor),"");set(GetDlgItem(window,ChatChannelSearch),"");chat_check(window,ChatChannelOnly,false);chat_check(window,ChatSuggestions,false);chat_check(window,ChatDeleted,false);
            auto scope=chat_guild(window);auto channel=tab==3?std::string("960"):std::string("950");auto mid=tab==3?31001:31000;
            auto forwarded=saved_message(mid,channel,"");forwarded["message_snapshots"]=Json::array({{{"message",{{"content","forwarduin eedle conversation"},{"attachments",Json::array({{{"id","9100"},{"filename","forward-ui.txt"},{"size",3}}})}}}}});
            chat_db->save_messages(scope,channel,Json::array({forwarded}),"forward-ui");chat_load_channels(window);set(GetDlgItem(window,ChatSearch),"forwarduin eedle");
            require(chat_messages.size()==1&&chat_messages[0]["content"].get<std::string>().find("[Forwarded message]")!=std::string::npos,"Forward not searchable/displayed in server or DM list");
            ListView_SetItemState(GetDlgItem(window,ChatMessages),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);chat_message_detail(window);
            require(value(GetDlgItem(window,ChatDetail)).find("forwarduin eedle")!=std::string::npos&&value(GetDlgItem(window,ChatDetail)).find("forward-ui.txt")!=std::string::npos,"Forward text/files missing from details");
            chat_export(window);auto base=workspace/L"reports"/(tab==3?L"messages-dm-1":L"messages-"+wide(scope));auto jp=base;jp+=L".json";auto tp=base;tp+=L".txt";
            auto exported=read_json(jp);std::ifstream input(tp);std::string txt((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());input.close();
            require(exported.size()==1&&exported[0]["message"]["message_snapshots"]==forwarded["message_snapshots"]&&exported[0]["parsed_content"].get<std::string>().find("forwarduin eedle")!=std::string::npos&&txt.find("forwarduin eedle")!=std::string::npos,"Forward missing from JSON/TXT export");
            double_click_message(0);require(chat_selected_message(window)==std::to_string(mid)&&chat_selected_channel(window)==channel&&value(GetDlgItem(window,ChatSearch)).empty(),"Forward context navigation left the receiving conversation");
        }
        // Exercise the real repeating worker and UI refresh without Discord requests.
        TabCtrl_SetCurSel(GetDlgItem(window,Tabs),2);switch_view(window);
        set(GetDlgItem(window,ChatAuthor),"");set(GetDlgItem(window,ChatSearch),"");set(GetDlgItem(window,ChatChannelSearch),"");chat_check(window,ChatChannelOnly,false);chat_check(window,ChatDelete,true);set(GetDlgItem(window,ChatFrom),"invalid manual date");
        chat_command(window,ChatClear,BN_CLICKED);for(size_t i=0;i<chat_channels.size();++i)ListView_SetCheckState(GetDlgItem(window,ChatChannels),static_cast<int>(i),chat_channels[i]["id"]=="950"||chat_channels[i]["id"]=="951");chat_begin(window,false,true);
        require(chat_live&&chat_running&&busy&&IsWindowEnabled(GetDlgItem(window,ChatChannels))&&value(GetDlgItem(window,ChatLive))=="Stop capture","Live start disabled the dark channel list or failed to expose Stop capture");
        auto live_channels=GetDlgItem(window,ChatChannels);bool was_checked=ListView_GetCheckState(live_channels,0)!=FALSE;ListView_SetCheckState(live_channels,0,!was_checked);require((ListView_GetCheckState(live_channels,0)!=FALSE)==was_checked&&ListView_GetBkColor(live_channels)==surface&&ListView_GetTextBkColor(live_channels)==surface,"Live channel list lost its checkbox lock or dark colors");
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        while(std::chrono::steady_clock::now()<until&&(live_test_cycles<2||std::none_of(chat_messages.begin(),chat_messages.end(),[](const Json& row){return row["id"]=="40002";}))){MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        bool repeated=live_test_cycles>=2&&std::any_of(chat_messages.begin(),chat_messages.end(),[](const Json& row){return row["id"]=="40002";});
        if(chat_live)chat_command(window,ChatLive,BN_CLICKED);if(network_thread.joinable())network_thread.join();
        MSG live_msg;while(PeekMessageW(&live_msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&live_msg);DispatchMessageW(&live_msg);}
        if(!(repeated&&!chat_live&&!chat_running&&!busy&&IsWindowEnabled(GetDlgItem(window,ChatChannels))&&value(GetDlgItem(window,ChatProgressLabel)).find("Live capture stopped")!=std::string::npos))throw std::runtime_error("Live worker did not repeat, refresh, or stop cleanly: cycles="+std::to_string(live_test_cycles.load())+" refreshed="+std::to_string(repeated)+" status="+value(GetDlgItem(window,ChatProgressLabel))+" error="+chat_error);
        require(value(GetDlgItem(window,ChatFrom))=="invalid manual date"&&checked(window,ChatDelete),"Live capture overwrote manual date/deletion settings");
        // Window close must cancel an unlimited collection, not wait forever.
        busy=collecting=true;cancel_collection=false;close_requested=false;
        SendMessageW(window,WM_CLOSE,0,0);require(cancel_collection&&close_requested,"Close did not cancel active collection");
        busy=collecting=false;DestroyWindow(window);window=nullptr;database.reset();chat_db.reset();
        DeleteObject(font);DeleteObject(background);DeleteObject(surface_brush);
        require(std::filesystem::equivalent(workspace.parent_path(),std::filesystem::temp_directory_path())&&workspace.filename().wstring().starts_with(L"sml-ui-test-"),"Invalid cleanup path");
        std::filesystem::remove_all(workspace);
        std::cout<<"PASS: progress, cancellation, partial merge, checkbox filtering, deduplication, sequential batch, failure continuation, exports, context navigation and per-account DM controls.\n";
        return 0;
    }catch(const std::exception& e){cancel_collection=true;if(network_thread.joinable())network_thread.join();if(window)DestroyWindow(window);std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}

