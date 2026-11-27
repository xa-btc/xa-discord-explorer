#pragma once
#include "chat.h"

namespace sml {
using AuditPage=std::function<void(Json rows)>;
Json audit_servers(const ChatGet& get,const std::string& account,const std::atomic<bool>& cancel);
Json audit_message_preview(const ChatGet& get,const std::string& guild,const std::string& channel,const std::string& message);
void scan_audit_log(const ChatGet& get,const std::string& server,const std::atomic<bool>& cancel,const AuditPage& publish,const std::vector<int>& actions={});
const std::map<int,std::string>& audit_action_names();
std::string audit_action_name(int action);
}
