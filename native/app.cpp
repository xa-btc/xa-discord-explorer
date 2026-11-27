#include "core.h"
#include "session.h"
#include "chat.h"
#include "audit.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <memory>
#include <fstream>
#include <thread>
#include <mutex>
#include <cwctype>
#include <chrono>
#include <algorithm>
#include <optional>
#include <cstring>

using namespace sml;
namespace {
constexpr int Import=101, Scan=102, Preview=105, Export=106, Folder=107, Connect=108, Refresh=109, Forget=110, Collect=111;
constexpr int User=201, Target=202, Card=203, Details=204, Table=205, Status=206;
constexpr int Token=210, Remember=211, Accounts=212, Search=213, Progress=214, ProgressText=215, Cancel=112;
constexpr int SelectAll=113,ClearAll=114,ScanSelected=115,ScanCounts=118;
bool batch_counts=false;
constexpr int RemoveLeaves=217,ScanAge=218;
constexpr int age_options[]={0,1,2,7,14,30};
int scan_days=0;
bool remove_server_leaves=false,active_remove_leaves=false;
size_t batch_skipped=0;


constexpr int Tabs=216,MemberTable=220,MemberSearch=221,MemberDetails=222,MemberPage=223,MemberLabel=224,MemberDetailLabel=225,PreviousPage=116,NextPage=117;
constexpr const wchar_t* tab_labels[]={L"View Servers",L"View Members",L"View Channels",L"View DMs",L"Audit Log"};
HWND member_table{};
bool viewing_members=false,server_desc=false,member_desc=false;
int server_sort=0,member_sort=0;
size_t member_offset=0;
Json member_rows=Json::array();
int64_t member_total=0;
constexpr UINT NetworkDone=WM_APP+1;
constexpr UINT CollectionDone=WM_APP+2;
Json collected;
std::string collection_error;
Path workspace, inbox, db_path;
void load_scan_options() {
    scan_days=0;remove_server_leaves=false;
    try{auto path=workspace/L"scan-options.json";if(!std::filesystem::exists(path))return;auto options=read_json(path);
        int days=options.value("outdated_days",0);for(int allowed:age_options)if(days==allowed)scan_days=days;
        remove_server_leaves=options.value("remove_server_leaves",false);
    }catch(...){scan_days=0;remove_server_leaves=false;}
}
std::unique_ptr<Database> database;
HFONT font{};
HWND table{}, status{}, user{}, target_edit{}, card{}, details{};
Json latest_preview;
double max_age=24;
constexpr COLORREF bg=RGB(15,20,29), surface=RGB(24,32,46), ink=RGB(232,237,245), muted=RGB(148,163,184), accent=RGB(79,143,247);
HBRUSH background{},surface_brush{};
Json sessions=Json::object(),remembered=Json::object();
std::vector<std::string> account_ids;
std::thread network_thread;
bool busy=false,close_requested=false;
std::vector<std::string> table_keys;
std::set<std::string> checked_servers;
bool rebuilding_table=false,syncing_checks=false,batch_active=false;
std::vector<std::string> batch_queue;
size_t batch_index=0;
Json batch_results=Json::array();
std::string batch_account;
std::string server_id_at(int row){return row>=0&&static_cast<size_t>(row)<table_keys.size()?table_keys[row].substr(0,table_keys[row].find('/')):std::string();}
void sync_checks() {
    syncing_checks=true;
    for(int i=0;i<ListView_GetItemCount(table);++i)ListView_SetCheckState(table,i,checked_servers.contains(server_id_at(i)));
    syncing_checks=false;
}
struct AccountResult { Json snapshot;std::string token,error;bool remember=false; };
std::vector<AccountResult> completed;
std::mutex completed_mutex;
std::atomic<bool> cancel_collection{false};
bool collecting=false;
CollectionProgress collection_progress;
std::chrono::steady_clock::time_point collection_started;
void update_collect(HWND window) {
    EnableWindow(GetDlgItem(window,Collect),!busy&&!sessions.empty()&&ListView_GetNextItem(table,-1,LVNI_SELECTED)>=0);
    EnableWindow(GetDlgItem(window,RemoveLeaves),!busy);EnableWindow(GetDlgItem(window,ScanAge),!busy);
    EnableWindow(GetDlgItem(window,ScanCounts),!busy&&!sessions.empty()&&!checked_servers.empty());
    EnableWindow(GetDlgItem(window,ScanSelected),!busy&&!sessions.empty()&&!checked_servers.empty());
    EnableWindow(GetDlgItem(window,SelectAll),!busy);EnableWindow(GetDlgItem(window,ClearAll),!busy);
    auto text=std::to_wstring(checked_servers.size())+L" servers checked";SetWindowTextW(GetDlgItem(window,3),text.c_str());
}

LRESULT CALLBACK dark_tabs(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT||msg==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};HDC dc=msg==WM_PAINT?BeginPaint(h,&paint):reinterpret_cast<HDC>(wp);
        int saved=SaveDC(dc);RECT bounds{};GetClientRect(h,&bounds);FillRect(dc,&bounds,background);
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
        for(int i=0;i<TabCtrl_GetItemCount(h);++i){
            RECT cell{};TabCtrl_GetItemRect(h,i,&cell);bool selected=i==TabCtrl_GetCurSel(h);
            FillRect(dc,&cell,selected?surface_brush:background);SetTextColor(dc,selected?ink:muted);
            wchar_t label[128]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=label;item.cchTextMax=128;TabCtrl_GetItem(h,i,&item);
            DrawTextW(dc,label,-1,&cell,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(selected){RECT line=cell;line.top=line.bottom-2;auto brush=CreateSolidBrush(accent);FillRect(dc,&line,brush);DeleteObject(brush);}
            if(selected&&GetFocus()==h){InflateRect(&cell,-3,-3);DrawFocusRect(dc,&cell);}
        }
        RestoreDC(dc,saved);if(msg==WM_PAINT)EndPaint(h,&paint);return 0;
    }
    auto result=DefSubclassProc(h,msg,wp,lp);
    if(msg==WM_LBUTTONDOWN||msg==WM_KEYDOWN||msg==TCM_SETCURSEL||msg==WM_SETFOCUS||msg==WM_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,dark_tabs,id);
    return result;
}

