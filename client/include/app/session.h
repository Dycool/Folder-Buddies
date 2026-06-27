// Folder Buddies — high-level host/mount orchestration shared by the GUI and CLI.
#pragma once

#include "client.h"
#include "fuse_fs.h"
#include "server.h"
#include "signaling.h"
#include "token.h"
#include "upnp.h"

#include <QString>
#include <string>

namespace fb {

bool start_hosting(Server& server, Upnp& upnp, const std::string& folder, int port,
                   bool lanOnly, bool allowWrites, HostedShareTicket& ticket, std::string& err);

bool remove_published_room(const HostedShareTicket& ticket, std::string& err);

bool resolve_share_code(const std::string& codeOrBlob, Token& tok, std::string& err);

bool start_mounting(Client& client, Mount& mount, const Token& tok,
                    std::string& mountpoint, std::string& err);

} // namespace fb
