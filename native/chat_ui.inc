// Included by app.cpp after the shared window helpers.
constexpr int ChatAccount=300,ChatGuild=301,ChatRefresh=302,ChatFrom=303,ChatSince=304,ChatDelete=305,ChatMax=306,ChatAll=307,ChatClear=308,ChatCollect=309,ChatCancel=310,ChatChannels=311,ChatMessages=312,ChatDetail=313,ChatSearch=314,ChatSuggestions=315,ChatDeleted=316,ChatPrev=317,ChatNext=318,ChatPage=319,ChatExport=320,ChatOpen=321,ChatKeep=322,ChatDismiss=323,ChatProgressLabel=324,ChatChannelOnly=325,ChatTemplates=326,ChatThreads=327,ChatChannelSearch=328,ChatChannelLabel=329;
constexpr UINT ChatDone=WM_APP+3;
constexpr UINT ChatLiveUpdated=WM_APP+4;
constexpr int ChatLive=336;
constexpr int ChatAuthor=334,ChatAuthorLabel=335;
constexpr int ChatControls[]={300,301,302,303,304,305,306,307,308,309,310,311,312,313,314,315,316,317,318,319,320,321,322,323,324,325,326,327,328,329,330,331,332,333,334,335,336,340,341,342,343,344,345,346};
std::unique_ptr<ChatStore> chat_db;
Json chat_channels=Json::array(),chat_messages=Json::array(),chat_server_rows=Json::array(),chat_result;
std::vector<std::string> chat_accounts;
std::set<std::string> chat_checked_channels;
std::string chat_selection_scope;
std::string chat_error,chat_progress;
bool chat_running=false,chat_listing=false,chat_rebuilding=false,chat_dm=false,chat_loading_options=false,chat_run_dm=false;
bool chat_live=false;
Path chat_db_path,chat_run_report;
HWND template_window{};
size_t chat_offset=0;int64_t chat_total=0;
std::chrono::steady_clock::time_point chat_started;
std::string chat_account(HWND window){auto i=SendMessageW(GetDlgItem(window,ChatAccount),CB_GETCURSEL,0,0);return i>=0&&static_cast<size_t>(i)<chat_accounts.size()?chat_accounts[i]:"";}
std::string chat_guild(HWND window){if(chat_dm)return chat_account(window).empty()?"":"@me";auto i=SendMessageW(GetDlgItem(window,ChatGuild),CB_GETCURSEL,0,0);return i>=0&&static_cast<size_t>(i)<chat_server_rows.size()?chat_server_rows[i]["server_id"].get<std::string>():"";}
Path chat_options_path(){return workspace/(chat_dm?L"chat-dm-options.json":L"chat-options.json");}
Path chat_database_path(const std::string& account,bool dm){if(!dm)return workspace/L"data"/L"messages.sqlite3";if(account.empty())return {};return workspace/L"data"/(L"messages-dm-"+wide(id(account))+L".sqlite3");}
Path chat_downloads_path(const std::string& account,bool dm){if(!dm)return workspace/L"downloads";if(account.empty())return {};return workspace/L"downloads"/(L"dm-"+wide(id(account)));}
void chat_select_store(HWND window){
    auto path=chat_database_path(chat_account(window),chat_dm);if(path==chat_db_path)return;
    if(template_window)DestroyWindow(template_window);
    chat_db.reset();chat_db_path.clear();chat_selection_scope.clear();chat_checked_channels.clear();chat_channels=Json::array();chat_messages=Json::array();chat_offset=0;chat_total=0;
    if(!path.empty()){chat_db=std::make_unique<ChatStore>(path);chat_db_path=path;}
}
bool checked(HWND window,int cid){return SendMessageW(GetDlgItem(window,cid),BM_GETCHECK,0,0)==BST_CHECKED;}
void chat_check(HWND window,int cid,bool selected){SendMessageW(GetDlgItem(window,cid),BM_SETCHECK,selected?BST_CHECKED:BST_UNCHECKED,0);}
std::string chat_author_filter(HWND window){auto text=value(GetDlgItem(window,ChatAuthor));if(text.empty())return {};try{return id(text);}catch(...){return "0";}}
void chat_save_options(HWND window){
    Json options={{"from_date",value(GetDlgItem(window,ChatFrom))},{"since",checked(window,ChatSince)},{"check_deletions",checked(window,ChatDelete)},{"include_threads",checked(window,ChatThreads)},{"max_mb",value(GetDlgItem(window,ChatMax))},{"types",Json::array()},{"account",chat_account(window)},{"guild",chat_guild(window)},{"author_id",value(GetDlgItem(window,ChatAuthor))}};
    options["message_search"]=value(GetDlgItem(window,ChatSearch));options["channel_search"]=value(GetDlgItem(window,ChatChannelSearch));options["suggestions"]=checked(window,ChatSuggestions);options["deleted_only"]=checked(window,ChatDeleted);options["selected_only"]=checked(window,ChatChannelOnly);
    for(int i=0;i<7;++i)options["types"].push_back(checked(window,340+i));write_json(chat_options_path(),options);
}
void chat_load_options(HWND window){
    Json o=Json::object();try{o=read_json(chat_options_path());if(!o.is_object())o=Json::object();}catch(...){}
    chat_loading_options=true;
    try{set(GetDlgItem(window,ChatFrom),o.value("from_date",stamp(now()-30LL*86400000000LL).substr(0,10)));chat_check(window,ChatSince,o.value("since",true));chat_check(window,ChatDelete,o.value("check_deletions",false));chat_check(window,ChatThreads,o.value("include_threads",false));set(GetDlgItem(window,ChatMax),o.value("max_mb",std::string("10")));set(GetDlgItem(window,ChatAuthor),o.value("author_id",std::string()));
        for(int i=0;i<7;++i)chat_check(window,340+i,o.contains("types")&&o["types"].is_array()&&o["types"].size()==7?o["types"][i].get<bool>():true);
        set(GetDlgItem(window,ChatSearch),o.value("message_search",std::string()));set(GetDlgItem(window,ChatChannelSearch),o.value("channel_search",std::string()));chat_check(window,ChatSuggestions,o.value("suggestions",false));chat_check(window,ChatDeleted,o.value("deleted_only",false));chat_check(window,ChatChannelOnly,o.value("selected_only",false));
    }catch(...){}
    chat_loading_options=false;
}
void chat_update_enabled(HWND window){for(int cid:{ChatAccount,ChatGuild,ChatRefresh,ChatAuthor,ChatDelete,ChatMax,ChatThreads,ChatAll,ChatClear,ChatCollect})EnableWindow(GetDlgItem(window,cid),!busy);bool author=!value(GetDlgItem(window,ChatAuthor)).empty();bool available=!chat_dm||!chat_account(window).empty();EnableWindow(GetDlgItem(window,ChatFrom),!busy&&(chat_dm||!author||checked(window,ChatDelete)));EnableWindow(GetDlgItem(window,ChatSince),!busy&&!author);EnableWindow(GetDlgItem(window,ChatGuild),!busy&&!chat_dm);EnableWindow(GetDlgItem(window,ChatThreads),!busy&&!chat_dm);for(int cid:{ChatRefresh,ChatCollect,ChatTemplates,ChatExport,ChatOpen})EnableWindow(GetDlgItem(window,cid),!busy&&available);set(GetDlgItem(window,ChatCollect),author?"Collect user messages":chat_dm?"Collect DMs":"Collect messages");EnableWindow(GetDlgItem(window,ChatCancel),chat_running&&!cancel_collection);EnableWindow(GetDlgItem(window,ChatLive),chat_live?!cancel_collection:(!busy&&!chat_dm&&!author&&available));set(GetDlgItem(window,ChatLive),chat_live?"Stop capture":"Live capture");EnableWindow(GetDlgItem(window,ChatChannels),TRUE);EnableWindow(GetDlgItem(window,ChatChannelSearch),!chat_live);for(int cid=340;cid<=346;++cid)EnableWindow(GetDlgItem(window,cid),!chat_live);}
void chat_fill_combos(HWND window,bool restore=false){
    auto account=restore?std::string():chat_account(window),guild=restore?std::string():chat_guild(window);if(account.empty()||guild.empty())try{auto o=read_json(chat_options_path());if(account.empty())account=o.value("account",std::string());if(guild.empty())guild=o.value("guild",std::string());}catch(...){}
    HWND accounts=GetDlgItem(window,ChatAccount);SendMessageW(accounts,CB_RESETCONTENT,0,0);chat_accounts.clear();int chosen=0;
    for(auto& [uid,record]:sessions.items()){int i=static_cast<int>(chat_accounts.size());chat_accounts.push_back(uid);auto label=wide(record["username"].get<std::string>()+" ("+uid+")");SendMessageW(accounts,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(uid==account)chosen=i;}if(!chat_accounts.empty())SendMessageW(accounts,CB_SETCURSEL,chosen,0);
    auto combo=GetDlgItem(window,ChatGuild);SendMessageW(combo,CB_RESETCONTENT,0,0);chat_server_rows=database->servers();chosen=0;for(size_t i=0;i<chat_server_rows.size();++i){auto& row=chat_server_rows[i];auto label=wide(row["server_name"].get<std::string>()+" ("+row["server_id"].get<std::string>()+")");SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));if(row["server_id"]==guild)chosen=static_cast<int>(i);}if(!chat_server_rows.empty())SendMessageW(combo,CB_SETCURSEL,chosen,0);
}
void chat_sync_channel_checks(HWND window){
    auto list=GetDlgItem(window,ChatChannels);
    for(int i=0;i<ListView_GetItemCount(list)&&static_cast<size_t>(i)<chat_channels.size();++i){auto id=chat_channels[i]["id"].get<std::string>();if(ListView_GetCheckState(list,i))chat_checked_channels.insert(id);else chat_checked_channels.erase(id);}
}
std::vector<std::string> chat_collection_channels(HWND window){
    chat_select_store(window);chat_sync_channel_checks(window);std::vector<std::string> selected;if(!chat_db)return selected;
    for(auto& row:chat_db->channels(chat_guild(window),chat_account(window))){int type=row["type"].get<int>();if(type==15||type==16||(!checked(window,ChatThreads)&&(type==10||type==11||type==12)))continue;auto id=row["id"].get<std::string>();if(chat_checked_channels.contains(id))selected.push_back(id);}
    return selected;
}
void chat_load_channels(HWND window){
    chat_select_store(window);
    auto guild=chat_guild(window);auto account=chat_account(window);auto list=GetDlgItem(window,ChatChannels);
    auto scope=account+":"+guild;std::string focused;
    if(scope==chat_selection_scope){chat_sync_channel_checks(window);int i=ListView_GetNextItem(list,-1,LVNI_SELECTED);if(i>=0&&static_cast<size_t>(i)<chat_channels.size())focused=chat_channels[i]["id"].get<std::string>();}else{chat_checked_channels.clear();chat_selection_scope=scope;}
    auto lower=[](std::wstring text){for(auto& c:text)c=static_cast<wchar_t>(std::towlower(c));return text;};auto query=lower(wide(value(GetDlgItem(window,ChatChannelSearch))));
    chat_rebuilding=true;chat_channels=Json::array();
    if(chat_db)for(auto& row:chat_db->channels(guild,account)){int type=row["type"].get<int>();if(!checked(window,ChatThreads)&&(type==10||type==11||type==12))continue;if(!query.empty()&&lower(wide(row["name"])).find(query)==std::wstring::npos&&wide(row["id"]).find(query)==std::wstring::npos)continue;chat_channels.push_back(row);}
    ListView_DeleteAllItems(list);
    for(size_t i=0;i<chat_channels.size();++i){auto& row=chat_channels[i];int type=row["type"].get<int>();std::wstring cols[]={wide(row["name"]),wide(row["id"]),type==1?L"DM":type==3?L"Group DM":type==15||type==16?L"Container":type==10||type==11||type==12?L"Thread":L"Channel",wide(row["checked"].get<std::string>().empty()?"Never":row["checked"].get<std::string>().substr(0,19)+" UTC"),wide(std::to_string(row["messages"].get<int64_t>()))};LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=cols[0].data();ListView_InsertItem(list,&item);for(int c=1;c<5;++c)ListView_SetItemText(list,item.iItem,c,cols[c].data());ListView_SetCheckState(list,item.iItem,chat_checked_channels.contains(row["id"].get<std::string>()));if(row["id"]==focused)ListView_SetItemState(list,item.iItem,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}chat_rebuilding=false;
}
std::string chat_selected_channel(HWND window){if(!checked(window,ChatChannelOnly))return {};int i=ListView_GetNextItem(GetDlgItem(window,ChatChannels),-1,LVNI_SELECTED);return i>=0&&static_cast<size_t>(i)<chat_channels.size()?chat_channels[i]["id"].get<std::string>():"";}
void chat_load_messages(HWND window,bool reset=true){
    if(reset)chat_offset=0;auto data=chat_db?chat_db->messages(chat_guild(window),chat_selected_channel(window),value(GetDlgItem(window,ChatSearch)),checked(window,ChatSuggestions),checked(window,ChatDeleted),chat_offset,chat_author_filter(window)):Json{{"total",0},{"rows",Json::array()}};chat_total=data["total"].get<int64_t>();chat_messages=std::move(data["rows"]);
    auto list=GetDlgItem(window,ChatMessages);ListView_DeleteAllItems(list);
    for(size_t i=0;i<chat_messages.size();++i){auto& row=chat_messages[i];auto text=row["content"].get<std::string>();std::replace(text.begin(),text.end(),'\n',' ');std::replace(text.begin(),text.end(),'\r',' ');std::wstring cols[]={wide(row["sent"].get<std::string>().substr(0,19)),wide(row["username"]),wide(text),wide(row["deleted"].get<bool>()?"Deleted":row["suggestion"].get<std::string>().empty()?"Message":row["review"].get<std::string>()+" suggestion")};LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=cols[0].data();ListView_InsertItem(list,&item);for(int c=1;c<4;++c)ListView_SetItemText(list,item.iItem,c,cols[c].data());}
    set(GetDlgItem(window,ChatPage),std::to_string(chat_total)+" messages | Page "+std::to_string(chat_offset/100+1)+" / "+std::to_string(std::max(int64_t(1),(chat_total+99)/100)));
    EnableWindow(GetDlgItem(window,ChatPrev),chat_offset>0);EnableWindow(GetDlgItem(window,ChatNext),chat_offset+100<static_cast<size_t>(chat_total));set(GetDlgItem(window,ChatDetail),"Select a saved message to view text, attachments and its Discord link.");
}
std::string chat_selected_message(HWND window){int i=ListView_GetNextItem(GetDlgItem(window,ChatMessages),-1,LVNI_SELECTED);return i>=0&&static_cast<size_t>(i)<chat_messages.size()?chat_messages[i]["id"].get<std::string>():"";}
void chat_message_detail(HWND window){auto mid=chat_selected_message(window);if(mid.empty()||!chat_db)return;auto data=chat_db->message(mid);if(data.is_null())return;auto& m=data["message"];std::string text=m["author"].value("username",std::string())+" | "+m["timestamp"].get<std::string>()+" | "+(data["deleted"].get<bool>()?"Deleted on Discord; saved copy":"Saved message")+"\r\nhttps://discord.com/channels/"+data["guild"].get<std::string>()+"/"+m["channel_id"].get<std::string>()+"/"+mid+"\r\n";
    if(!data["suggestion"].get<std::string>().empty())text+="Suggestion match: "+data["suggestion"].get<std::string>()+" | Review: "+data["review"].get<std::string>()+"\r\n";
    text+="\r\n"+chat_message_text(m);for(auto& a:chat_message_attachments(m))text+="\r\nAttachment: "+a.value("filename",std::string())+" ("+std::to_string(a.value("size",int64_t(0)))+" bytes)";
    for(auto& f:data["files"])text+="\r\n"+f["status"].get<std::string>()+": "+f["path"].get<std::string>();std::string formatted;for(size_t i=0;i<text.size();++i){if(text[i]=='\n'&&(i==0||text[i-1]!='\r'))formatted+='\r';formatted+=text[i];}set(GetDlgItem(window,ChatDetail),formatted);
}
void chat_message_context(HWND window,int row){
    if(!chat_db||row<0||static_cast<size_t>(row)>=chat_messages.size())return;
    // Copy the clicked ID before clearing filters: edit/selection notifications rebuild the rows.
    auto mid=chat_messages[row]["id"].get<std::string>();auto guild=chat_guild(window);
    auto location=chat_db->message_location(guild,mid);
    if(location.is_null()){set(status,"This message is no longer in the saved history.");return;}
    auto channel=location["channel"].get<std::string>();Json channel_row;
    for(auto& candidate:chat_db->channels(guild,chat_account(window)))if(candidate["id"]==channel){channel_row=candidate;break;}
    if(channel_row.is_null()){set(status,chat_dm?"The saved conversation is unavailable. Refresh DMs and try again.":"The saved channel is unavailable. Refresh channels and try again.");return;}
    int type=channel_row["type"].get<int>();if(type==10||type==11||type==12)chat_check(window,ChatThreads,true);
    chat_check(window,ChatSuggestions,false);chat_check(window,ChatDeleted,false);
    set(GetDlgItem(window,ChatAuthor),"");
    set(GetDlgItem(window,ChatSearch),"");
    if(!value(GetDlgItem(window,ChatChannelSearch)).empty())set(GetDlgItem(window,ChatChannelSearch),"");
    chat_load_channels(window);chat_check(window,ChatChannelOnly,true);
    auto channels=GetDlgItem(window,ChatChannels);
    for(size_t i=0;i<chat_channels.size();++i)if(chat_channels[i]["id"]==channel){
        ListView_SetItemState(channels,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(channels,static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_EnsureVisible(channels,static_cast<int>(i),FALSE);break;
    }
    chat_offset=(location["position"].get<size_t>()/100)*100;chat_load_messages(window,false);
    auto messages=GetDlgItem(window,ChatMessages);
    for(size_t i=0;i<chat_messages.size();++i)if(chat_messages[i]["id"]==mid){
        ListView_SetItemState(messages,static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_EnsureVisible(messages,static_cast<int>(i),FALSE);SetFocus(messages);chat_message_detail(window);
        set(status,std::string("Showing conversation in ")+(chat_dm?"":"#")+channel_row["name"].get<std::string>()+" | Page "+std::to_string(chat_offset/100+1));return;
    }
    set(status,"Saved history changed while opening the conversation. Search for the message and try again.");
}
void chat_tick(HWND window){std::string text;{std::lock_guard lock(completed_mutex);text=chat_progress;}auto seconds=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-chat_started).count();set(GetDlgItem(window,ChatProgressLabel),"Elapsed "+std::to_string(seconds/60)+":"+(seconds%60<10?"0":"")+std::to_string(seconds%60)+" | "+text);}
void chat_begin(HWND window,bool listing,bool live=false){
    if(busy)return;auto account=chat_account(window),guild=chat_guild(window);if(account.empty()||guild.empty())throw std::runtime_error(chat_dm?"Select a connected account; connect accounts in View Servers":"Select a connected account and server; connect accounts in View Servers");
    ChatOptions options;auto author=value(GetDlgItem(window,ChatAuthor));if(!listing&&!author.empty())options.author_id=id(author);options.from_date=value(GetDlgItem(window,ChatFrom));options.since=checked(window,ChatSince);options.check_deletions=checked(window,ChatDelete);if(live){options.from_date.clear();options.author_id.clear();options.since=true;options.check_deletions=false;options.live_from=chat_time_id(now());}if(!listing)chat_from_id(options,chat_dm);auto max=value(GetDlgItem(window,ChatMax));size_t consumed=0;auto mb=std::stoll(max,&consumed);if(consumed!=max.size()||mb<1||mb>1024)throw std::runtime_error("Max file size must be 1 to 1024 MB");options.max_bytes=mb*1024*1024;options.types.clear();const char* types[]={"pictures","zip","txt","lua","json","py","files"};for(int i=0;i<7;++i)if(checked(window,340+i))options.types.insert(types[i]);
    auto selected=chat_collection_channels(window);
    if(!listing&&selected.empty())throw std::runtime_error(chat_dm?"Check at least one DM or group DM":"Check at least one channel or thread; forum containers themselves have no message history");
    auto store_path=chat_database_path(account,chat_dm),download_root=chat_downloads_path(account,chat_dm);chat_run_dm=chat_dm;chat_run_report=workspace/L"reports"/(chat_run_dm?L"last-dm-collection-"+wide(account)+L".json":L"last-message-collection.json");
    chat_save_options(window);auto token=sessions.at(account).at("token").get<std::string>();if(network_thread.joinable())network_thread.join();busy=collecting=chat_running=true;chat_listing=listing;chat_live=live;cancel_collection=false;chat_started=std::chrono::steady_clock::now();{std::lock_guard lock(completed_mutex);chat_progress=listing?(chat_dm?"Loading DMs and group DMs":"Loading channels and threads"):"Starting message collection";chat_error.clear();chat_result=Json();}
    chat_update_enabled(window);update_accounts(window);SetTimer(window,3,250,nullptr);chat_tick(window);
    network_thread=std::thread([window,token=std::move(token),account,guild,selected=std::move(selected),options,listing,live,store_path,download_root,dm=chat_dm,include_threads=checked(window,ChatThreads)]()mutable{
        Json result;std::string error;try{auto progress=[](const std::string& text){std::lock_guard lock(completed_mutex);chat_progress=text;};ChatClient client(token,cancel_collection,progress);ChatGet get=[&](const std::string& route){return client.get(route);};ChatStore db(store_path);
            if(listing){result=dm?list_dm_channels(get,cancel_collection,progress):list_chat_channels(get,guild,cancel_collection,progress,include_threads);if(!cancel_collection)db.channels(guild,result.at("channels"));}
            else {
                int64_t captured=0;
                do {
                    if(cancel_collection)break;
                    result=collect_chat(db,get,[&](const std::string& url,const Path& path,int64_t max_bytes){client.download(url,path,max_bytes);},download_root,account,guild,selected,options,cancel_collection,progress);
                    if(!live)break;
                    int64_t received=0;int failed=0;std::string failure;
                    for(auto& row:result["results"]){received+=row.value("messages_received",int64_t(0));if(row["status"]!="complete"){++failed;failure=row.value("error",std::string());if(failure.find("Session expired")!=std::string::npos)throw std::runtime_error(failure);}}
                    captured+=received;result["live_capture"]=true;result["session_messages_received"]=captured;
                    {std::lock_guard lock(completed_mutex);chat_result=result;}
                    if(received)PostMessageW(window,ChatLiveUpdated,0,0);
                    if(cancel_collection)break;
                    progress("Live capture | "+std::to_string(captured)+" new messages | "+std::to_string(selected.size())+" channels | "+(failed?std::to_string(failed)+" incomplete; retrying in 30 seconds: "+failure:"Watching; next check in 5 seconds"));
                    auto next=std::chrono::steady_clock::now()+std::chrono::seconds(failed?30:5);
                    while(!cancel_collection&&std::chrono::steady_clock::now()<next)std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }while(live&&!cancel_collection);
            }
        }catch(const std::exception& e){error=e.what();}SecureZeroMemory(token.data(),token.size());{std::lock_guard lock(completed_mutex);chat_result=std::move(result);chat_error=error;}PostMessageW(window,ChatDone,0,0);
    });
}
void chat_live_refresh(HWND window){
    if(!chat_live)return;
    auto selected=chat_selected_message(window);chat_load_channels(window);chat_load_messages(window,false);
    if(!selected.empty())for(size_t i=0;i<chat_messages.size();++i)if(chat_messages[i]["id"]==selected){ListView_SetItemState(GetDlgItem(window,ChatMessages),static_cast<int>(i),LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);chat_message_detail(window);break;}
}
void chat_finish(HWND window){if(network_thread.joinable())network_thread.join();KillTimer(window,3);bool was_live=chat_live;busy=collecting=chat_running=chat_live=false;Json result;std::string error;{std::lock_guard lock(completed_mutex);result=chat_result;error=chat_error;}update_accounts(window);chat_update_enabled(window);chat_load_channels(window);chat_load_messages(window);std::string outcome;
    if(!error.empty())outcome="Stopped: "+error;else if(cancel_collection)outcome="Cancelled; saved messages kept";else if(chat_listing)outcome="Loaded "+std::to_string(result["channels"].size())+(chat_run_dm?" DMs/group DMs; ":" channels/threads; ")+std::to_string(result["warnings"].size())+" discovery warnings";else{int64_t total=0,failed=0;for(auto& row:result["results"]){total+=row.value("messages_received",int64_t(0));if(row["status"]!="complete")++failed;}outcome="Finished: "+std::to_string(total)+" messages received; "+std::to_string(failed)+(chat_run_dm?" conversations incomplete":" channels incomplete");}
    if(was_live&&error.empty())outcome="Live capture stopped; saved messages kept";
    set(GetDlgItem(window,ChatProgressLabel),outcome);set(status,outcome);if(!result.is_null()){write_json(chat_run_report,result);set(GetDlgItem(window,ChatDetail),result.dump(2));}else set(GetDlgItem(window,ChatDetail),error);if(close_requested)DestroyWindow(window);
}
void chat_export(HWND window){
    auto guild=chat_guild(window);if(guild.empty()||!chat_db)return;auto channel=chat_selected_channel(window);auto author=value(GetDlgItem(window,ChatAuthor));if(!author.empty())author=id(author);
    auto base=workspace/L"reports"/(L"messages-"+(chat_dm?L"dm-"+wide(id(chat_account(window))):wide(guild))+(author.empty()?L"":L"-user-"+wide(author))+(checked(window,ChatSuggestions)?L"-suggestions":L""));std::filesystem::create_directories(base.parent_path());auto jp=base;jp+=L".json";auto tp=base;tp+=L".txt";std::ofstream json(jp,std::ios::binary),txt(tp,std::ios::binary);if(!json||!txt)throw std::runtime_error("Could not create exports");json<<"[\n";size_t count=0;
    for(size_t offset=0;;offset+=100){auto page=chat_db->messages(guild,channel,value(GetDlgItem(window,ChatSearch)),checked(window,ChatSuggestions),checked(window,ChatDeleted),offset,author);
        for(auto& item:page["rows"]){auto row=chat_db->message(item["id"]);auto& m=row["message"];if(count++)json<<",\n";json<<row.dump(2);txt<<m.value("timestamp",std::string())<<" | "<<m["author"].value("username",std::string())<<(row["deleted"].get<bool>()?" [DELETED]":"")<<"\n"<<chat_message_text(m)<<"\nhttps://discord.com/channels/"<<row["guild"].get<std::string>()<<"/"<<m["channel_id"].get<std::string>()<<"/"<<m["id"].get<std::string>()<<"\n\n";}
        if(offset+100>=page["total"].get<size_t>())break;
    }
    json<<"\n]\n";json.flush();txt.flush();if(!json||!txt)throw std::runtime_error("Message export failed; check disk space");set(status,"Exported "+std::to_string(count)+" messages as JSON and TXT in reports");
}

LRESULT CALLBACK template_proc(HWND window,UINT msg,WPARAM wp,LPARAM lp){
    try{switch(msg){
    case WM_CREATE:{control(window,L"STATIC",L"DevHub templates: phrase, kind (suggestion/bug), weight. ^ anchors to start. Score >= 0.5 is a candidate.",0,700);control(window,L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_AUTOHSCROLL|ES_WANTRETURN|WS_VSCROLL|WS_HSCROLL|WS_TABSTOP,701);SendMessageW(GetDlgItem(window,701),EM_SETLIMITTEXT,200000,0);control(window,L"BUTTON",L"Save + rescan saved messages",WS_TABSTOP,702);control(window,L"BUTTON",L"Reset to defaults",WS_TABSTOP,703);control(window,L"BUTTON",L"Close",WS_TABSTOP,704);auto text=chat_db->detection_patterns().dump(2);std::string formatted;for(char c:text){if(c=='\n')formatted+='\r';formatted+=c;}set(GetDlgItem(window,701),formatted);return 0;}
    case WM_SIZE:{RECT r{};GetClientRect(window,&r);MoveWindow(GetDlgItem(window,700),16,14,r.right-32,40,TRUE);MoveWindow(GetDlgItem(window,701),16,60,r.right-32,std::max(100L,r.bottom-115),TRUE);MoveWindow(GetDlgItem(window,702),16,r.bottom-42,260,28,TRUE);MoveWindow(GetDlgItem(window,703),290,r.bottom-42,155,28,TRUE);MoveWindow(GetDlgItem(window,704),460,r.bottom-42,80,28,TRUE);return 0;}
    case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED){if(LOWORD(wp)==704){DestroyWindow(window);return 0;}if(LOWORD(wp)==703){auto text=default_detection_patterns().dump(2);std::string formatted;for(char c:text){if(c=='\n')formatted+='\r';formatted+=c;}set(GetDlgItem(window,701),formatted);return 0;}if(LOWORD(wp)==702){if(busy)throw std::runtime_error("Wait for the current collection to finish before rescanning templates");auto patterns=Json::parse(value(GetDlgItem(window,701)));chat_db->set_detection_patterns(patterns);chat_load_messages(GetParent(window));set(status,"Detection templates saved; saved messages rescanned");return 0;}}break;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:case WM_NOTIFY:return SendMessageW(GetParent(window),msg,wp,lp);
    case WM_DESTROY:template_window=nullptr;return 0;
    }}catch(const std::exception& e){MessageBoxW(window,wide(e.what()).c_str(),L"Detection templates",MB_OK|MB_ICONERROR);return 0;}return DefWindowProcW(window,msg,wp,lp);
}
void chat_templates(HWND owner){if(!chat_db)return;if(template_window){SetForegroundWindow(template_window);return;}WNDCLASSW cls{};cls.lpfnWndProc=template_proc;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hbrBackground=background;cls.lpszClassName=L"SmlDetectionTemplates";RegisterClassW(&cls);template_window=CreateWindowExW(0,cls.lpszClassName,L"Detection templates",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,850,650,owner,nullptr,cls.hInstance,nullptr);BOOL dark=TRUE;DwmSetWindowAttribute(template_window,20,&dark,sizeof(dark));ShowWindow(template_window,SW_SHOW);}
bool chat_command(HWND window,int cid,int notification){
    if(cid<300||cid>346)return false;
    if(chat_loading_options)return true;
    if((cid==ChatAccount||cid==ChatGuild)&&notification==CBN_SELCHANGE){if(chat_running)return true;chat_load_channels(window);chat_load_messages(window);chat_update_enabled(window);chat_save_options(window);return true;}
    if(cid==ChatAuthor&&notification==EN_CHANGE){chat_update_enabled(window);chat_load_messages(window);if(!busy){auto author=chat_author_filter(window);set(GetDlgItem(window,ChatProgressLabel),author.empty()?(chat_dm?"Check DMs and choose From date, or leave it blank for all dates.":"Check channels, then collect messages. User ID is optional."):author=="0"?"Enter a valid Discord user ID or member mention.":"User "+author+(chat_dm?": collect from checked DMs using From date; blank means all dates.":": collect full history from checked channels; attachment filters apply."));}return true;}
    if(cid==ChatAuthor&&notification==EN_KILLFOCUS){chat_save_options(window);return true;}
    if(cid==ChatChannelSearch&&notification==EN_CHANGE){chat_load_channels(window);chat_load_messages(window);return true;}
    if(cid==ChatSearch&&notification==EN_CHANGE){chat_load_messages(window);return true;}if(notification!=BN_CLICKED)return true;
    switch(cid){case ChatRefresh:chat_begin(window,true);break;case ChatCollect:chat_begin(window,false);break;case ChatCancel:cancel_collection=true;EnableWindow(GetDlgItem(window,ChatCancel),FALSE);break;
    case ChatDelete:chat_update_enabled(window);chat_save_options(window);break;
    case ChatLive:if(chat_live){cancel_collection=true;chat_update_enabled(window);}else chat_begin(window,false,true);break;
    case ChatThreads:chat_load_channels(window);chat_save_options(window);break;
    case ChatAll:case ChatClear:if(!busy){if(cid==ChatClear)chat_checked_channels.clear();for(int i=0;i<ListView_GetItemCount(GetDlgItem(window,ChatChannels));++i){int type=chat_channels[i]["type"].get<int>();ListView_SetCheckState(GetDlgItem(window,ChatChannels),i,cid==ChatAll&&type!=15&&type!=16);}chat_sync_channel_checks(window);}break;
    case ChatSuggestions:case ChatDeleted:case ChatChannelOnly:chat_load_messages(window);break;
    case ChatPrev:if(chat_offset>=100){chat_offset-=100;chat_load_messages(window,false);}break;case ChatNext:if(chat_offset+100<static_cast<size_t>(chat_total)){chat_offset+=100;chat_load_messages(window,false);}break;
    case ChatKeep:case ChatDismiss:{auto mid=chat_selected_message(window);if(!mid.empty()){chat_db->review(mid,cid==ChatKeep?"Keep":"Dismiss");chat_message_detail(window);}}break;
    case ChatTemplates:chat_templates(window);break;
    case ChatExport:chat_export(window);break;case ChatOpen:{auto path=chat_downloads_path(chat_account(window),chat_dm);if(!path.empty()){std::filesystem::create_directories(path);ShellExecuteW(window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);}}break;
    default:break;}return true;
}
void chat_layout(HWND window,int w,int h,double scale){auto move=[&](int cid,int x,int y,int cw,int ch){MoveWindow(GetDlgItem(window,cid),static_cast<int>(x*scale),static_cast<int>(y*scale),static_cast<int>(cw*scale),static_cast<int>(ch*scale),TRUE);};
    move(330,24,59,64,22);move(ChatAccount,90,54,chat_dm?w-306:270,240);move(331,375,59,52,22);move(ChatGuild,432,54,w-636,300);move(ChatRefresh,w-192,53,168,30);
    move(332,24,96,105,22);move(ChatFrom,132,91,125,28);move(ChatSince,274,91,128,28);move(ChatDelete,420,91,165,28);move(333,600,96,110,22);move(ChatMax,710,91,60,28);move(ChatThreads,790,91,155,28);
    int x=24;int widths[]={100,65,65,65,70,65,140};for(int i=0;i<7;++i){move(340+i,x,128,widths[i],26);x+=widths[i]+8;}
    move(ChatAuthorLabel,660,133,65,22);move(ChatAuthor,730,128,w-754,28);
    move(ChatAll,24,164,110,30);move(ChatClear,144,164,110,30);move(ChatCollect,264,164,180,30);move(ChatCancel,454,164,100,30);move(ChatOpen,564,164,155,30);move(ChatExport,729,164,170,30);move(ChatLive,w-174,204,150,28);
    move(ChatChannelLabel,24,209,115,22);move(ChatChannelSearch,145,204,chat_dm?w-169:w-337,28);
    move(ChatChannels,24,240,w-48,114);move(ChatProgressLabel,24,360,w-48,24);
    move(ChatSearch,24,393,285,28);move(ChatSuggestions,325,393,180,28);move(ChatDeleted,520,393,135,28);move(ChatChannelOnly,670,393,190,28);move(ChatTemplates,875,393,220,28);
    int message_h=std::max(105,h-661);move(ChatMessages,24,430,w-48,message_h);int y=440+message_h;
    move(ChatPrev,24,y,80,28);move(ChatNext,114,y,80,28);move(ChatPage,210,y+4,300,22);move(ChatKeep,530,y,135,28);move(ChatDismiss,675,y,145,28);move(ChatDetail,24,y+38,w-48,std::max(80,h-y-104));
    int cw[]={std::max(150,w-48-600),175,90,240,95};for(int i=0;i<5;++i)ListView_SetColumnWidth(GetDlgItem(window,ChatChannels),i,static_cast<int>(cw[i]*scale));int mw[]={180,150,std::max(200,w-48-495),165};for(int i=0;i<4;++i)ListView_SetColumnWidth(GetDlgItem(window,ChatMessages),i,static_cast<int>(mw[i]*scale));
}
void chat_create(HWND window){chat_db_path=chat_database_path("",false);chat_db=std::make_unique<ChatStore>(chat_db_path);
    control(window,L"STATIC",L"Find channel",0,ChatChannelLabel);auto channel_search=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ChatChannelSearch);SendMessageW(channel_search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search channel name or ID"));SendMessageW(channel_search,EM_SETLIMITTEXT,256,0);
    control(window,L"STATIC",L"Account",0,330);control(window,L"STATIC",L"Server",0,331);control(window,L"STATIC",L"From date (UTC)",0,332);control(window,L"STATIC",L"Max file MB",0,333);
    control(window,L"STATIC",L"User ID",0,ChatAuthorLabel);auto author=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ChatAuthor);SendMessageW(author,EM_SETLIMITTEXT,64,0);SendMessageW(author,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Optional - collect all dates"));
    for(int cid:{ChatAccount,ChatGuild}){auto combo=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_TABSTOP|WS_VSCROLL,cid);SetWindowSubclass(combo,dark_control,2,0);}
    for(auto item:std::vector<std::pair<int,const wchar_t*>>{{ChatLive,L"Live capture"},{ChatTemplates,L"Detection templates"},{ChatRefresh,L"Refresh channels"},{ChatAll,L"Select all"},{ChatClear,L"Clear all"},{ChatCollect,L"Collect messages"},{ChatCancel,L"Cancel"},{ChatOpen,L"Open downloads"},{ChatExport,L"Export JSON + TXT"},{ChatPrev,L"Previous"},{ChatNext,L"Next"},{ChatKeep,L"Keep suggestion"},{ChatDismiss,L"Dismiss suggestion"}})control(window,L"BUTTON",item.second,WS_TABSTOP,item.first);
    for(auto item:std::vector<std::pair<int,const wchar_t*>>{{ChatThreads,L"Include threads"},{ChatSince,L"Check since"},{ChatDelete,L"Check for deletion"},{ChatSuggestions,L"Suggestions / bugs"},{ChatDeleted,L"Deleted only"},{ChatChannelOnly,L"Selected channel only"},{340,L"Pictures"},{341,L"ZIP"},{342,L"TXT"},{343,L"LUA"},{344,L"JSON"},{345,L"PY"},{346,L"Other files"}})control(window,L"BUTTON",item.second,BS_AUTOCHECKBOX|WS_TABSTOP,item.first);
    control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ChatFrom);control(window,L"EDIT",L"10",ES_NUMBER|ES_AUTOHSCROLL|WS_TABSTOP,ChatMax);auto search=control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,ChatSearch);SendMessageW(search,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Search saved text, author or message ID"));
    control(window,L"STATIC",L"Blank From date resumes saved messages. Deletion checks require a date.",0,ChatProgressLabel);control(window,L"STATIC",L"",0,ChatPage);control(window,L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,ChatDetail);
    for(int cid:{ChatChannels,ChatMessages}){auto list=control(window,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|WS_BORDER|WS_TABSTOP,cid);ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|(cid==ChatChannels?LVS_EX_CHECKBOXES:0));ListView_SetBkColor(list,surface);ListView_SetTextBkColor(list,surface);ListView_SetTextColor(list,ink);SetWindowTheme(ListView_GetHeader(list),L"",L"");SetWindowSubclass(ListView_GetHeader(list),dark_control,1,0);int i=0;for(auto title:cid==ChatChannels?std::vector<const wchar_t*>{L"Channel / thread",L"Channel ID",L"Type",L"Last checked (UTC)",L"Saved"}:std::vector<const wchar_t*>{L"Date (UTC)",L"Author",L"Message",L"Status"}){LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<LPWSTR>(title);col.cx=150;ListView_InsertColumn(list,i++,&col);}}
    chat_load_options(window);chat_fill_combos(window);chat_update_enabled(window);
}
void chat_show(HWND window,bool show,bool dm=false){
    bool changed=show&&chat_dm!=dm;
    if(changed){if(chat_running)return;chat_save_options(window);if(template_window)DestroyWindow(template_window);chat_dm=dm;chat_load_options(window);chat_fill_combos(window,true);}else if(show)chat_fill_combos(window);
    for(int cid:ChatControls)ShowWindow(GetDlgItem(window,cid),show&&!(chat_dm&&(cid==ChatGuild||cid==331||cid==ChatThreads||cid==ChatLive))?SW_SHOW:SW_HIDE);
    if(show){set(GetDlgItem(window,ChatRefresh),chat_dm?"Refresh DMs":"Refresh channels");set(GetDlgItem(window,ChatChannelLabel),chat_dm?"Find DM":"Find channel");set(GetDlgItem(window,ChatChannelOnly),chat_dm?"Selected DM only":"Selected channel only");
        SendMessageW(GetDlgItem(window,ChatChannelSearch),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(chat_dm?L"Search conversation name or ID":L"Search channel name or ID"));SendMessageW(GetDlgItem(window,ChatAuthor),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(chat_dm?L"Optional - filter by sender":L"Optional - collect all dates"));SendMessageW(GetDlgItem(window,ChatFrom),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(chat_dm?L"Blank: all dates":L"YYYY-MM-DD"));
        LVCOLUMNW column{};column.mask=LVCF_TEXT;column.pszText=const_cast<LPWSTR>(chat_dm?L"DM / group DM":L"Channel / thread");ListView_SetColumn(GetDlgItem(window,ChatChannels),0,&column);
        chat_load_channels(window);chat_load_messages(window);chat_update_enabled(window);if(changed)set(GetDlgItem(window,ChatProgressLabel),chat_dm?"Refresh DMs, check conversations, then collect back to From date. Blank means all dates.":"Blank From date resumes saved messages. Deletion checks require a date.");SendMessageW(window,WM_SIZE,0,0);
    }
}