LRESULT CALLBACK dark_control(HWND h,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR kind,DWORD_PTR) {
    auto result=DefSubclassProc(h,msg,wp,lp);
    if(msg==WM_PAINT||msg==WM_PRINTCLIENT) {
        HDC dc=msg==WM_PRINTCLIENT?reinterpret_cast<HDC>(wp):GetDC(h);
        RECT r{};GetClientRect(h,&r);FillRect(dc,&r,surface_brush);SetBkMode(dc,TRANSPARENT);SelectObject(dc,font);
        SetTextColor(dc,kind==1?muted:ink);
        if(kind==1) {
            for(int i=0;i<Header_GetItemCount(h);++i) {
                RECT cell{};Header_GetItemRect(h,i,&cell);cell.left+=8;
                wchar_t text[128]{};HDITEMW item{};item.mask=HDI_TEXT|HDI_FORMAT;item.pszText=text;item.cchTextMax=128;Header_GetItem(h,i,&item);
                std::wstring label=text;if(item.fmt&HDF_SORTUP)label+=L" \x25B2";if(item.fmt&HDF_SORTDOWN)label+=L" \x25BC";
                DrawTextW(dc,label.c_str(),-1,&cell,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            }
        } else {
            auto border=CreateSolidBrush(RGB(49,65,87));FrameRect(dc,&r,border);DeleteObject(border);
            RECT textrect=r;textrect.left+=8;textrect.right-=28;
            int index=static_cast<int>(SendMessageW(h,CB_GETCURSEL,0,0));
            std::wstring label=L"Select a connected account";
            if(index>=0){int n=static_cast<int>(SendMessageW(h,CB_GETLBTEXTLEN,index,0));if(n>=0){label.resize(n+1);SendMessageW(h,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(label.data()));}}
            DrawTextW(dc,label.c_str(),-1,&textrect,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);
            RECT arrow=r;arrow.left=arrow.right-24;DrawTextW(dc,L"\x25BE",1,&arrow,DT_SINGLELINE|DT_VCENTER|DT_CENTER);
            if(GetFocus()==h){InflateRect(&textrect,-2,-3);DrawFocusRect(dc,&textrect);}
        }
        if(msg!=WM_PRINTCLIENT)ReleaseDC(h,dc);
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(h,dark_control,kind);
    return result;
}

HWND control(HWND parent,const wchar_t* cls,const wchar_t* text,DWORD style,int cid) {
    HWND h=CreateWindowExW(cls==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text,
        WS_CHILD|WS_VISIBLE|style,0,0,10,10,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(cid)),GetModuleHandleW(nullptr),nullptr);
    if(!h)throw std::runtime_error("Cannot create window control");
    SetWindowTheme(h,L"DarkMode_Explorer",nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;
}
std::string value(HWND edit) {
    int n=GetWindowTextLengthW(edit);std::wstring text(n+1,L'\0');GetWindowTextW(edit,text.data(),n+1);text.resize(n);
    auto first=text.find_first_not_of(L" \t\r\n");if(first==std::wstring::npos)return {};
    return utf8(text.substr(first,text.find_last_not_of(L" \t\r\n")-first+1));
}
void set(HWND h,const std::string& text){SetWindowTextW(h,wide(text).c_str());}
void show_collection_progress(HWND window,const std::string& outcome="") {
    CollectionProgress progress;{std::lock_guard lock(completed_mutex);progress=collection_progress;}
    auto seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-collection_started).count();
    auto time=std::to_string(seconds/60)+":"+(seconds%60<10?"0":"")+std::to_string(seconds%60);
    std::string text=std::to_string(progress.members);
    if(batch_active)text="Server "+std::to_string(batch_index+1)+"/"+std::to_string(batch_queue.size())+" | "+text;
    if(progress.total>=0)text+=" / "+std::to_string(progress.total);
    text+=" members | Elapsed "+time+" | "+(outcome.empty()?(cancel_collection?"Cancelling...":progress.stage):outcome);
    if(batch_counts)text="Server "+std::to_string(std::min(batch_index+1,batch_queue.size()))+"/"+std::to_string(batch_queue.size())+" | Counts in one connection | Elapsed "+time+" | "+(outcome.empty()?(cancel_collection?"Cancelling...":progress.stage):outcome);
    set(GetDlgItem(window,ProgressText),text);
    HWND bar=GetDlgItem(window,Progress);bool unknown=progress.total<0&&collecting;
    auto style=GetWindowLongPtrW(bar,GWL_STYLE);
    if(unknown!=((style&PBS_MARQUEE)!=0)){SetWindowLongPtrW(bar,GWL_STYLE,unknown?(style|PBS_MARQUEE):(style&~PBS_MARQUEE));SendMessageW(bar,PBM_SETMARQUEE,unknown,35);}
    if(!unknown){SendMessageW(bar,PBM_SETRANGE32,0,1000);auto amount=outcome=="Complete"?1000:progress.total>0?static_cast<int>(std::min(size_t(999),progress.members*1000/static_cast<size_t>(progress.total))):0;SendMessageW(bar,PBM_SETPOS,amount,0);}
}
void cancel_active_collection(HWND window) {
    if(!collecting)return;
    cancel_collection=true;EnableWindow(GetDlgItem(window,Cancel),FALSE);
    show_collection_progress(window);set(status,"Cancelling collection; existing database roster will be kept.");
}
void show_details(const Json& data){auto text=data.dump(2);std::string lines;for(char c:text){if(c=='\n')lines+='\r';lines+=c;}set(details,lines);}
void audit_sync_accounts(HWND window);
void update_accounts(HWND window) {
    HWND combo=GetDlgItem(window,Accounts);auto selected=SendMessageW(combo,CB_GETCURSEL,0,0);
    std::string previous=selected>=0&&static_cast<size_t>(selected)<account_ids.size()?account_ids[selected]:"";
    SendMessageW(combo,CB_RESETCONTENT,0,0);account_ids.clear();int index=0,choose=0;
    for(auto& [uid,record]:sessions.items()) {
        account_ids.push_back(uid);auto label=wide(record["username"].get<std::string>()+" ("+uid+")"+(remembered.contains(uid)?" - remembered":" - this run"));
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(uid==previous)choose=index;++index;
    }
    if(index)SendMessageW(combo,CB_SETCURSEL,choose,0);
    EnableWindow(GetDlgItem(window,Refresh),!busy&&!sessions.empty());EnableWindow(GetDlgItem(window,Forget),!busy&&!sessions.empty());
    update_collect(window);
    audit_sync_accounts(window);
}
void begin_collection(HWND window,const std::string& queued_server="") {
    if(busy)return;
    if(!batch_active){batch_counts=false;active_remove_leaves=remove_server_leaves;}
    int row=ListView_GetNextItem(table,-1,LVNI_SELECTED);
    if(queued_server.empty()&&(row<0||static_cast<size_t>(row)>=table_keys.size()))throw std::runtime_error("Select a server first");
    auto account=SendMessageW(GetDlgItem(window,Accounts),CB_GETCURSEL,0,0);
    if(account<0||static_cast<size_t>(account)>=account_ids.size())throw std::runtime_error("Select a connected account first");
    auto sid=queued_server.empty()?server_id_at(row):queued_server;
    auto token=sessions.at(batch_active?batch_account:account_ids[account]).at("token").get<std::string>();
    if(network_thread.joinable())network_thread.join();
    busy=true;collecting=true;cancel_collection=false;collection_started=std::chrono::steady_clock::now();
    {std::lock_guard lock(completed_mutex);collection_progress=CollectionProgress{};}
    EnableWindow(GetDlgItem(window,Cancel),TRUE);SetTimer(window,2,250,nullptr);show_collection_progress(window);
    EnableWindow(GetDlgItem(window,Connect),FALSE);EnableWindow(GetDlgItem(window,Accounts),FALSE);update_accounts(window);
    set(status,std::string(batch_counts?"Reading count for server ":"Collecting server ")+sid+" through your user session. Cancel stops collection and remaining queued servers.");
    network_thread=std::thread([window,sid,token=std::move(token),counts=batch_counts,requested_servers=batch_queue,pause_before=batch_active&&!batch_counts&&batch_index>0]() mutable {
        Json result;std::string error;
        try{
            if(pause_before){
                {std::lock_guard lock(completed_mutex);collection_progress.stage="Waiting between servers";}
                for(int i=0;i<20&&!cancel_collection;++i)std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if(cancel_collection)throw std::runtime_error("Collection cancelled before the next server started");
            auto progress=[](const CollectionProgress& state){std::lock_guard lock(completed_mutex);collection_progress=state;};
            result=counts?collect_server_counts(token,requested_servers,cancel_collection,progress):collect_members(token,sid,cancel_collection,progress);
        }catch(const std::exception& e){error=e.what();}
        SecureZeroMemory(token.data(),token.size());
        {std::lock_guard lock(completed_mutex);collected=std::move(result);collection_error=std::move(error);}
        PostMessageW(window,CollectionDone,0,0);
    });
}
void begin_batch(HWND window,bool counts=false) {
    if(busy||checked_servers.empty())return;
    auto account=SendMessageW(GetDlgItem(window,Accounts),CB_GETCURSEL,0,0);
    if(account<0||static_cast<size_t>(account)>=account_ids.size())throw std::runtime_error("Select a connected account first");
    batch_counts=counts;active_remove_leaves=remove_server_leaves;batch_account=account_ids[account];batch_queue.clear();batch_skipped=0;
    auto checked_at=now();for(const auto& sid:checked_servers){if(database->scan_due(sid,counts,scan_days,checked_at))batch_queue.push_back(sid);else ++batch_skipped;}
    if(batch_queue.empty()){set(status,"Nothing to scan: all "+std::to_string(batch_skipped)+" selected servers were checked within the chosen period.");return;}
    batch_index=0;batch_results=Json::array();batch_active=true;
    try{begin_collection(window,batch_queue.front());}catch(...){batch_active=false;throw;}
}
void begin_network(HWND window,bool new_account) {
    if(busy)return;
    struct Work {std::string token,expected;bool remember;};std::vector<Work> work;
    if(new_account) {
        auto token=value(GetDlgItem(window,Token));if(token.empty())throw std::runtime_error("Enter your account's session token in the masked field");
        work.push_back({token,"",SendMessageW(GetDlgItem(window,Remember),BM_GETCHECK,0,0)==BST_CHECKED});
        SetWindowTextW(GetDlgItem(window,Token),L"");
    } else for(auto& [uid,record]:sessions.items())work.push_back({record["token"].get<std::string>(),uid,remembered.contains(uid)});
    if(work.empty())throw std::runtime_error("Connect an account first");
    if(network_thread.joinable())network_thread.join();
    busy=true;EnableWindow(GetDlgItem(window,Connect),FALSE);update_accounts(window);
    set(status,"Checking connected accounts' own server memberships...");
    network_thread=std::thread([window,work=std::move(work)]() mutable {
        std::vector<AccountResult> results;
        for(auto& item:work) {
            AccountResult result;result.token=std::move(item.token);result.remember=item.remember;
            try {result.snapshot=connect_account(result.token);
                if(!item.expected.empty()&&result.snapshot["user_id"]!=item.expected)throw std::runtime_error("Saved session returned a different account. Reconnect it.");
            }catch(const std::exception& e){result.snapshot=nullptr;result.error=e.what();}
            results.push_back(std::move(result));
        }
        {std::lock_guard lock(completed_mutex);completed=std::move(results);}
        PostMessageW(window,NetworkDone,0,0);
    });
}
void sort_indicator(HWND list,int column,bool descending) {
    HWND header=ListView_GetHeader(list);for(int i=0;i<Header_GetItemCount(header);++i){HDITEMW item{};item.mask=HDI_FORMAT;Header_GetItem(header,i,&item);item.fmt&=~(HDF_SORTUP|HDF_SORTDOWN);if(i==column)item.fmt|=descending?HDF_SORTDOWN:HDF_SORTUP;Header_SetItem(header,i,&item);}InvalidateRect(header,nullptr,TRUE);
}
void update_table(bool preserve=true) {
    if(!table)return;
    auto key_at=[&](int i){return i>=0&&static_cast<size_t>(i)<table_keys.size()?table_keys[i]:std::string();};
    auto selected=key_at(ListView_GetNextItem(table,-1,LVNI_SELECTED));auto top=key_at(ListView_GetTopIndex(table));
    auto query=wide(value(GetDlgItem(GetParent(table),Search)));
    auto lower=[](std::wstring s){for(auto& c:s)c=static_cast<wchar_t>(std::towlower(c));return s;};query=lower(query);
    auto rows=database->servers();
    std::sort(rows.begin(),rows.end(),[&](const Json& a,const Json& b){
        int comparison=0;
        if(server_sort==1){auto x=std::stoull(a["server_id"].get<std::string>()),y=std::stoull(b["server_id"].get<std::string>());comparison=x<y?-1:x>y?1:0;}
        else if(server_sort==2||server_sort==6){auto key=server_sort==2?"member_count":"server_total";auto x=a[key].get<int64_t>(),y=b[key].get<int64_t>();comparison=x<y?-1:x>y?1:0;}
        else {const char* key=server_sort==7?"count_checked":server_sort==3?"captured_at":server_sort==5?"coverage":"server_name";auto x=lower(wide(a[key].get<std::string>())),y=lower(wide(b[key].get<std::string>()));if(server_sort==4){x=a["stale"].get<bool>()?L"Stale":L"Fresh";y=b["stale"].get<bool>()?L"Stale":L"Fresh";}comparison=x<y?-1:x>y?1:0;}
        if(!comparison)comparison=a["server_id"].get<std::string>().compare(b["server_id"].get<std::string>());
        return server_desc?comparison>0:comparison<0;
    });
    rebuilding_table=true;SendMessageW(table,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(table);table_keys.clear();int index=0,restore_top=-1;
    for(const auto& row:rows) {
        auto sid=row["server_id"].get<std::string>();auto name=row["server_name"].get<std::string>();
        if(!query.empty()&&lower(wide(name+" "+sid)).find(query)==std::wstring::npos)continue;
        auto key=sid;table_keys.push_back(key);
        std::wstring columns[]={wide(row["server_name"]),wide(row["server_id"]),wide(std::to_string(row["member_count"].get<int64_t>())),
            wide(row["captured_at"].get<std::string>().substr(0,19)+" UTC"),row["stale"].get<bool>()?L"Stale - excluded":L"Fresh",wide(row.value("coverage",std::string("Imported roster"))),row["server_total"].get<int64_t>()<0?L"Unknown":wide(std::to_string(row["server_total"].get<int64_t>())),row["count_checked"].get<std::string>().empty()?L"Never":wide(row["count_checked"].get<std::string>().substr(0,19)+" UTC")};
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=index;item.pszText=columns[0].data();ListView_InsertItem(table,&item);
        for(int col=1;col<8;++col)ListView_SetItemText(table,index,col,columns[col].data());
        if(preserve&&key==selected)ListView_SetItemState(table,index,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        if(preserve&&key==top)restore_top=index;
        ++index;
    }
    sync_checks();rebuilding_table=false;
    SendMessageW(table,WM_SETREDRAW,TRUE,0);
    if(restore_top>=0){ListView_EnsureVisible(table,restore_top,FALSE);RECT r{};ListView_GetItemRect(table,restore_top,&r,LVIR_BOUNDS);ListView_Scroll(table,0,(restore_top-ListView_GetTopIndex(table))*(r.bottom-r.top));}
    InvalidateRect(table,nullptr,TRUE);
    sort_indicator(table,server_sort,server_desc);
    update_collect(GetParent(table));
}
void update_members(HWND window,bool reset=false) {
    if(!member_table)return;if(reset)member_offset=0;
    auto result=database->members_page(value(GetDlgItem(window,MemberSearch)),member_sort,member_desc,member_offset);
    member_total=result["total"].get<int64_t>();member_rows=std::move(result["rows"]);
    SendMessageW(member_table,WM_SETREDRAW,FALSE,0);ListView_DeleteAllItems(member_table);
    for(size_t i=0;i<member_rows.size();++i){auto& row=member_rows[i];std::wstring columns[]={wide(row["username"]),wide(row["user_id"]),wide(std::to_string(row["server_count"].get<int64_t>())),wide(row["last_seen"].get<std::string>().substr(0,19)+" UTC")};
        LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=columns[0].data();ListView_InsertItem(member_table,&item);
        for(int c=1;c<4;++c)ListView_SetItemText(member_table,static_cast<int>(i),c,columns[c].data());}
    SendMessageW(member_table,WM_SETREDRAW,TRUE,0);InvalidateRect(member_table,nullptr,TRUE);
    sort_indicator(member_table,member_sort,member_desc);
    EnableWindow(GetDlgItem(window,PreviousPage),member_offset>0);EnableWindow(GetDlgItem(window,NextPage),member_offset+member_rows.size()<static_cast<size_t>(member_total));
    set(GetDlgItem(window,MemberPage),std::to_string(member_total)+" members | Page "+std::to_string(member_offset/500+1)+" / "+std::to_string(std::max(int64_t(1),(member_total+499)/500)));
    set(GetDlgItem(window,MemberDetails),"Select a member to see their cached server memberships.");
}
#include "chat_ui.inc"
#include "audit_ui.inc"

void switch_view(HWND window) {
    auto selected_tab=TabCtrl_GetCurSel(GetDlgItem(window,Tabs));
    if(chat_running&&(selected_tab==2||selected_tab==3)&&(selected_tab==3)!=chat_dm){selected_tab=chat_dm?3:2;TabCtrl_SetCurSel(GetDlgItem(window,Tabs),selected_tab);set(status,"Finish or cancel collection before switching between channels and DMs.");InvalidateRect(GetDlgItem(window,Tabs),nullptr,TRUE);}
    bool viewing_channels=selected_tab==2||selected_tab==3;
    viewing_members=TabCtrl_GetCurSel(GetDlgItem(window,Tabs))==1;
    for(int cid:{3,4,5,6,7,8,10,11,12,Token,Remember,Accounts,Connect,Refresh,Forget,Import,Scan,Folder,Collect,SelectAll,ClearAll,ScanSelected,ScanCounts,RemoveLeaves,ScanAge,13,14,Search,Progress,ProgressText,Cancel,Table,User,Target,Preview,Export,Card,Details})ShowWindow(GetDlgItem(window,cid),selected_tab==0?SW_SHOW:SW_HIDE);
    for(int cid:{MemberTable,MemberSearch,MemberDetails,MemberPage,MemberLabel,MemberDetailLabel,PreviousPage,NextPage})ShowWindow(GetDlgItem(window,cid),viewing_members?SW_SHOW:SW_HIDE);
    if(viewing_members)update_members(window,true);
    chat_show(window,viewing_channels,selected_tab==3);
    audit_show(window,selected_tab==4);
}
void preview() {
    latest_preview=database->preview(value(user),value(target_edit));
    std::string text="MEMBER JOINED - PREVIEW\r\n\r\nMember "+latest_preview["user_id"].get<std::string>();
    auto& matches=latest_preview["common_servers"];
    if(!matches.empty()) {
        text+="\r\n\r\nAlso part of\r\n";
        for(auto& match:matches)text+=match["server_name"].get<std::string>()+"\r\n";
        set(status,std::to_string(matches.size())+" shared community match(es).");
    } else set(status,"No current matches. The join card has no shared-community field.");
    set(card,text);show_details(latest_preview);
}
void refresh_preview_if_present(){if(!latest_preview.is_null())preview();}
Path choose_file(HWND window,bool save) {
    wchar_t buffer[32768]{};if(save)wcscpy_s(buffer,L"join-preview.json");
    OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFile=buffer;ofn.nMaxFile=32768;
    ofn.lpstrFilter=L"JSON snapshot (*.json)\0*.json\0All files\0*.*\0";
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    ofn.lpstrDefExt=L"json";
    if(save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))return Path(buffer);
    return {};
}
void run_scan(bool automatic) {
    auto results=scan(*database,inbox);bool changed=false;size_t errors=0;
    for(auto& item:results) {
        if(item.contains("error")){++errors;continue;}
        for(auto& result:item["results"])if(result["status"]=="updated")changed=true;
    }
    update_table();
    if(changed)refresh_preview_if_present();
    // A previously visible match also disappears when its snapshot ages out.
    else if(!latest_preview.is_null()) {
        auto fresh=database->preview(value(user),value(target_edit));
        if(fresh!=latest_preview)preview();
    }
    if(!automatic||changed||errors) {
        show_details(results);
        set(status,errors?"Inbox checked; "+std::to_string(errors)+" file(s) rejected. See operation details.":"Inbox refreshed. Current membership is saved.");
    }
}
void action(HWND window,int cid) {
    switch(cid) {
    case Import: {
        auto path=choose_file(window,false);if(path.empty())return;
        auto result=database->apply(read_json(path));update_table();refresh_preview_if_present();show_details(result);
        set(status,"Snapshot imported. Complete lists replace rosters; partial lists merge known members.");break;
    }
    case Scan:run_scan(false);break;
    case Connect:begin_network(window,true);break;
    case Refresh:begin_network(window,false);break;
    case Collect:begin_collection(window);break;
    case ScanSelected:begin_batch(window);break;
    case ScanCounts:begin_batch(window,true);break;
    case SelectAll:if(!busy){for(int i=0;i<ListView_GetItemCount(table);++i)checked_servers.insert(server_id_at(i));sync_checks();update_collect(window);}break;
    case ClearAll:if(!busy){checked_servers.clear();sync_checks();update_collect(window);}break;
    case PreviousPage:if(member_offset>=500){member_offset-=500;update_members(window);}break;
    case NextPage:if(member_offset+500<static_cast<size_t>(member_total)){member_offset+=500;update_members(window);}break;
    case Cancel:cancel_active_collection(window);break;
    case Forget: {
        if(busy)return;auto selection=SendMessageW(GetDlgItem(window,Accounts),CB_GETCURSEL,0,0);
        if(selection<0||static_cast<size_t>(selection)>=account_ids.size())return;
        auto uid=account_ids[selection];auto next=remembered;next.erase(uid);
        save_sessions(workspace/L".auth"/L"sessions.bin",next);database->forget_account(uid);
        remembered=std::move(next);sessions.erase(uid);update_accounts(window);update_table();refresh_preview_if_present();
        set(status,"Account forgotten. Its saved session and connected-account matches were removed.");break;
    }
    case Preview:preview();break;
    case Export: {
        preview();auto path=choose_file(window,true);if(!path.empty()){write_json(path,latest_preview);set(status,"Join preview JSON exported.");}break;
    }
    case Folder:std::filesystem::create_directories(inbox);ShellExecuteW(window,L"open",inbox.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;
    }
}
void layout(HWND window) {
    RECT rect{};GetClientRect(window,&rect);double scale=GetDpiForWindow(window)/96.0;
    int w=static_cast<int>(rect.right/scale),h=static_cast<int>(rect.bottom/scale);
    auto move=[&](int cid,int x,int y,int cw,int ch){MoveWindow(GetDlgItem(window,cid),static_cast<int>(x*scale),static_cast<int>(y*scale),static_cast<int>(cw*scale),static_cast<int>(ch*scale),TRUE);};
    move(Tabs,24,12,w-48,32);move(MemberLabel,24,63,100,22);move(MemberSearch,132,59,w-156,28);
    move(MemberTable,24,101,w-48,std::max(120,h-371));move(PreviousPage,24,h-254,105,30);move(NextPage,139,h-254,105,30);move(MemberPage,260,h-248,w-284,22);
    move(MemberDetailLabel,24,h-211,w-48,22);move(MemberDetails,24,h-183,w-48,120);
    move(10,24,56,95,22);move(Token,123,52,310,28);move(Remember,448,53,170,28);move(Connect,635,51,180,32);
    move(11,24,97,95,22);move(Accounts,123,91,390,250);move(Refresh,529,90,155,32);move(Forget,700,90,140,32);
    move(Import,24,136,155,34);move(Scan,189,136,140,34);move(Folder,339,136,130,34);
    move(Collect,489,136,190,34);
    move(SelectAll,24,175,110,30);move(ClearAll,144,175,105,30);move(ScanSelected,259,175,160,30);move(ScanCounts,429,175,160,30);move(3,609,180,w-633,22);
    move(12,24,214,100,22);move(Search,132,210,w-156,28);
    move(13,24,247,145,22);move(RemoveLeaves,172,243,70,140);move(14,258,247,125,22);move(ScanAge,386,243,120,220);move(Progress,522,247,std::max(30,w-758),22);move(Cancel,w-220,243,196,30);move(ProgressText,24,279,w-48,22);
    int list_h=std::max(100,h-678);move(Table,24,307,w-48,list_h);
    int y=319+list_h,left=(w-64)/2;
    move(4,24,y,left,22);move(5,24,y+30,100,22);move(User,132,y+27,left-108,27);
    move(6,24,y+66,100,22);move(Target,132,y+63,left-108,27);
    move(Preview,24,y+103,145,32);move(Export,180,y+103,160,32);
    move(7,left+40,y,left,22);move(Card,left+40,y+29,left,130);
    move(8,24,y+174,w-48,22);move(Details,24,y+201,w-48,80);
    move(Status,24,h-53,w-48,25);move(9,24,h-27,w-48,20);
    int widths[]={std::max(150,(w-48)-745),165,70,225,110,175,110,225};for(int i=0;i<8;++i)ListView_SetColumnWidth(table,i,static_cast<int>(widths[i]*scale));
    chat_layout(window,w,h,scale);
    audit_layout(window,w,h,scale);
    int member_widths[]={std::max(180,w-48-540),190,100,250};for(int i=0;i<4;++i)ListView_SetColumnWidth(member_table,i,static_cast<int>(member_widths[i]*scale));
}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    try {
        switch(message) {
        case WM_CREATE: {
            int dpi=GetDpiForWindow(window);
            font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
            auto tabs=control(window,WC_TABCONTROLW,L"",TCS_OWNERDRAWFIXED|WS_TABSTOP,Tabs);SetWindowTheme(tabs,L"",L"");SetWindowSubclass(tabs,dark_tabs,1,0);
            for(auto title:tab_labels){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<LPWSTR>(title);TabCtrl_InsertItem(tabs,TabCtrl_GetItemCount(tabs),&item);}
            control(window,L"STATIC",L"Find member",0,MemberLabel);auto member_search=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,MemberSearch);SendMessageW(member_search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Username, user ID, server name or server ID"));
            member_table=control(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_BORDER|WS_TABSTOP,MemberTable);
            ListView_SetExtendedListViewStyle(member_table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);ListView_SetBkColor(member_table,surface);ListView_SetTextBkColor(member_table,surface);ListView_SetTextColor(member_table,ink);
            SetWindowTheme(ListView_GetHeader(member_table),L"",L"");SetWindowSubclass(ListView_GetHeader(member_table),dark_control,1,0);
            int col=0;for(auto title:{L"Username",L"User ID",L"Servers",L"Last seen (UTC)"}){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<LPWSTR>(title);column.cx=150;ListView_InsertColumn(member_table,col++,&column);}
            control(window,L"BUTTON",L"Previous",WS_TABSTOP,PreviousPage);control(window,L"BUTTON",L"Next",WS_TABSTOP,NextPage);control(window,L"STATIC",L"",0,MemberPage);
            control(window,L"STATIC",L"Cached server memberships",0,MemberDetailLabel);control(window,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,MemberDetails);
            control(window,L"STATIC",L"Session token",0,10);
            auto token=control(window,L"EDIT",L"",ES_PASSWORD|ES_AUTOHSCROLL|WS_TABSTOP,Token);SendMessageW(token,EM_SETLIMITTEXT,4096,0);
            control(window,L"BUTTON",L"Remember session",BS_AUTOCHECKBOX|WS_TABSTOP,Remember);SendMessageW(GetDlgItem(window,Remember),BM_SETCHECK,BST_CHECKED,0);
            control(window,L"BUTTON",L"Connect account",WS_TABSTOP,Connect);
            control(window,L"STATIC",L"Accounts",0,11);auto accounts=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,Accounts);
            SetWindowSubclass(accounts,dark_control,2,0);
            control(window,L"BUTTON",L"Refresh servers",WS_TABSTOP,Refresh);control(window,L"BUTTON",L"Forget account",WS_TABSTOP,Forget);
            control(window,L"BUTTON",L"Import snapshot...",WS_TABSTOP,Import);control(window,L"BUTTON",L"Refresh inbox",WS_TABSTOP,Scan);
            control(window,L"BUTTON",L"Open inbox",WS_TABSTOP,Folder);
            control(window,L"BUTTON",L"Collect Member List",WS_TABSTOP,Collect);EnableWindow(GetDlgItem(window,Collect),FALSE);
            control(window,L"BUTTON",L"Select all",WS_TABSTOP,SelectAll);control(window,L"BUTTON",L"Clear all",WS_TABSTOP,ClearAll);control(window,L"BUTTON",L"Scan Member Lists",WS_TABSTOP,ScanSelected);control(window,L"BUTTON",L"Scan Server Counts",WS_TABSTOP,ScanCounts);
            control(window,L"STATIC",L"0 servers checked",0,3);
            control(window,L"STATIC",L"Find server",0,12);
            auto search=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,Search);SendMessageW(search,EM_SETLIMITTEXT,200,0);
            SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search server name or ID"));
            control(window,L"STATIC",L"Remove Server Leaves",0,13);control(window,L"STATIC",L"Only scan outdated",0,14);
            for(int cid:{RemoveLeaves,ScanAge}){auto combo=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,cid);SetWindowSubclass(combo,dark_control,2,0);}
            for(auto label:{L"No",L"Yes"})SendMessageW(GetDlgItem(window,RemoveLeaves),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            for(auto label:{L"All",L"1 day",L"2 days",L"7 days",L"14 days",L"30 days"})SendMessageW(GetDlgItem(window,ScanAge),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
            load_scan_options();SendMessageW(GetDlgItem(window,RemoveLeaves),CB_SETCURSEL,remove_server_leaves?1:0,0);
            for(int i=0;i<6;++i)if(age_options[i]==scan_days)SendMessageW(GetDlgItem(window,ScanAge),CB_SETCURSEL,i,0);
            auto progress=control(window,PROGRESS_CLASSW,L"",PBS_SMOOTH,Progress);SetWindowTheme(progress,L"",L"");
            SendMessageW(progress,PBM_SETBKCOLOR,0,surface);SendMessageW(progress,PBM_SETBARCOLOR,0,accent);
            control(window,L"STATIC",L"Ready to collect members",0,ProgressText);
            control(window,L"BUTTON",L"Cancel collection",WS_TABSTOP,Cancel);EnableWindow(GetDlgItem(window,Cancel),FALSE);
            table=control(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_BORDER|WS_TABSTOP,Table);
            ListView_SetExtendedListViewStyle(table,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_CHECKBOXES);
            ListView_SetBkColor(table,surface);ListView_SetTextBkColor(table,surface);ListView_SetTextColor(table,ink);
            SetWindowTheme(ListView_GetHeader(table),L"",L"");
            SetWindowSubclass(ListView_GetHeader(table),dark_control,1,0);
            const wchar_t* headings[]={L"Server",L"Server ID",L"Tracked",L"Checked (UTC)",L"Status",L"Coverage",L"Server total",L"Count checked (UTC)"};
            for(int i=0;i<8;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<LPWSTR>(headings[i]);column.cx=150;ListView_InsertColumn(table,i,&column);}
            int server_order[]={0,1,6,2,7,3,4,5};ListView_SetColumnOrderArray(table,8,server_order);
            control(window,L"STATIC",L"Preview a member joining",0,4);control(window,L"STATIC",L"Member ID",0,5);
            user=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,User);SendMessageW(user,EM_SETLIMITTEXT,64,0);
            control(window,L"STATIC",L"Joining server",0,6);target_edit=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,Target);SendMessageW(target_edit,EM_SETLIMITTEXT,64,0);
            control(window,L"BUTTON",L"Preview join",WS_TABSTOP,Preview);control(window,L"BUTTON",L"Export preview...",WS_TABSTOP,Export);
            control(window,L"STATIC",L"Join message preview",0,7);
            card=control(window,L"EDIT",L"Connect an account or import a snapshot, then enter a member and joining server ID.",ES_MULTILINE|ES_READONLY|WS_VSCROLL,Card);
            control(window,L"STATIC",L"Operation details",0,8);details=control(window,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,Details);
            status=control(window,L"STATIC",L"Ready. Session tokens are entered here and encrypted for your Windows account.",0,Status);
            control(window,L"STATIC",(L"Database: "+db_path.wstring()).c_str(),SS_PATHELLIPSIS,9);
            chat_create(window);audit_create(window);update_table();update_accounts(window);switch_view(window);return 0;
        }
        case WM_SIZE:if(table)layout(window);return 0;
        case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);int dpi=GetDpiForWindow(window);info->ptMinTrackSize={MulDiv(1010,dpi,96),MulDiv(900,dpi,96)};return 0;}
        case ChatDone:chat_finish(window);return 0;
        case ChatLiveUpdated:chat_live_refresh(window);return 0;
        case AuditDone:audit_finish(window);return 0;
        case AuditUpdated:if(audit_running)audit_display_pages(window);return 0;
        case AuditPreviewDone:audit_finish_preview(window);return 0;
        case WM_COMMAND:
            if(audit_command(window,LOWORD(wp),HIWORD(wp)))return 0;
            if(chat_command(window,LOWORD(wp),HIWORD(wp)))return 0;
            if((LOWORD(wp)==ScanAge||LOWORD(wp)==RemoveLeaves)&&HIWORD(wp)==CBN_SELCHANGE){
                auto index=SendMessageW(GetDlgItem(window,ScanAge),CB_GETCURSEL,0,0);scan_days=index>=0&&index<6?age_options[index]:0;
                remove_server_leaves=SendMessageW(GetDlgItem(window,RemoveLeaves),CB_GETCURSEL,0,0)==1;
                write_json(workspace/L"scan-options.json",{{"outdated_days",scan_days},{"remove_server_leaves",remove_server_leaves}});return 0;
            }
            if(LOWORD(wp)==Search&&HIWORD(wp)==EN_CHANGE)update_table(false);
            else if(LOWORD(wp)==MemberSearch&&HIWORD(wp)==EN_CHANGE&&viewing_members)update_members(window,true);
            else if(HIWORD(wp)==BN_CLICKED)action(window,LOWORD(wp));return 0;
        case WM_TIMER:if(wp==AuditFilterTimer){audit_filter_tick(window);return 0;}if(wp==3&&chat_running){chat_tick(window);return 0;}if(wp==2&&collecting)show_collection_progress(window);return 0;
        case CollectionDone: {
            if(network_thread.joinable())network_thread.join();busy=false;
            collecting=false;KillTimer(window,2);EnableWindow(GetDlgItem(window,Cancel),FALSE);
            EnableWindow(GetDlgItem(window,Connect),TRUE);EnableWindow(GetDlgItem(window,Accounts),TRUE);update_accounts(window);
            Json snapshot;std::string error;
            {std::lock_guard lock(completed_mutex);snapshot=std::move(collected);error=std::move(collection_error);}
            if(batch_counts){
                size_t saved=0,failed=0;
                if(cancel_collection||!error.empty()){
                    batch_results.push_back({{"status",cancel_collection?"cancelled":"failed"},{"error",cancel_collection?"Count scan cancelled; existing counts kept":error}});
                }else{
                    for(auto& result:snapshot){
                        try{if(result.contains("error"))throw std::runtime_error(result["error"].get<std::string>());
                            database->save_server_count(result);result["status"]="saved";++saved;
                        }catch(const std::exception& e){result["status"]="failed";result["error"]=e.what();++failed;}
                        batch_results.push_back(result);++batch_index;
                    }
                }
                batch_active=false;update_table();
                show_details({{"servers_selected",batch_queue.size()+batch_skipped},{"servers_skipped_recent",batch_skipped},{"counts_saved",saved},{"counts_failed",failed},{"servers_not_scanned",batch_queue.size()-batch_index},{"results",batch_results}});
                set(status,cancel_collection?"Count scan cancelled; existing counts kept":!error.empty()?error:"Count scan finished: "+std::to_string(saved)+" saved, "+std::to_string(failed)+" failed, "+std::to_string(batch_skipped)+" recent servers skipped.");
                show_collection_progress(window,cancel_collection?"Cancelled":!error.empty()?"Failed":"Complete");
                if(close_requested)DestroyWindow(window);return 0;
            }
            try {
                if(!error.empty())throw std::runtime_error(error);
                if(cancel_collection){snapshot["complete"]=false;snapshot["cancelled"]=true;snapshot["coverage_note"]="Collection cancelled; existing database roster was kept";}
                bool complete=snapshot.at("complete").get<bool>();
                database->apply(snapshot,now(),active_remove_leaves);update_table();
                auto sid=snapshot["server_id"].get<std::string>();auto filename="member-list-"+sid+(complete?"":"-partial")+".json";auto path=workspace/L"reports"/wide(filename);
                auto count=snapshot["members"].size();
                try{write_json(path,snapshot);}catch(...){throw std::runtime_error("Members saved in the database, but the JSON export failed. Check the reports folder permissions.");}
                Json report={{"server_id",sid},{"server_name",snapshot["server_name"]},{"members_collected",count},{"complete",complete},{"coverage",snapshot.value("coverage_note",std::string())},{"saved_file",utf8(path.wstring())},{"members_preview",Json::array()}};
                for(size_t i=0;i<std::min(count,size_t(100));++i)report["members_preview"].push_back(snapshot["members"][i]);
                show_details(report);set(status,"Collected "+std::to_string(count)+(complete?" members. Database updated; ":" members (partial). Known members merged; ")+filename);
                show_collection_progress(window,cancel_collection?"Cancelled - partial members saved":complete?"Complete":"Partial members saved");
                if(batch_active)batch_results.push_back({{"server_id",sid},{"members_collected",count},{"complete",complete},{"expected_members",snapshot.value("expected_members",int64_t(-1))},{"coverage",snapshot.value("coverage_note",std::string())},{"status",cancel_collection?"cancelled":complete?"complete":"partial"}});
            }catch(const std::exception& e){error=e.what();show_details(Json{{"collection_error",error}});set(status,error);show_collection_progress(window,cancel_collection?"Cancelled":"Connection or collection failed");
                if(batch_active)batch_results.push_back({{"server_id",batch_queue[batch_index]},{"status",cancel_collection?"cancelled":"failed"},{"error",error}});
            }
            if(batch_active) {
                ++batch_index;
                bool stop=cancel_collection||close_requested||error.find("rate limit")!=std::string::npos||error.find("Session expired")!=std::string::npos||error.find("Gateway")!=std::string::npos||error.find("Discord invalidated")!=std::string::npos;
                if(!stop&&batch_index<batch_queue.size()) {
                    try{begin_collection(window,batch_queue[batch_index]);return 0;}
                    catch(const std::exception& e){batch_results.push_back({{"server_id",batch_queue[batch_index]},{"error",e.what()},{"status","not started"}});}
                }
                batch_active=false;
                Json report={{"servers_processed",batch_index},{"servers_selected",batch_queue.size()+batch_skipped},{"servers_skipped_recent",batch_skipped},{"servers_not_scanned",batch_queue.size()-batch_index},{"results",batch_results}};
                show_details(report);set(status,"Batch "+std::string(batch_index==batch_queue.size()?"finished":"stopped")+": "+std::to_string(batch_index)+" / "+std::to_string(batch_queue.size())+" servers processed; "+std::to_string(batch_skipped)+" recent servers skipped. See operation details.");
                show_collection_progress(window,cancel_collection?"Batch cancelled":batch_index==batch_queue.size()?"Batch finished":"Batch stopped");
            }
            if(close_requested)DestroyWindow(window);return 0;
        }
        case NetworkDone: {
            if(network_thread.joinable())network_thread.join();busy=false;EnableWindow(GetDlgItem(window,Connect),TRUE);
            std::vector<AccountResult> results;{std::lock_guard lock(completed_mutex);results=std::move(completed);}
            Json report=Json::array();size_t success=0;
            for(auto& result:results) {
                try {
                    if(!result.error.empty())throw std::runtime_error(result.error);
                    auto uid=result.snapshot["user_id"].get<std::string>();auto username=result.snapshot["username"].get<std::string>();
                    Json record={{"username",username},{"token",result.token}};auto next=remembered;
                    if(result.remember)next[uid]=record;else next.erase(uid);
                    save_sessions(workspace/L".auth"/L"sessions.bin",next);remembered=std::move(next);
                    database->apply_account(result.snapshot);sessions[uid]=std::move(record);
                    report.push_back({{"user_id",uid},{"username",username},{"servers",result.snapshot["guilds"].size()},{"checked_at",result.snapshot["captured_at"]}});
                    if(value(user).empty())set(user,uid);++success;
                }catch(const std::exception& e){report.push_back({{"error",e.what()}});}
                SecureZeroMemory(result.token.data(),result.token.size());
            }
            update_accounts(window);update_table();refresh_preview_if_present();show_details(report);
            set(status,std::to_string(success)+" account(s) refreshed; "+std::to_string(results.size()-success)+" failed. Connected-account coverage is not a full server roster.");
            if(close_requested)DestroyWindow(window);return 0;
        }
        case WM_CLOSE:if(audit_running){close_requested=true;cancel_collection=true;return 0;}if(chat_running){close_requested=true;cancel_collection=true;return 0;}if(busy){close_requested=true;if(collecting)cancel_active_collection(window);else set(status,"Finishing the server check before closing...");return 0;}break;
        case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);return 0;}
        case WM_MEASUREITEM: {
            auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if(item->CtlID==Accounts||item->CtlID==RemoveLeaves||item->CtlID==ScanAge||item->CtlID==ChatAccount||item->CtlID==ChatGuild||item->CtlID==AuditGuild||item->CtlID==AuditAccount){item->itemHeight=MulDiv(24,GetDpiForWindow(window),96);return TRUE;}break;
        }
        case WM_DRAWITEM: {
            auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if(item->CtlID==Tabs){FillRect(item->hDC,&item->rcItem,surface_brush);SetBkMode(item->hDC,TRANSPARENT);SetTextColor(item->hDC,item->itemID==static_cast<UINT>(TabCtrl_GetCurSel(item->hwndItem))?accent:ink);SelectObject(item->hDC,font);auto label=item->itemID<std::size(tab_labels)?tab_labels[item->itemID]:L"";DrawTextW(item->hDC,label,-1,&item->rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);return TRUE;}
            if(item->CtlID==Accounts||item->CtlID==RemoveLeaves||item->CtlID==ScanAge||item->CtlID==ChatAccount||item->CtlID==ChatGuild||item->CtlID==AuditGuild||item->CtlID==AuditAccount) {
                auto fill=CreateSolidBrush(item->itemState&ODS_SELECTED?RGB(39,70,110):surface);FillRect(item->hDC,&item->rcItem,fill);DeleteObject(fill);
                if(item->itemID!=static_cast<UINT>(-1)) {
                    int n=static_cast<int>(SendMessageW(item->hwndItem,CB_GETLBTEXTLEN,item->itemID,0));
                    if(n>=0){std::wstring label(n+1,L'\0');SendMessageW(item->hwndItem,CB_GETLBTEXT,item->itemID,reinterpret_cast<LPARAM>(label.data()));
                        RECT r=item->rcItem;r.left+=8;SetTextColor(item->hDC,ink);SetBkMode(item->hDC,TRANSPARENT);SelectObject(item->hDC,font);DrawTextW(item->hDC,label.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_LEFT|DT_END_ELLIPSIS);}
                }
                return TRUE;
            }break;
        }
        case WM_NOTIFY: {
            auto* info=reinterpret_cast<NMHDR*>(lp);
            if(info->idFrom==AuditTable||info->idFrom==AuditFilters){audit_notify(window,info);return 0;}
            if(info->idFrom==ChatChannels&&info->code==LVN_ITEMCHANGING&&busy&&!chat_rebuilding){auto change=reinterpret_cast<NMLISTVIEW*>(lp);if((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK)return TRUE;}
            if(info->idFrom==ChatMessages&&info->code==LVN_ITEMCHANGED){auto change=reinterpret_cast<NMLISTVIEW*>(lp);if(change->uNewState&LVIS_SELECTED)chat_message_detail(window);}
            if(info->idFrom==ChatMessages&&info->code==NM_DBLCLK){chat_message_context(window,reinterpret_cast<NMITEMACTIVATE*>(lp)->iItem);return 0;}
            if(info->idFrom==ChatChannels&&info->code==LVN_ITEMCHANGED&&!chat_rebuilding){auto change=reinterpret_cast<NMLISTVIEW*>(lp);if(change->uNewState&LVIS_SELECTED)chat_load_messages(window);}

            if(info->idFrom==Tabs&&info->code==TCN_SELCHANGE){switch_view(window);return 0;}
            if(info->hwndFrom==table&&info->code==LVN_COLUMNCLICK){auto col=reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;if(server_sort==col)server_desc=!server_desc;else{server_sort=col;server_desc=false;}update_table();return 0;}
            if(info->hwndFrom==member_table&&info->code==LVN_COLUMNCLICK){auto col=reinterpret_cast<NMLISTVIEW*>(lp)->iSubItem;if(member_sort==col)member_desc=!member_desc;else{member_sort=col;member_desc=false;}update_members(window,true);return 0;}
            if(info->hwndFrom==member_table&&info->code==LVN_ITEMCHANGED){auto change=reinterpret_cast<NMLISTVIEW*>(lp);if((change->uNewState&LVIS_SELECTED)&&change->iItem>=0&&static_cast<size_t>(change->iItem)<member_rows.size()){
                auto uid=member_rows[change->iItem]["user_id"].get<std::string>();std::string text="User "+uid+"\r\n";
                for(auto& row:database->member_servers(uid))text+=row["server_name"].get<std::string>()+" | "+row["server_id"].get<std::string>()+" | "+row["last_seen"].get<std::string>().substr(0,19)+" UTC | "+row["coverage"].get<std::string>()+"\r\n";
                set(GetDlgItem(window,MemberDetails),text);}return 0;}
            if(info->hwndFrom==table&&info->code==LVN_ITEMCHANGING&&busy&&!rebuilding_table&&!syncing_checks) {
                auto* change=reinterpret_cast<NMLISTVIEW*>(lp);if((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK)return TRUE;
            }
            if(info->hwndFrom==table&&info->code==LVN_ITEMCHANGED) {
                auto* change=reinterpret_cast<NMLISTVIEW*>(lp);
                if(!rebuilding_table&&!syncing_checks&&change->iItem>=0&&((change->uOldState^change->uNewState)&LVIS_STATEIMAGEMASK)) {
                    auto sid=server_id_at(change->iItem);if(!sid.empty()){if(ListView_GetCheckState(table,change->iItem))checked_servers.insert(sid);else checked_servers.erase(sid);sync_checks();}
                }
                if(!rebuilding_table&&!syncing_checks)update_collect(window);
            }
            if(info->code==NM_CUSTOMDRAW) {
                auto* draw=reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if(info->hwndFrom==ListView_GetHeader(table)) {
                    if(draw->dwDrawStage==CDDS_PREPAINT)return CDRF_NOTIFYITEMDRAW;
                    if(draw->dwDrawStage==CDDS_ITEMPREPAINT) {
                        FillRect(draw->hdc,&draw->rc,surface_brush);SetTextColor(draw->hdc,muted);SetBkMode(draw->hdc,TRANSPARENT);
                        wchar_t label[128]{};HDITEMW item{};item.mask=HDI_TEXT;item.pszText=label;item.cchTextMax=128;
                        Header_GetItem(info->hwndFrom,static_cast<int>(draw->dwItemSpec),&item);
                        RECT r=draw->rc;r.left+=8;SelectObject(draw->hdc,font);DrawTextW(draw->hdc,label,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                        return CDRF_SKIPDEFAULT;
                    }
                }
                wchar_t cls[32]{};GetClassNameW(info->hwndFrom,cls,32);
                if(std::wstring(cls)==L"Button"&&draw->dwDrawStage==CDDS_PREPAINT) {
                    const bool check=(GetWindowLongPtrW(info->hwndFrom,GWL_STYLE)&BS_TYPEMASK)==BS_AUTOCHECKBOX,enabled=IsWindowEnabled(info->hwndFrom)!=FALSE;
                    const COLORREF fill=check?bg:(draw->uItemState&CDIS_SELECTED)?RGB(39,70,110):(draw->uItemState&CDIS_HOT)?RGB(38,52,73):surface;
                    auto brush=CreateSolidBrush(fill);FillRect(draw->hdc,&draw->rc,brush);DeleteObject(brush);
                    RECT r=draw->rc;
                    if(check) {
                        RECT box{r.left+2,(r.top+r.bottom-16)/2,r.left+18,(r.top+r.bottom+16)/2};
                        auto border=CreateSolidBrush(accent);FrameRect(draw->hdc,&box,border);DeleteObject(border);
                        if(SendMessageW(info->hwndFrom,BM_GETCHECK,0,0)==BST_CHECKED) {
                            InflateRect(&box,-4,-4);auto mark=CreateSolidBrush(accent);FillRect(draw->hdc,&box,mark);DeleteObject(mark);
                        }
                        r.left+=26;
                    } else {auto border=CreateSolidBrush(RGB(49,65,87));FrameRect(draw->hdc,&r,border);DeleteObject(border);}
                    wchar_t label[128]{};GetWindowTextW(info->hwndFrom,label,128);SelectObject(draw->hdc,font);
                    SetTextColor(draw->hdc,enabled?ink:muted);SetBkMode(draw->hdc,TRANSPARENT);
                    DrawTextW(draw->hdc,label,-1,&r,(check?DT_LEFT:DT_CENTER)|DT_VCENTER|DT_SINGLELINE);
                    if(draw->uItemState&CDIS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(draw->hdc,&r);}
                    return CDRF_SKIPDEFAULT;
                }
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORBTN: {
            auto dc=reinterpret_cast<HDC>(wp);wchar_t cls[32]{};GetClassNameW(reinterpret_cast<HWND>(lp),cls,32);
            bool field=message==WM_CTLCOLOREDIT||message==WM_CTLCOLORLISTBOX||std::wstring(cls)==L"Edit"||std::wstring(cls)==L"ComboBox";
            SetTextColor(dc,ink);SetBkColor(dc,field?surface:bg);
            return reinterpret_cast<LRESULT>(field?surface_brush:background);
        }
        case WM_DESTROY:audit_stop_preview();KillTimer(window,2);PostQuitMessage(0);return 0;
        }
    }catch(const std::exception& e){
        if(status)set(status,"Error: "+std::string(e.what()));
        if(message!=WM_TIMER)MessageBoxW(window,wide(e.what()).c_str(),L"XA Discord Explorer",MB_OK|MB_ICONERROR);
        if(message==WM_CREATE)return -1;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);workspace=Path(executable).parent_path();
        int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
        for(int i=1;i<argc;++i)if(std::wstring(argv[i])==L"--workspace"&&i+1<argc)workspace=Path(argv[++i]);
        LocalFree(argv);std::filesystem::create_directories(workspace);
        Json config=Json::object();if(std::filesystem::exists(workspace/L"config.json"))config=read_json(workspace/L"config.json");
        db_path=Path(wide(config.value("database",std::string("data/members.sqlite3"))));if(db_path.is_relative())db_path=workspace/db_path;
        inbox=Path(wide(config.value("inbox",std::string("inbox"))));if(inbox.is_relative())inbox=workspace/inbox;
        max_age=config.value("max_age_hours",24.0);
        database=std::make_unique<Database>(db_path,max_age);
        remembered=load_sessions(workspace/L".auth"/L"sessions.bin");sessions=remembered;
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES|ICC_PROGRESS_CLASS|ICC_TAB_CLASSES};InitCommonControlsEx(&common);
        background=CreateSolidBrush(bg);surface_brush=CreateSolidBrush(surface);
        WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=instance;cls.lpszClassName=L"XADiscordExplorer";
        cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));cls.hbrBackground=background;
        RegisterClassW(&cls);
        HWND window=CreateWindowExW(0,cls.lpszClassName,L"XA Discord Explorer",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,1160,950,nullptr,nullptr,instance,nullptr);
        if(!window)throw std::runtime_error("Could not create application window");
        BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));
        SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED)));
        ShowWindow(window,show);UpdateWindow(window);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
        if(network_thread.joinable())network_thread.join();DeleteObject(font);DeleteObject(background);DeleteObject(surface_brush);return 0;
    }catch(const std::exception& e){
        try {std::filesystem::create_directories(workspace/L"logs");std::ofstream log(workspace/L"logs"/L"native-error.txt");log<<e.what()<<'\n';}catch(...){}
        MessageBoxW(nullptr,wide(e.what()).c_str(),L"XA Discord Explorer could not start",MB_OK|MB_ICONERROR);return 1;
    }
}
