#include "chat.h"
#include <iostream>
#include <fstream>
using namespace sml;
static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
static Json message(uint64_t mid,const std::string& text="hello"){
    return {{"id",std::to_string(mid)},{"channel_id","200"},{"author",{{"id","300"},{"username","Test user"}}},{"content",text},{"timestamp",stamp(now()-86400000000LL)},{"attachments",Json::array()}};
}
static std::string parameter(const std::string& route,const std::string& name){
    auto begin=route.find('?');if(begin==std::string::npos)return {};auto key=name+"=";
    for(++begin;begin<route.size();){auto end=route.find('&',begin);if(route.compare(begin,key.size(),key)==0)return route.substr(begin+key.size(),end==std::string::npos?end:end-begin-key.size());if(end==std::string::npos)break;begin=end+1;}return {};
}
static uint64_t search_cursor(const std::string& route,const std::string& author,const std::string& channel){
    require(route.starts_with("guilds/100/messages/search?")&&parameter(route,"author_id")==author&&parameter(route,"channel_id")==channel,"Search did not scope server, user and selected channel");
    require(parameter(route,"sort_by")=="timestamp"&&parameter(route,"sort_order")=="desc"&&parameter(route,"offset")=="0"&&parameter(route,"limit")=="25"&&parameter(route,"include_nsfw")=="true","Search must use descending results, all accessible channels and keyset pagination without offsets");
    return std::stoull(parameter(route,"max_id"));
}
static Json hit_group(Json row){row["hit"]=true;return Json::array({row});}
static Json search_page(const Json& groups,int64_t total){return {{"messages",groups},{"total_results",total},{"doing_deep_historical_index",false}};}
int main(){auto folder=std::filesystem::temp_directory_path()/(L"sml-chat-tests-"+std::to_wstring(GetCurrentProcessId()));int checks=0;try{
    std::atomic<bool> cancel=false;ChatOptions options;options.from_date=stamp(now()-2*86400000000LL).substr(0,10);auto start=std::stoull(chat_date_id(options.from_date))+1000;
    {
        ChatStore db(folder/L"live.sqlite3");ChatOptions live;live.live_from=std::to_string(start-10);live.types.clear();
        db.save_messages("100","200",Json::array({message(start)}),"seed");
        Json source=Json::array();for(int i=1;i<=205;++i)source.push_back(message(start+i));
        int pages=0;bool fail_second=true;bool invalid=false;
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Live"},{"type",0}};
            require(route.starts_with("channels/200/messages?")&&!parameter(route,"after").empty()&&parameter(route,"before").empty(),"Live capture did not use selected-channel forward pagination");
            auto after=std::stoull(parameter(route,"after"));++pages;if(fail_second&&pages==2)throw ChatError(503,0,"Temporary failure");
            if(invalid)return Json::array({message(after)});
            Json page=Json::array();for(auto& row:source)if(std::stoull(row["id"].get<std::string>())>after&&page.size()<100)page.push_back(row);std::reverse(page.begin(),page.end());return page;
        };
        auto collect=[&]{return collect_chat(db,get,{},folder/L"unused","300","100",{"200"},live,cancel);};
        auto partial=collect();require(partial["results"][0]["status"]=="failed"&&db.latest_message("200")==std::to_string(start+100),"Live failure did not retain its contiguous saved page");++checks;
        fail_second=false;auto recovered=collect();require(recovered["results"][0]["messages_received"]==105&&db.messages("100","200","",false,false)["total"]==206,"Live retry skipped unsaved messages or repeated saved pages");++checks;
        auto idle=collect();require(idle["results"][0]["messages_received"]==0&&db.state("200","300").empty(),"Idle live capture duplicated messages or changed history coverage");++checks;
        source.push_back(message(start+206));auto arrived=collect();require(arrived["results"][0]["messages_received"]==1&&db.latest_message("200")==std::to_string(start+206),"Later live message was not collected");++checks;
        invalid=true;auto rejected=collect();require(rejected["results"][0]["status"]=="failed"&&db.latest_message("200")==std::to_string(start+206),"Invalid live page advanced saved cursor");++checks;invalid=false;
        ChatStore fresh(folder/L"live-first.sqlite3");live.live_from=std::to_string(start+205);auto initial=collect_chat(fresh,get,{},folder/L"unused","300","100",{"200"},live,cancel);require(initial["results"][0]["messages_received"]==1&&fresh.message(std::to_string(start+205)).is_null(),"New live channel collected before session start");++checks;
        cancel=true;auto before=pages;collect();require(pages==before,"Cancelled live capture made requests");++checks;cancel=false;
    }
    {
        ChatStore db(folder/L"date-boundaries.sqlite3");ChatOptions opts;opts.types.clear();auto midnight=start-1000;
        Json rows=Json::array({message(midnight-1),message(midnight),message(start),message(start+1)});
        db.save_messages("100","200",Json::array({rows[2]}),"previous-capture");
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Dates"},{"type",0}};
            auto before=std::stoull(parameter(route,"before"));Json page=Json::array();for(auto it=rows.rbegin();it!=rows.rend();++it)if(std::stoull((*it)["id"].get<std::string>())<before)page.push_back(*it);return page;
        };
        auto collect=[&]{return collect_chat(db,get,{},folder/L"unused","300","100",{"200"},opts,cancel);};
        auto resumed=collect();require(resumed["results"][0]["messages_received"]==2&&!db.message(std::to_string(start+1)).is_null()&&db.message(std::to_string(midnight)).is_null(),"Blank date did not save newer messages from last saved ID");++checks;
        require(db.latest_message("201")=="1","Resume cursor leaked across channels");++checks;
        opts.from_date=options.from_date;auto dated=collect();require(dated["results"][0]["messages_received"]==3&&!db.message(std::to_string(midnight)).is_null()&&db.message(std::to_string(midnight-1)).is_null(),"Explicit date was not inclusive at UTC midnight");++checks;
        opts.author_id="300";opts.check_deletions=true;opts.from_date.clear();bool refused=false;try{collect();}catch(const std::exception& e){refused=std::string(e.what()).find("requires a From date")!=std::string::npos;}require(refused,"Author deletion allowed a blank date");++checks;
    }
    {
        ChatStore db(folder/L"chat.sqlite3");Json source=Json::array();for(int i=0;i<205;++i)source.push_back(message(start+i,i==203?"Could we add a plugin option?":"message "+std::to_string(i)));
        std::vector<std::string> routes;int mode=0;bool block=false;int download_calls=0;
        ChatGet get=[&](const std::string& route)->Json{
            routes.push_back(route);if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Chat"},{"type",0}};
            if(route.find("&around=")!=std::string::npos){
                if(mode==2)throw ChatError(403,50001,"History denied");
                if(mode==5)return Json::array();
                if(mode==6)return Json::object();
                if(mode==7){cancel=true;return Json::array({source[0]});}
                if(mode==4)return Json::array({source[4],source[0]});
                return Json::array({source[0]});
            }
            if(route.starts_with("channels/200/messages?")){
                if(block)throw ChatError(403,50001,"Denied");auto before=std::stoull(route.substr(route.find("before=")+7));Json rows=Json::array();for(auto it=source.rbegin();it!=source.rend()&&rows.size()<100;++it){auto id=std::stoull((*it)["id"].get<std::string>());if(id<before&&(mode==0||id!=start+4))rows.push_back(*it);}return rows;
            }
            if(route=="channels/200/messages/"+std::to_string(start+4)){if(mode>=2)throw ChatError(403,50001,"Denied");throw ChatError(404,10008,"Unknown message");}
            throw std::runtime_error("Unexpected route "+route);
        };
        auto download=[&](const std::string&,const Path& path,int64_t){++download_calls;std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<"abc";};
        auto collect=[&]{return collect_chat(db,get,download,folder/L"files","300","100",{"200"},options,cancel);};
        auto first=collect();require(first["results"][0]["messages_received"]==205&&routes.size()==5,"Pagination failed");++checks;
        require(db.messages("100","","",false,false)["total"]==205&&db.messages("100","","",true,false)["total"]==1,"Messages/suggestions count");++checks;
        auto state=db.state("200","300");auto date=options.from_date;options.from_date.clear();routes.clear();auto resumed=collect();require(db.messages("100","","",false,false)["total"]==205&&routes.size()==2&&resumed["results"][0]["messages_received"]==1,"Blank date did not resume at last saved message");++checks;
        options.from_date=date;auto dated=collect();require(dated["results"][0]["messages_received"]==205,"Explicit date skipped saved history with Check since enabled");++checks;
        options.from_date.clear();options.check_deletions=true;auto calls=routes.size();bool refused=false;try{collect();}catch(const std::exception& e){refused=std::string(e.what()).find("requires a From date")!=std::string::npos;}require(refused&&routes.size()==calls,"Blank deletion date was not refused before network access");++checks;options.check_deletions=false;
        ChatStore fresh(folder/L"blank-date.sqlite3");auto initial=collect_chat(fresh,get,download,folder/L"files","300","100",{"200"},options,cancel);require(initial["results"][0]["messages_received"]==205,"Blank date without saved history did not collect all history");++checks;
        options.since=false;auto reread=collect();require(reread["results"][0]["messages_received"]==205,"Unchecked Check since did not reread blank-date history");++checks;options.from_date=date;
        options.since=false;source[0]["content"]="edited message";collect();require(db.message(std::to_string(start))["message"]["content"]=="edited message","Edit not updated");++checks;
        options.check_deletions=true;mode=1;collect();require(db.message(std::to_string(start+4))["deleted"]==true&&db.message(std::to_string(start+4))["message"]["content"]=="message 4","Deletion did not retain original");++checks;
        mode=0;collect();require(db.message(std::to_string(start+4))["deleted"]==false,"Reappearance not restored");++checks;
        auto before_denial=db.state("200","300");mode=2;collect();require(db.message(std::to_string(start+4))["deleted"]==false&&db.state("200","300")==before_denial,"Message access denial became deletion or advanced checkpoint");++checks;mode=0;
        mode=3;auto fallback=collect();require(fallback["results"][0]["status"]=="complete"&&fallback["results"][0]["deletions_confirmed"]==1&&db.messages("100","200","",false,true)["total"]==1&&db.message(std::to_string(start+4))["message"]["content"]=="message 4","User-session fallback did not retain and expose deletion");++checks;
        mode=0;collect();mode=4;auto present=collect();require(present["results"][0]["status"]=="complete"&&present["results"][0]["deletions_confirmed"]==0&&db.message(std::to_string(start+4))["deleted"]==false,"Around lookup marked an existing message deleted");++checks;
        for(int failure_mode:{5,6,7}){mode=failure_mode;auto checkpoint=db.state("200","300");auto result=collect();require(result["results"][0]["status"]!="complete"&&db.state("200","300")==checkpoint&&db.messages("100","","",false,true)["total"]==0,"Empty, malformed or cancelled verification marked deleted or advanced checkpoint");++checks;cancel=false;}
        mode=0;
        auto previous=db.state("200","300");block=true;auto failed=collect();require(failed["results"][0]["status"]=="failed"&&db.state("200","300")==previous&&db.messages("100","","",false,true)["total"]==0,"Access failure advanced checkpoint or marked deleted");++checks;block=false;
        cancel=true;collect();require(db.state("200","300")==previous,"Cancel advanced checkpoint");++checks;cancel=false;
        options.check_deletions=false;options.types={"py"};source[0]["attachments"]=Json::array({{{"id","400"},{"filename","../../test.py"},{"size",4},{"url","https://cdn.discordapp.com/attachments/200/400/test.py"}},{{"id","401"},{"filename","big.py"},{"size",options.max_bytes+1},{"url","never"}},{{"id","402"},{"filename","image.png"},{"size",3},{"url","never"}}});collect();require(download_calls==1&&std::filesystem::exists(folder/L"files"/L"100"/L"200"/(wide(std::to_string(start))+L"-400.py")),"File filters or safe path failed");++checks;
        collect();require(download_calls==1,"Downloaded same attachment twice");++checks;
        auto old=options.from_date;options.since=true;options.from_date=stamp(now()-3*86400000000LL).substr(0,10);routes.clear();collect();require(routes.size()>=4,"Earlier from date skipped backfill");++checks;options.from_date=old;
        db.review(std::to_string(start+203),"Keep");collect();require(db.message(std::to_string(start+203))["review"]=="Keep","Recheck reset review");++checks;
        auto defaults=default_detection_patterns();require(defaults.size()==73&&suggestion_reason("feature request: test").starts_with("suggestion")&&suggestion_reason("bug report: broken").starts_with("bug"),"DevHub default template scoring");++checks;
        Json patterns=Json::array({{{"phrase","^special"},{"kind","suggestion"},{"weight",0.6}}});
        require(!suggestion_reason("Special thing",patterns).empty()&&suggestion_reason("a special thing",patterns).empty(),"Anchored detection templates");++checks;
        db.set_detection_patterns(patterns);require(db.messages("100","","",true,false)["total"]==0&&db.detection_patterns()==patterns,"Template save/rescan failed");++checks;
        db.set_detection_patterns(defaults);require(db.messages("100","","",true,false)["total"]==1&&db.message(std::to_string(start+203))["review"]=="Keep","Default reset lost review");++checks;
        require(attachment_allowed({{"filename","TEST.PY"},{"size",options.max_bytes}},options)&&!attachment_allowed({{"filename","a.py"},{"size",-1}},options),"Size/type boundary");++checks;
        bool bad=false;try{chat_date_id("2026-02-30");}catch(...){bad=true;}require(bad,"Invalid date accepted");++checks;
        require(db.messages("100","200","edited",false,false)["total"]==1&&db.messages("100","","",false,false,200)["rows"].size()==5,"Search/page failure");++checks;
    }
    {
        ChatStore db(folder/L"author.sqlite3");Json source=Json::array();
        for(int i=0;i<250;++i){auto row=message(start+i,i==0?"Could we add an option?":"author fixture "+std::to_string(i));if(i!=0&&i!=110)row["author"]["id"]="400";source.push_back(row);}
        source[0]["attachments"]=Json::array({{{"id","600"},{"filename","wanted.txt"},{"size",3},{"url","wanted"}}});
        source[249]["attachments"]=Json::array({{{"id","601"},{"filename","unwanted.txt"},{"size",3},{"url","unwanted"}}});
        auto second_target=message(start+1000);second_target["channel_id"]="201";
        auto second_other=message(start+1001);second_other["channel_id"]="201";second_other["author"]["id"]="400";
        auto absent_other=message(start+2000,"previously saved other user");absent_other["author"]["id"]="400";
        db.save_messages("100","200",Json::array({absent_other}),"before-author-scan");
        db.checkpoint("200","500",chat_date_id(options.from_date),chat_time_id(now()));auto ordinary_state=db.state("200","500");
        ChatOptions author_options;author_options.author_id="<@!00300>";author_options.from_date="ignored in all-history user mode";author_options.since=true;author_options.types={"txt"};
        int history_calls=0,search_calls=0,verification_calls=0,downloads=0,verification_mode=0;bool omit_target=false,allow_history=false;
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200"||route=="channels/201")return {{"id",route.substr(9)},{"guild_id","100"},{"name","Author fixture"},{"type",0}};
            if(route.starts_with("guilds/100/messages/search?")){
                ++search_calls;auto channel=parameter(route,"channel_id");require(channel=="200"||channel=="201","Author search requested an unselected channel");
                auto author=id(author_options.author_id);auto before=search_cursor(route,author,channel);Json groups=Json::array();int64_t total=0;
                auto add=[&](const Json& row){auto mid=std::stoull(row.at("id").get<std::string>());if(mid>=before||row.at("author").at("id")!=author||(omit_target&&mid==start))return;++total;if(groups.size()<25){auto group=hit_group(row);auto context=source[249];context["hit"]=false;group.push_back(context);groups.push_back(group);}};
                if(channel=="200"){for(auto it=source.rbegin();it!=source.rend();++it)add(*it);}else{add(second_other);add(second_target);}
                return search_page(groups,total);
            }
            if(route.find("/messages?")!=std::string::npos){
                if(route.find("&around=")!=std::string::npos){auto row=source[0];row["author"]["id"]="400";return Json::array({row,source[110]});}
                require(allow_history,"User search read unrelated full-channel history");
                ++history_calls;auto before=std::stoull(route.substr(route.find("before=")+7));Json rows=Json::array();
                if(route.starts_with("channels/200/")){for(auto it=source.rbegin();it!=source.rend()&&rows.size()<100;++it){auto mid=std::stoull((*it)["id"].get<std::string>());if(mid<before&&(!omit_target||mid!=start))rows.push_back(*it);}}
                else if(route.starts_with("channels/201/")){for(auto& row:Json::array({second_other,second_target}))if(std::stoull(row["id"].get<std::string>())<before)rows.push_back(row);}
                else throw std::runtime_error("Author collection requested unselected channel");return rows;
            }
            ++verification_calls;require(route=="channels/200/messages/"+std::to_string(start),"Author deletion checked another user's message");
            if(verification_mode==0)throw ChatError(404,10008,"Unknown message");
            if(verification_mode==4)throw ChatError(403,50001,"Single-message endpoint denied");
            auto row=source[0];if(verification_mode==1)row["author"]["id"]="400";if(verification_mode==2)row["id"]=std::to_string(start+999);if(verification_mode==3)row["channel_id"]="201";return row;
        };
        auto download=[&](const std::string& url,const Path& path,int64_t){require(url=="wanted","Downloaded a different user's attachment");++downloads;std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<"abc";};
        auto collect=[&]{return collect_chat(db,get,download,folder/L"author-files","500","100",{"200","201"},author_options,cancel);};
        auto selected=collect();
        require(selected["author_id"]=="300"&&selected["all_history"]==true&&selected["processed_channels"]==2&&selected["results"][0]["messages_received"]==2&&selected["results"][1]["messages_received"]==1&&selected["results"][0]["messages_scanned"]==2&&search_calls==4&&history_calls==0,"Author collection did not page exclusively through matching search results across selected channels");++checks;
        require(db.messages("100","","",false,false,0,"300")["total"]==3&&db.messages("100","","",false,false,0,"30")["total"]==0&&db.messages("100","201","",false,false,0,"300")["total"]==1&&db.messages("100","","author fixture",false,false,0,"300")["total"]==1,"Saved author filter was not exact or did not combine with search/channel");++checks;
        require(downloads==1&&db.message(std::to_string(start+249)).is_null()&&db.message(std::to_string(start))["files"].size()==1,"Author scan saved/downloaded another author's messages");++checks;
        require(db.state("200","500")==ordinary_state&&db.state("201","500").empty()&&selected["results"][0]["checkpoint_updated"]==false,"Author scan changed ordinary checkpoint coverage");++checks;
        search_calls=0;collect();require(search_calls==4&&history_calls==0&&downloads==1&&db.messages("100","","",false,false,0,"300")["total"]==3,"Repeated author search skipped results, duplicated messages or redownloaded attachment");++checks;
        auto calls_before=search_calls;for(const auto& invalid:{"0","no-user","18446744073709551616"}){author_options.author_id=invalid;bool refused=false;try{collect();}catch(...){refused=true;}require(refused&&search_calls==calls_before&&history_calls==0,"Invalid author ID made network requests");++checks;}author_options.author_id="300";
        omit_target=true;author_options.check_deletions=true;author_options.from_date=options.from_date;auto deleted=collect();
        require(deleted["results"][0]["status"]=="complete"&&deleted["results"][0]["deletions_confirmed"]==1&&verification_calls==1&&db.message(std::to_string(start))["deleted"]==true&&db.message(std::to_string(start+2000))["deleted"]==false,"User-only deletion touched another author or missed target");++checks;
        require(db.messages("100","","",false,true,0,"300")["total"]==1&&db.messages("100","","",true,false,0,"300")["total"]==1,"Author filter did not combine with deletion/suggestion filters");++checks;
        omit_target=false;author_options.check_deletions=false;collect();omit_target=true;author_options.check_deletions=true;
        for(int mode:{1,2,3,4}){verification_mode=mode;auto mismatch=collect();require(mismatch["results"][0]["status"]=="failed"&&db.message(std::to_string(start))["deleted"]==false&&db.message(std::to_string(start))["message"]["author"]["id"]=="300"&&db.state("200","500")==ordinary_state,"Author verification accepted mismatched message ID/channel/author");++checks;}
        omit_target=false;author_options.check_deletions=false;author_options.author_id="400";author_options.types.clear();auto other=collect();
        require(other["results"][0]["messages_received"]==248&&other["results"][1]["messages_received"]==1&&db.messages("100","","",false,false,0,"400")["total"]==250&&db.state("200","500")==ordinary_state,"Switching author reused/overwrote ordinary checkpoint or mixed users");++checks;
        ChatStore fresh(folder/L"author-then-normal.sqlite3");author_options.author_id="300";author_options.check_deletions=false;
        collect_chat(fresh,get,download,folder/L"author-files","501","100",{"200"},author_options,cancel);
        require(fresh.state("200","501").empty()&&fresh.messages("100","200","",false,false)["total"]==2,"User-only capture advertised full-channel completion");++checks;
        ChatOptions normal_options;normal_options.from_date=options.from_date;normal_options.types.clear();
        allow_history=true;auto normal=collect_chat(fresh,get,download,folder/L"author-files","501","100",{"200"},normal_options,cancel);
        require(normal["results"][0]["status"]=="complete"&&normal["results"][0]["checkpoint_updated"]==true&&fresh.messages("100","200","",false,false)["total"]==250&&!fresh.state("200","501").empty(),"Normal scan after author-only scan skipped other users");++checks;
    }
    {
        ChatStore db(folder/L"search-parent-thread.sqlite3");ChatOptions user;user.author_id="300";user.types={"txt"};int searches=0,downloads=0;
        auto thread_hit=message(start+1000);thread_hit["channel_id"]="201";thread_hit["attachments"]=Json::array({{{"id","700"},{"filename","thread.txt"},{"size",3},{"url","not-selected"}}});
        auto target=message(start+500);auto older_thread=thread_hit;older_thread["id"]=std::to_string(start+400);
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Parent channel"},{"type",0}};
            auto cursor=search_cursor(route,"300","200");++searches;
            if(searches==1)return search_page(Json::array({hit_group(thread_hit)}),3);
            if(searches==2){require(cursor==start+1000,"Thread-only search page did not advance to its oldest hit");return search_page(Json::array({hit_group(target),hit_group(older_thread)}),2);}
            require(searches==3&&cursor==start+400,"Mixed thread/channel search page did not advance past every hit");return search_page(Json::array(),0);
        };
        auto result=collect_chat(db,get,[&](const std::string&,const Path&,int64_t){++downloads;},folder/L"unused","500","100",{"200"},user,cancel);
        require(result["results"][0]["status"]=="complete"&&result["results"][0]["messages_received"]==1&&result["results"][0]["messages_scanned"]==3&&searches==3&&downloads==0&&db.messages("100","200","",false,false)["total"]==1&&db.messages("100","201","",false,false)["total"]==0&&db.state("200","500").empty()&&db.state("201","500").empty(),"Parent-channel search failed to traverse thread-only results or saved/downloaded unselected thread messages");++checks;
    }
    {
        ChatStore db(folder/L"search-large.sqlite3");ChatOptions user;user.author_id="300";user.types.clear();int searches=0;uint64_t last_cursor=~uint64_t(0);constexpr int count=10001;
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Large search"},{"type",0}};
            auto cursor=search_cursor(route,"300","200");require(cursor<last_cursor,"Search cursor did not move to older matches");last_cursor=cursor;++searches;
            Json groups=Json::array();int64_t total=0;for(int i=count-1;i>=0;--i)if(start+static_cast<uint64_t>(i)<cursor){++total;if(groups.size()<25)groups.push_back(hit_group(message(start+i)));}return search_page(groups,total);
        };
        auto result=collect_chat(db,get,{},folder/L"unused","500","100",{"200"},user,cancel);
        require(result["results"][0]["status"]=="complete"&&result["results"][0]["messages_received"]==count&&searches==402&&db.messages("100","200","",false,false)["total"]==count,"Author search stopped at Discord's 10000-result offset limit or dropped a boundary match");++checks;
        require(db.state("200","500").empty(),"Large author search wrote a full-channel checkpoint");++checks;
    }
    {
        ChatStore db(folder/L"search-safety.sqlite3");ChatOptions user;user.author_id="300";user.types.clear();int searches=0,verifications=0;std::string mode="empty";
        auto cached=message(start,"previously saved user message");cached["reactions"]=Json::array({{{"count",2},{"emoji",{{"name","test"}}}}});
        auto seed=[&]{db.save_messages("100","200",Json::array({cached}),"before-search");db.checkpoint("200","500",chat_date_id(options.from_date),chat_time_id(now()));};
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return {{"id","200"},{"guild_id","100"},{"name","Search safety"},{"type",0}};
            if(route.starts_with("guilds/100/messages/search?")){
                search_cursor(route,"300","200");++searches;auto row=message(start+10000);auto groups=Json::array({hit_group(row)});
                if(mode=="denied")throw ChatError(403,50001,"Search denied");
                if(mode=="indexing"||(mode=="index-once"&&searches==1))return {{"code",110000},{"retry_after",0.001}};
                if(mode=="cancel-index"){cancel=true;return {{"code",110000},{"retry_after",0}};}
                if(mode=="malformed")return Json::array();
                if(mode=="missing-total")return {{"messages",Json::array()}};
                if(mode=="wrong-total")return {{"messages",Json::array()},{"total_results","0"}};
                if(mode=="negative-total")return search_page(Json::array(),-1);
                if(mode=="empty-positive")return search_page(Json::array(),1);
                if(mode=="malformed-group")return search_page(Json::array({Json::object()}),1);
                if(mode=="wrong-author")groups[0][0]["author"]["id"]="400";
                if(mode=="wrong-guild")groups[0][0]["guild_id"]="101";
                if(mode=="context-only")groups[0][0]["hit"]=false;
                if(mode=="missing-hit")groups[0][0].erase("hit");
                if(mode=="wrong-hit")groups[0][0]["hit"]="true";
                if(mode=="duplicate-hit")groups.push_back(groups[0]);
                if(mode=="deep-index"){auto page=search_page(groups,1);page["doing_deep_historical_index"]=true;return page;}
                if(mode=="cancel-page"){cancel=true;return search_page(groups,1);}
                if(mode=="repeat"||mode=="wrong-author"||mode=="wrong-guild"||mode=="context-only"||mode=="missing-hit"||mode=="wrong-hit"||mode=="duplicate-hit")return search_page(groups,1);
                if(mode=="refresh"&&searches==1){auto fresh=cached;fresh.erase("reactions");fresh["content"]="updated search text";return search_page(Json::array({hit_group(fresh)}),1);}
                return search_page(Json::array(),0);
            }
            ++verifications;require(route=="channels/200/messages/"+std::to_string(start),"Search absence verified the wrong saved message");
            if(mode=="cancel-404"){cancel=true;throw ChatError(404,10008,"Unknown message");}
            if(mode=="unknown")throw ChatError(404,10008,"Unknown message");
            auto fresh=cached;fresh["content"]="verified present";if(mode=="cancel-200")cancel=true;return fresh;
        };
        auto collect=[&]{searches=verifications=0;return collect_chat(db,get,{},folder/L"unused","500","100",{"200"},user,cancel);};
        auto empty=collect();require(empty["results"][0]["status"]=="complete"&&empty["results"][0]["messages_received"]==0&&searches==1&&verifications==0,"Empty user search was not a successful zero-result collection");++checks;
        mode="index-once";auto indexed=collect();require(indexed["results"][0]["status"]=="complete"&&searches==2,"Discord indexing response did not retry the same search");++checks;
        seed();auto checkpoint=db.state("200","500");user.check_deletions=true;user.from_date=options.from_date;
        for(const auto& failure:{"malformed","missing-total","wrong-total","negative-total","empty-positive","malformed-group","wrong-author","wrong-guild","context-only","missing-hit","wrong-hit","duplicate-hit","deep-index","denied","indexing","cancel-index","cancel-page","repeat"}){
            mode=failure;auto result=collect();require(result["results"][0]["status"]!="complete"&&verifications==0&&db.message(std::to_string(start))["deleted"]==false&&db.message(std::to_string(start))["message"]["content"]==cached["content"]&&db.state("200","500")==checkpoint,"Incomplete, malformed, denied or cancelled author search verified deletions or changed existing history/checkpoint");++checks;
            if(mode=="indexing"){require(searches==6,"Persistent indexing did not stop after bounded retries");++checks;}
            if(mode=="cancel-page"){require(db.message(std::to_string(start+10000)).is_null(),"Cancelled search page saved new messages");++checks;}
            if(mode=="repeat"){require(searches==2&&result["results"][0]["messages_received"]==1&&!db.message(std::to_string(start+10000)).is_null(),"Nonadvancing search lost already-saved matches or did not stop");++checks;}
            cancel=false;
        }
        // Exclude the partial-page fixture from later missing-row verification checks.
        db.deleted(std::to_string(start+10000));
        mode="present";auto present=collect();require(present["results"][0]["status"]=="complete"&&verifications==1&&db.message(std::to_string(start))["deleted"]==false&&db.message(std::to_string(start))["message"]["content"]=="verified present","Missing search result was treated as deletion instead of verifying the saved message");++checks;
        for(const auto& failure:{"cancel-200","cancel-404"}){seed();mode=failure;auto result=collect();require(result["results"][0]["status"]=="cancelled"&&verifications==1&&db.message(std::to_string(start))["deleted"]==false&&db.message(std::to_string(start))["message"]["content"]==cached["content"],"Cancellation during direct deletion verification mutated a saved message");++checks;cancel=false;}
        mode="unknown";auto removed=collect();require(removed["results"][0]["status"]=="complete"&&removed["results"][0]["deletions_confirmed"]==1&&db.message(std::to_string(start))["deleted"]==true,"Explicit Unknown Message did not retain a deleted saved copy after author search");++checks;
        seed();mode="refresh";user.check_deletions=false;auto refreshed=collect();require(refreshed["results"][0]["status"]=="complete"&&db.message(std::to_string(start))["message"]["content"]=="updated search text"&&db.message(std::to_string(start))["message"]["reactions"]==cached["reactions"],"Author search discarded saved reactions omitted from search payloads");++checks;
    }
    {
        ChatStore db(folder/L"location.sqlite3");Json rows=Json::array();
        for(int i=0;i<1000;++i){auto row=message(10000+i,i==350?"context needle":i==700?"Could we add an option?":"surrounding conversation");row["timestamp"]="2026-01-01T00:00:00+00:00";rows.push_back(row);}
        db.save_messages("100","200",rows,"location-test");db.deleted("10350");db.deleted("10900");
        auto other_channel=message(20000);other_channel["channel_id"]="201";other_channel["timestamp"]="2026-01-02T00:00:00+00:00";db.save_messages("100","201",Json::array({other_channel}),"location-test");
        auto other_guild=message(30000);other_guild["timestamp"]="2026-01-02T00:00:00+00:00";db.save_messages("101","200",Json::array({other_guild}),"location-test");
        auto location=db.message_location("100","10350");auto position=location["position"].get<size_t>();
        require(location["channel"]=="200"&&position==649&&position/100+1==7&&db.messages("100","200","",false,false,position/100*100)["rows"][position%100]["id"]=="10350","Message context did not locate page 7 of ten channel pages");++checks;
        require(db.messages("100","200","context needle",false,false)["total"]==1&&db.messages("100","200","",true,false)["total"]==1&&db.messages("100","200","",false,true)["total"]==2&&db.message_location("100","10350")["position"]==649,"Context location filtered out deleted or suggestion messages");++checks;
        for(size_t rank:{size_t(0),size_t(99),size_t(100),size_t(199),size_t(200),size_t(599),size_t(600),size_t(699),size_t(700),size_t(999)}){
            auto mid=std::to_string(10999-rank);auto found=db.message_location("100",mid);
            require(found["position"]==rank&&db.messages("100","200","",false,false,rank/100*100)["rows"][rank%100]["id"]==mid,"Same-timestamp context rank disagrees with page ordering");++checks;
        }
        require(db.message_location("100","20000")==Json({{"channel","201"},{"position",0}})&&db.message_location("101","30000")==Json({{"channel","200"},{"position",0}}),"Context rank leaked messages across channels or guilds");++checks;
        require(db.message_location("101","10350").is_null()&&db.message_location("100","999999").is_null(),"Missing or wrong-server context target was accepted");++checks;
        auto older=message(60001),newer=message(60000);older["channel_id"]=newer["channel_id"]="202";older["timestamp"]="2026-01-01T00:00:00+00:00";newer["timestamp"]="2026-01-02T00:00:00+00:00";
        db.save_messages("100","202",Json::array({older,newer}),"location-test");
        require(db.message_location("100","60000")["position"]==0&&db.message_location("100","60001")["position"]==1,"Context ordering prioritized message ID over timestamp");++checks;
        auto lexical_first=message(20000000),numeric_first=message(190000000);lexical_first["channel_id"]=numeric_first["channel_id"]="203";lexical_first["timestamp"]=numeric_first["timestamp"]="2026-01-01T00:00:00+00:00";
        db.save_messages("100","203",Json::array({lexical_first,numeric_first}),"location-test");
        require(db.message_location("100","20000000")["position"]==0&&db.messages("100","203","",false,false)["rows"][0]["id"]=="20000000","Context ordering differs from existing message ID TEXT ordering");++checks;
    }
    {
        std::vector<std::string> routes;ChatGet threads=[&](const std::string& route)->Json{routes.push_back(route);
            if(route=="guilds/100/channels")return Json::array({{{"id","200"},{"name","Chat"},{"type",0}}});
            if(route.find("archived=false&offset=0")!=std::string::npos)return {{"threads",Json::array({{{"id","210"},{"name","Active"},{"type",11},{"parent_id","200"}}})},{"has_more",true}};
            if(route.find("archived=false&offset=1")!=std::string::npos)return {{"threads",Json::array({{{"id","211"},{"name","Active 2"},{"type",11},{"parent_id","200"}}})},{"has_more",false}};
            if(route.find("archived=true&offset=0")!=std::string::npos)return {{"threads",Json::array({{{"id","210"},{"name","Active became archived"},{"type",11},{"parent_id","200"}}})},{"has_more",false}};
            throw std::runtime_error("Unexpected thread route");};
        auto result=list_chat_channels(threads,"100",cancel);require(result["channels"].size()==3&&routes.size()==4&&result["warnings"].empty(),"User thread search pagination or deduplication failed");++checks;
    }
    for(bool dm:{false,true}){
        auto scope=dm?std::string("@me"):std::string("100");auto path=folder/(dm?L"forward-dm.sqlite3":L"forward-channel.sqlite3");
        auto forward=message(start+500,"outer caption");
        Json file={{"id","9000"},{"filename","forwarded.txt"},{"size",3},{"url","forward-file"}};
        Json inner={{"content","Could we add forwardedneedle support?"},{"attachments",Json::array({file})},{"embeds",Json::array({{{"title","forwardembed"},{"description","embedded description"},{"fields",Json::array({{{"name","field name"},{"value","field value"}}})}}})}};
        forward["message_snapshots"]=Json::array({{{"message",inner}}});forward["message_reference"]={{"type",1},{"channel_id","999"},{"message_id","9999"}};
        Json original_state,raw_before_migration;int downloads=0;
        {
            ChatStore db(path);ChatOptions opts;opts.from_date=options.from_date;opts.since=false;opts.types={"txt"};
            ChatGet get=[&](const std::string& route)->Json{
                if(route=="channels/200"){Json meta={{"id","200"},{"name","Forward fixture"},{"type",dm?1:0}};if(!dm)meta["guild_id"]="100";return meta;}
                if(route.find("/messages/search?")!=std::string::npos){require(route.starts_with(dm?"channels/200/":"guilds/100/"),"Wrong forward author search scope");return std::stoull(parameter(route,"max_id"))>start+500?search_page(Json::array({hit_group(forward)}),1):search_page(Json::array(),0);}
                if(route.starts_with("channels/200/messages?"))return std::stoull(parameter(route,"before"))>start+500?Json::array({forward}):Json::array();
                throw std::runtime_error("Forward parsing fetched a source conversation");
            };
            auto download=[&](const std::string& url,const Path& target,int64_t){require(url=="forward-file","Wrong forward attachment URL");++downloads;std::filesystem::create_directories(target.parent_path());std::ofstream(target)<<"abc";};
            auto collect=[&]{return collect_chat(db,get,download,folder/(dm?L"forward-dm-files":L"forward-channel-files"),"500",scope,{"200"},opts,cancel);};
            auto result=collect();require(result["results"][0]["status"]=="complete"&&downloads==1&&db.messages(scope,"","forwardedneedle",false,false)["total"]==1&&db.messages(scope,"","forwardembed",false,false)["total"]==1,"Forward text/embed collection or download failed");++checks;
            auto saved=db.message(std::to_string(start+500));require(saved["message"]==forward&&saved["parsed_content"].get<std::string>().find("[Forwarded message]")!=std::string::npos&&saved["files"].size()==1&&db.messages(scope,"","",false,false,0,"300")["total"]==1,"Forward raw snapshot, sender or parsed JSON not preserved");++checks;
            db.set_detection_patterns(Json::array({{{"phrase","^could we"},{"kind","suggestion"},{"weight",1.0}}}));require(db.messages(scope,"","",true,false)["total"]==1,"Forward suggestion anchors were lost");++checks;
            original_state=db.state("200","500");opts.author_id="300";collect();require(downloads==1&&db.state("200","500")==original_state,"Forward author search redownloaded attachments or changed checkpoint");++checks;
            db.review(std::to_string(start+500),"Keep");db.deleted(std::to_string(start+500));
            raw_before_migration=db.message(std::to_string(start+500))["message"];
        }
        // Emulate a pre-parser cache and reopen: only derived content/suggestions may change.
        sqlite3* legacy{};require(sqlite3_open(utf8(path.wstring()).c_str(),&legacy)==SQLITE_OK,"Could not open legacy fixture");
        require(sqlite3_exec(legacy,"DELETE FROM chat_settings WHERE key='forward_content_version'; UPDATE chat_messages SET content='outer caption',suggestion='';",nullptr,nullptr,nullptr)==SQLITE_OK,"Could not prepare legacy fixture");sqlite3_close(legacy);
        {ChatStore db(path);auto saved=db.message(std::to_string(start+500));require(db.messages(scope,"","forwardedneedle",true,true)["total"]==1&&saved["review"]=="Keep"&&saved["deleted"]==true&&saved["message"]==raw_before_migration&&db.state("200","500")==original_state,"Forward migration lost status/raw/checkpoint or did not index saved content");++checks;}
        auto nested=forward;for(int i=0;i<12;++i)nested={{"message_snapshots",Json::array({{{"message",nested}}})}};require(chat_message_text(nested).find("truncated")!=std::string::npos,"Nested forwards were not bounded");++checks;
        auto malformed=forward;malformed["message_snapshots"]=Json::array({nullptr,{{"message",nullptr}}});require(chat_message_text(malformed).find("unavailable")!=std::string::npos,"Malformed snapshot was silently empty");++checks;
        auto attachment_only=forward;attachment_only["content"]="";attachment_only["message_snapshots"][0]["message"]["content"]="";attachment_only["message_snapshots"][0]["message"].erase("embeds");require(chat_message_text(attachment_only).find("forwarded.txt")!=std::string::npos&&chat_message_attachments(attachment_only).size()==1,"Attachment-only forward was invisible");++checks;
        forward["attachments"]=Json::array({file});require(chat_message_attachments(forward).size()==1,"Duplicate forward attachment was not deduplicated");++checks;
        auto reply=message(start+501);reply["referenced_message"]=forward;require(chat_message_text(reply)=="hello"&&chat_message_attachments(reply).empty(),"Reply was incorrectly expanded as a forward");++checks;
    }
    {ChatStore db(folder/L"chat.sqlite3");require(db.messages("100","","",false,false)["total"]==205&&!db.state("200","300").empty(),"Restart lost messages/checkpoint");++checks;}
    require(folder.filename().wstring().starts_with(L"sml-chat-tests-"),"Unsafe cleanup");std::filesystem::remove_all(folder);std::cout<<"PASS: "<<checks<<" message collection checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
