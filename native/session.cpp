#include "session.h"
#include "gateway.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace sml {
namespace {
struct Internet {
    HINTERNET handle{};
    explicit Internet(HINTERNET value):handle(value){if(!handle)throw std::runtime_error("Could not initialize the Discord connection");}
    ~Internet(){WinHttpCloseHandle(handle);}
    Internet(const Internet&)=delete;
};
Json get_discord(const std::string& path,const std::string& token) {
    Internet session(WinHttpOpen(L"XADiscordExplorer/0.3",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0));
    WinHttpSetTimeouts(session.handle,10000,10000,10000,10000);
    Internet connection(WinHttpConnect(session.handle,L"discord.com",INTERNET_DEFAULT_HTTPS_PORT,0));
    auto route=wide("/api/v10/"+path);
    Internet request(WinHttpOpenRequest(connection.handle,L"GET",route.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if(!WinHttpSetOption(request.handle,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)))
        throw std::runtime_error("Could not set connection security options");
    auto header=wide("Authorization: "+token+"\r\n");
    BOOL sent=WinHttpSendRequest(request.handle,header.c_str(),static_cast<DWORD>(header.size()),nullptr,0,0,0);
    SecureZeroMemory(header.data(),header.size()*sizeof(wchar_t));
    if(!sent||!WinHttpReceiveResponse(request.handle,nullptr))throw std::runtime_error("Discord could not be reached. Check your connection and try again.");
    DWORD status=0,length=sizeof(status);
    if(!WinHttpQueryHeaders(request.handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&length,nullptr))
        throw std::runtime_error("Discord returned an unreadable response");
    if(status==401)throw std::runtime_error("Session expired or invalid. Reconnect this account with its current token.");
    if(status==403)throw std::runtime_error("Discord denied this session access to the requested list (HTTP 403). Existing saved data was kept.");
    if(status==429)throw std::runtime_error("Discord rate limit reached. Wait before refreshing again; saved membership was kept.");
    if(status!=200)throw std::runtime_error("Discord refused the request (HTTP "+std::to_string(status)+"). Saved membership was kept.");
    std::string body;char buffer[16384];DWORD read=0;
    while(true) {
        if(!WinHttpReadData(request.handle,buffer,sizeof(buffer),&read))throw std::runtime_error("The Discord response was interrupted. Saved membership was kept.");
        if(!read)break;
        body.append(buffer,read);
        if(body.size()>10*1024*1024)throw std::runtime_error("Discord response exceeded the supported size");
    }
    try{return Json::parse(body);}catch(...){throw std::runtime_error("Discord returned an invalid response. Saved membership was kept.");}
}
}
Json fetch_account(const DiscordGet& get) {
    auto profile=get("users/@me");
    if(!profile.is_object()||profile.value("bot",false))throw std::runtime_error("Connect one of your user accounts, not a bot token");
    auto uid=id(profile.at("id"));auto username=profile.at("username").get<std::string>();
    Json guilds=Json::array();std::set<std::string> seen;std::string after="0";
    // Fetch every page before committing; incomplete responses never remove saved memberships.
    for(int page=0;page<100;++page) {
        auto batch=get("users/@me/guilds?limit=200&after="+after);
        if(!batch.is_array()||batch.size()>200)throw std::runtime_error("Discord returned an invalid server list");
        uint64_t largest=std::stoull(after);
        for(auto& guild:batch) {
            auto sid=id(guild.at("id"));auto name=guild.at("name").get<std::string>();
            if(name.empty()||wide(name).size()>100||std::stoull(sid)<=std::stoull(after)||!seen.insert(sid).second)
                throw std::runtime_error("Discord returned an inconsistent server page; saved membership was kept");
            guilds.push_back({{"server_id",sid},{"server_name",name}});largest=std::max(largest,std::stoull(sid));
        }
        if(batch.size()<200)return {{"user_id",uid},{"username",username},{"guilds",guilds},{"captured_at",stamp(now())}};
        after=std::to_string(largest);
    }
    throw std::runtime_error("Server-list pagination did not finish; saved membership was kept");
}
Json connect_account(const std::string& token) {
    if(token.empty()||token.size()>4096||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<33||c>126;}))
        throw std::runtime_error("Enter a session token without spaces or line breaks");
    return fetch_account([&](const std::string& path){return get_discord(path,token);});
}
Json collect_members(const std::string& token,const std::string& server,const std::atomic<bool>& cancel,const ProgressCallback& progress) {
    if(token.empty()||token.size()>4096||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<33||c>126;}))
        throw std::runtime_error("Invalid session token; reconnect the account");
    return collect_gateway_members(token,server,cancel,progress);
}
Json collect_server_count(const std::string& token,const std::string& server,const std::atomic<bool>& cancel,const ProgressCallback& progress) {
    if(token.empty()||token.size()>4096||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<33||c>126;}))throw std::runtime_error("Invalid session token; reconnect the account");
    return collect_gateway_members(token,server,cancel,progress,true);
}
Json collect_server_counts(const std::string& token,const std::vector<std::string>& servers,const std::atomic<bool>& cancel,const ProgressCallback& progress) {
    if(token.empty()||token.size()>4096||std::any_of(token.begin(),token.end(),[](unsigned char c){return c<33||c>126;}))throw std::runtime_error("Invalid session token; reconnect the account");
    if(servers.empty()||servers.size()>20000)throw std::runtime_error("Choose 1 to 20,000 servers");
    for(const auto& server:servers)id(server);
    return collect_gateway_members(token,servers.front(),cancel,progress,true,servers);
}
void save_sessions(const Path& path,const Json& sessions) {
    std::string plain=sessions.dump();
    if(plain.size()>1024*1024)throw std::runtime_error("Session cache exceeded the supported size");
    DATA_BLOB input{static_cast<DWORD>(plain.size()),reinterpret_cast<BYTE*>(plain.data())},output{};
    BOOL result=CryptProtectData(&input,L"XA Discord Explorer sessions",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output);
    SecureZeroMemory(plain.data(),plain.size());
    if(!result)throw std::runtime_error("Windows could not encrypt the session cache");
    try {
        std::filesystem::create_directories(path.parent_path());
        auto temp=path;temp+=L".tmp-"+std::to_wstring(GetCurrentProcessId());
        {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file.write(reinterpret_cast<char*>(output.pbData),output.cbData);file.flush();
         if(!file)throw std::runtime_error("Could not save the encrypted session cache");}
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace the encrypted session cache");
    }catch(...){LocalFree(output.pbData);throw;}
    LocalFree(output.pbData);
}
Json load_sessions(const Path& path) {
    if(!std::filesystem::exists(path))return Json::object();
    if(std::filesystem::file_size(path)>2*1024*1024)throw std::runtime_error("Session cache is invalid");
    std::ifstream file(path,std::ios::binary);std::string encrypted((std::istreambuf_iterator<char>(file)),{});
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),reinterpret_cast<BYTE*>(encrypted.data())},output{};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))
        throw std::runtime_error("Windows cannot decrypt the saved sessions. Use the original Windows account or reconnect in a new workspace.");
    try {
        auto data=Json::parse(output.pbData,output.pbData+output.cbData);
        if(!data.is_object())throw std::runtime_error("Session cache is invalid");
        for(auto& [key,value]:data.items()) {
            if(id(key)!=key||!value.is_object()||!value.contains("token")||!value["token"].is_string()||!value.contains("username")||!value["username"].is_string())
                throw std::runtime_error("Session cache is invalid");
        }
        SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);return data;
    }catch(...){SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);throw std::runtime_error("Saved session data is invalid. Reconnect in a new workspace.");}
}
}
