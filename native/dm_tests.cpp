#include "chat.h"
#include <fstream>
#include <iostream>
using namespace sml;

static int checks=0;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
template<class F> static void rejects(F action,const char* message){bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}require(rejected,message);}
static Json dm_message(uint64_t mid,const std::string& channel="200",const std::string& author="300",const std::string& text="DM text"){
    return {{"id",std::to_string(mid)},{"channel_id",channel},{"author",{{"id",author},{"username","Person "+author}}},{"content",text},{"timestamp",stamp((1420070400000LL+static_cast<int64_t>(mid>>22))*1000)},{"attachments",Json::array()}};
}
static Json metadata(const std::string& cid="200",int type=1){return {{"id",cid},{"type",type},{"name",nullptr},{"guild_id",nullptr},{"recipients",Json::array({{{"id","300"},{"username","Friend"}}})}};}
static Json attachment(const std::string& aid,const std::string& url){return {{"id",aid},{"filename","note.txt"},{"size",3},{"url",url}};}
static std::string parameter(const std::string& route,const std::string& name){
    auto begin=route.find('?');if(begin==std::string::npos)return {};auto key=name+"=";
    for(++begin;begin<route.size();){auto end=route.find('&',begin);if(route.compare(begin,key.size(),key)==0)return route.substr(begin+key.size(),end==std::string::npos?end:end-begin-key.size());if(end==std::string::npos)break;begin=end+1;}return {};
}
static Json search_page(Json rows,int64_t total){Json groups=Json::array();for(auto row:rows){row["hit"]=true;groups.push_back(Json::array({row}));}return {{"messages",groups},{"total_results",total},{"doing_deep_historical_index",false}};}

int main(){auto folder=std::filesystem::temp_directory_path()/(L"sml-dm-tests-"+std::to_wstring(GetCurrentProcessId()));try{
    std::atomic<bool> cancel=false;const std::string date="2026-01-01";const auto cutoff=std::stoull(chat_date_id(date));
    {
        int calls=0;Json listing=Json::array({
            {{"id","200"},{"type",1},{"recipients",Json::array({{{"id","300"},{"username","friend"},{"global_name","Display Friend"}}})}},
            {{"id","201"},{"type",3},{"name","Named group"},{"guild_id",nullptr},{"recipients",Json::array()}},
            {{"id","202"},{"type",3},{"name",nullptr},{"recipients",Json::array({{{"id","301"},{"username","First"}},{{"id","302"},{"username","Second"}}})}},
            {{"id","203"},{"type",1},{"recipients",Json::array()}}
        });
        ChatGet get=[&](const std::string& route)->Json{++calls;if(route!="users/@me/channels")throw std::runtime_error("DM listing used a non-private route");return listing;};
        auto result=list_dm_channels(get,cancel);auto rows=result.at("channels");
        require(calls==1&&rows.size()==4&&result.at("warnings").empty(),"Private channel listing lost DM/group rows");
        require(rows[0].at("name").get<std::string>().find("Display Friend")!=std::string::npos&&rows[1]["name"]=="Named group"&&rows[2].at("name").get<std::string>().find("First")!=std::string::npos&&rows[2].at("name").get<std::string>().find("Second")!=std::string::npos&&!rows[3].at("name").get<std::string>().empty(),"DM names did not use recipient display names, group name or fallback");
        ChatStore db(folder/L"listing.sqlite3");db.channels("@me",rows);
        require(db.channels("@me",std::string("500")).size()==4&&db.channels("100",std::string("500")).empty(),"Private channel cache leaked into server channel scope");
        listing=Json::array();require(list_dm_channels(get,cancel)["channels"].empty(),"Empty DM list was rejected");
        listing=Json::array({metadata("200",0),metadata("201",1)});auto skipped=list_dm_channels(get,cancel);require(skipped["channels"].size()==1&&skipped["channels"][0]["id"]=="201"&&skipped["warnings"].size()==1,"Unsupported conversation type was not excluded with a warning");
        for(const auto& invalid:std::vector<Json>{Json::object(),Json::array({metadata("0")}),Json::array({{{"id","200"},{"type",1},{"guild_id","100"}}}),Json::array({metadata(),metadata()}),Json::array({{{"id","200"},{"type",1},{"recipients","bad"}}})}){listing=invalid;rejects([&]{list_dm_channels(get,cancel);},"Malformed, duplicate or server DM listing was accepted");}
        rejects([&]{list_dm_channels([](const std::string&)->Json{throw ChatError(403,50001,"Denied");},cancel);},"Denied DM discovery became an empty successful list");
        int before=calls;cancel=true;rejects([&]{list_dm_channels(get,cancel);},"Cancelled discovery completed");require(calls==before,"Cancelled DM discovery sent a request");cancel=false;
        rejects([&]{list_dm_channels([&](const std::string&)->Json{cancel=true;return Json::array({metadata()});},cancel);},"Cancellation after discovery response was ignored");cancel=false;
    }
    {
        ChatStore db(folder/L"history.sqlite3");ChatOptions options;options.from_date=date;options.types={"txt"};Json source=Json::array();
        for(int i=-1;i<205;++i)source.push_back(dm_message(cutoff+i,"200",i%2?"300":"500"));
        source[0]["attachments"]=Json::array({attachment("701","too-old")});source[1]["attachments"]=Json::array({attachment("702","at-cutoff")});
        db.save_messages("@me","200",Json::array({source[0]}),"before-date-scan");
        int history=0,downloads=0,verified=0;bool missing=false;
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return metadata();
            if(route.starts_with("channels/200/messages?")){++history;auto before=std::stoull(parameter(route,"before"));Json page=Json::array();for(auto it=source.rbegin();it!=source.rend()&&page.size()<100;++it){auto mid=std::stoull((*it).at("id").get<std::string>());if(mid<before&&(!missing||mid!=cutoff+100))page.push_back(*it);}return page;}
            if(route=="channels/200/messages/"+std::to_string(cutoff+100)){++verified;throw ChatError(404,10008,"Unknown message");}
            throw std::runtime_error("Unexpected DM history/deletion route "+route);
        };
        auto download=[&](const std::string& url,const Path& path,int64_t){if(url!="at-cutoff")throw std::runtime_error("Downloaded before cutoff");++downloads;std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<"abc";};
        auto collect=[&]{return collect_chat(db,get,download,folder/L"history-files","500","@me",{"200"},options,cancel);};
        auto first=collect();require(first["results"][0]["status"]=="complete"&&first["results"][0]["messages_received"]==205&&history==3,"DM pagination did not stop at inclusive From date");
        require(!db.message(std::to_string(cutoff)).is_null()&&db.message(std::to_string(cutoff))["guild"]=="@me"&&db.messages("100","","",false,false)["total"]==0&&db.messages("@me","","",false,false,0,"500")["total"]==103,"DM collection omitted self messages or polluted server messages");
        require(downloads==1&&db.message(std::to_string(cutoff-1))["files"].empty()&&db.message(std::to_string(cutoff))["files"].size()==1,"Date cutoff did not constrain attachment downloads");
        auto state=db.state("200","500");require(state["lower"]==std::to_string(cutoff)&&first["results"][0]["checkpoint_updated"]==true,"Completed DM range checkpoint is wrong");
        history=0;auto again=collect();require(again["results"][0]["messages_received"]==0&&history==1&&downloads==1,"Incremental DM rerun reread or downloaded old messages");
        options.check_deletions=true;missing=true;auto deleted=collect();require(deleted["results"][0]["status"]=="complete"&&verified==1&&db.message(std::to_string(cutoff+100))["deleted"]==true&&db.message(std::to_string(cutoff-1))["deleted"]==false,"DM deletion verification escaped date range or lost missing message");
        options.check_deletions=false;options.since=false;missing=false;collect();require(db.message(std::to_string(cutoff+100))["deleted"]==false&&downloads==1,"DM reread did not restore a reappearing message or reused attachment");
        ChatStore all(folder/L"all-dates.sqlite3");options.from_date="";options.types.clear();history=0;auto all_result=collect_chat(all,get,{},folder/L"unused","500","@me",{"200"},options,cancel);
        require(all_result["results"][0]["status"]=="complete"&&all_result["results"][0]["messages_received"]==206&&all_result["all_history"]==true&&!all.message(std::to_string(cutoff-1)).is_null(),"Blank DM From date did not collect all available history");
        auto before=history;for(const auto& invalid:{"2026-02-30","not-a-date","2999-01-01"}){options.from_date=invalid;rejects([&]{collect();},"Invalid DM From date was accepted");}require(history==before,"Invalid DM date made history requests");
    }
    {
        ChatStore db(folder/L"group.sqlite3");ChatOptions options;options.from_date=date;options.types.clear();int pages=0;
        ChatGet get=[&](const std::string& route)->Json{if(route=="channels/201"){auto row=metadata("201",3);row["name"]="Named group";return row;}if(route.starts_with("channels/201/messages?")){++pages;return Json::array({dm_message(cutoff,"201"),dm_message(cutoff-1,"201")});}throw std::runtime_error("Unexpected group route");};
        auto result=collect_chat(db,get,{},folder/L"unused","500","@me",{"201"},options,cancel);require(result["results"][0]["status"]=="complete"&&result["results"][0]["messages_received"]==1&&pages==1&&db.message(std::to_string(cutoff-1)).is_null(),"Named group DM collection did not honor its date cutoff");
    }
    {
        ChatStore db(folder/L"safety.sqlite3");ChatOptions options;options.from_date=date;options.check_deletions=true;options.types.clear();auto cached=dm_message(cutoff+2);db.save_messages("@me","200",Json::array({cached}),"before-failure");db.checkpoint("200","500",std::to_string(cutoff),std::to_string(cutoff+10));auto checkpoint=db.state("200","500");
        for(const auto& mode:{"guild-metadata","wrong-id","wrong-type","guild-message","wrong-channel","denied","malformed","cancel"}){
            int history=0,verified=0;ChatGet get=[&](const std::string& route)->Json{
                if(route=="channels/200"){auto row=metadata();if(std::string(mode)=="guild-metadata")row["guild_id"]="100";if(std::string(mode)=="wrong-id")row["id"]="201";if(std::string(mode)=="wrong-type")row["type"]=0;return row;}
                if(route.starts_with("channels/200/messages?")){++history;if(std::string(mode)=="denied")throw ChatError(403,50001,"Denied");if(std::string(mode)=="malformed")return Json::object();auto row=dm_message(cutoff+3);if(std::string(mode)=="guild-message")row["guild_id"]="100";if(std::string(mode)=="wrong-channel")row["channel_id"]="201";if(std::string(mode)=="cancel")cancel=true;return Json::array({row});}
                ++verified;throw ChatError(404,10008,"Unknown message");
            };
            auto result=collect_chat(db,get,{},folder/L"unused","500","@me",{"200"},options,cancel);
            require(result["results"][0]["status"]!="complete"&&verified==0&&db.state("200","500")==checkpoint&&db.message(std::to_string(cutoff+2))["deleted"]==false&&db.message(std::to_string(cutoff+3)).is_null(),"Invalid, denied or cancelled DM response changed saved history/checkpoint or verified deletion");cancel=false;
        }
    }
    {
        ChatStore db(folder/L"author.sqlite3");ChatOptions options;options.from_date=date;options.author_id="<@!300>";options.types={"txt"};options.check_deletions=true;
        auto older=dm_message(cutoff-1);auto missing=dm_message(cutoff+500);auto other=dm_message(cutoff+501,"200","400");db.save_messages("@me","200",Json::array({older,missing,other}),"before-author");db.checkpoint("200","500",std::to_string(cutoff),std::to_string(cutoff+1000));auto checkpoint=db.state("200","500");
        int searches=0,verified=0,downloads=0;std::vector<uint64_t> cursors;Json source=Json::array();for(int i=0;i<31;++i)source.push_back(dm_message(cutoff+i));source[0]["attachments"]=Json::array({attachment("800","author-cutoff")});
        ChatGet get=[&](const std::string& route)->Json{
            if(route=="channels/200")return metadata();
            if(route.starts_with("channels/200/messages/search?")){
                if(parameter(route,"author_id")!="300"||parameter(route,"min_id")!=std::to_string(cutoff-1)||parameter(route,"sort_by")!="timestamp"||parameter(route,"sort_order")!="desc"||parameter(route,"offset")!="0")throw std::runtime_error("DM author search lost author/date/pagination scope");
                ++searches;auto before=std::stoull(parameter(route,"max_id"));cursors.push_back(before);Json rows=Json::array();int64_t total=0;for(auto it=source.rbegin();it!=source.rend();++it)if(std::stoull((*it)["id"].get<std::string>())<before){++total;if(rows.size()<25)rows.push_back(*it);}auto page=search_page(rows,total);if(!page["messages"].empty()){auto context=other;context["hit"]=false;page["messages"][0].push_back(context);}return page;
            }
            if(route=="channels/200/messages/"+std::to_string(cutoff+500)){++verified;throw ChatError(404,10008,"Unknown message");}
            throw std::runtime_error("DM author search used a guild/full-history/wrong-deletion route "+route);
        };
        auto download=[&](const std::string& url,const Path& path,int64_t){if(url!="author-cutoff")throw std::runtime_error("Unexpected author attachment");++downloads;std::filesystem::create_directories(path.parent_path());std::ofstream(path)<<"abc";};
        auto result=collect_chat(db,get,download,folder/L"author-files","500","@me",{"200"},options,cancel);
        require(result["results"][0]["status"]=="complete"&&result["results"][0]["messages_received"]==31&&searches==3&&cursors[1]==cutoff+6&&cursors[2]==cutoff&&downloads==1,"DM author search lost exact cutoff, pagination or attachments");
        require(verified==1&&db.message(std::to_string(cutoff+500))["deleted"]==true&&db.message(std::to_string(cutoff-1))["deleted"]==false&&db.message(std::to_string(cutoff+501))["deleted"]==false&&db.state("200","500")==checkpoint&&result["results"][0]["checkpoint_updated"]==false,"DM author deletion changed older/other-author messages or ordinary checkpoint");
        require(result["all_history"]==false&&db.messages("@me","200","",false,false,0,"300")["total"]==33,"Dated DM author scan claimed all dates or mixed author context");
        for(const auto& mode:{"wrong-author","wrong-channel","guild-hit","cancel"}){ChatGet bad=[&](const std::string& route)->Json{if(route=="channels/200")return metadata();auto row=dm_message(cutoff+700);if(std::string(mode)=="wrong-author")row["author"]["id"]="400";if(std::string(mode)=="wrong-channel")row["channel_id"]="201";if(std::string(mode)=="guild-hit")row["guild_id"]="100";if(std::string(mode)=="cancel")cancel=true;return search_page(Json::array({row}),1);};auto failed=collect_chat(db,bad,{},folder/L"unused","500","@me",{"200"},options,cancel);require(failed["results"][0]["status"]!="complete"&&db.message(std::to_string(cutoff+700)).is_null()&&db.state("200","500")==checkpoint,"DM author search accepted another scope or cancelled page");cancel=false;}
        auto calls_before=searches;options.from_date="2026-02-30";rejects([&]{collect_chat(db,get,{},folder/L"unused","500","@me",{"200"},options,cancel);},"DM author search ignored an invalid From date");require(searches==calls_before,"Invalid DM author date made network search requests");
    }
    {
        ChatStore db(folder/L"author-cutoff.sqlite3");ChatOptions options;options.from_date=date;options.author_id="300";options.types.clear();int pages=0;
        ChatGet get=[&](const std::string& route)->Json{if(route=="channels/200")return metadata();if(route.starts_with("channels/200/messages/search?")){++pages;return search_page(Json::array({dm_message(cutoff),dm_message(cutoff-1)}),2);}throw std::runtime_error("Unexpected cutoff search route");};
        auto result=collect_chat(db,get,{},folder/L"unused","500","@me",{"200"},options,cancel);require(result["results"][0]["status"]=="complete"&&result["results"][0]["messages_received"]==1&&pages==1&&!db.message(std::to_string(cutoff)).is_null()&&db.message(std::to_string(cutoff-1)).is_null(),"DM author search did not enforce inclusive cutoff locally");
    }
    {
        ChatStore first(folder/L"accounts"/L"500.sqlite3"),second(folder/L"accounts"/L"600.sqlite3");auto channel=metadata("201",3);channel["name"]="Same group";first.channels("@me",Json::array({channel}));second.channels("@me",Json::array({channel}));auto a=dm_message(cutoff,"201","300","Account A copy"),b=a;b["content"]="Account B copy";first.save_messages("@me","201",Json::array({a}),"A");second.save_messages("@me","201",Json::array({b}),"B");first.checkpoint("201","500",std::to_string(cutoff),std::to_string(cutoff+100));first.deleted(std::to_string(cutoff));
        require(first.message(std::to_string(cutoff))["message"]["content"]=="Account A copy"&&second.message(std::to_string(cutoff))["message"]["content"]=="Account B copy"&&second.message(std::to_string(cutoff))["deleted"]==false&&second.state("201","500").empty()&&second.state("201","600").empty(),"Separate account DM stores mixed group messages, deletion or checkpoints");
    }
    std::filesystem::remove_all(folder);std::cout<<"PASS: "<<checks<<" DM collection checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" DM checks: "<<e.what()<<"\n";return 1;}}
