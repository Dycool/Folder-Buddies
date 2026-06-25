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

// Start sharing `folder`. The returned ticket contains either a 6-character
// Cloudflare room code or, if the Worker is unavailable/quota-exhausted, a long
// self-contained offline Base91 blob. Neither requires a separate password.
bool start_hosting(Server& server, Upnp& upnp, const std::string& folder, int port,
                   int maxClients, bool lanOnly, bool allowWrites, bool secureHash,
                   HostedShareTicket& ticket, std::string& err);

// Remove a remotely-published room from the backend that created it. Offline/secure-hash
// tickets have no remote state, so this is a no-op.
bool remove_published_room(const HostedShareTicket& ticket, std::string& err);

// Resolve a user-entered connect code. Exactly 6 clean Base91 chars trigger the
// Worker GET path; any longer Base91 string is opened locally as an offline blob.
bool resolve_share_code(const std::string& codeOrBlob, Token& tok, std::string& err);

// Connect to and mount the share described by `tok` under `mountBase`. On
// success `mountpoint` is the full path/volume the folder appears at.
bool start_mounting(Client& client, Mount& mount, const Token& tok, const std::string& mountBase,
                    int nconns, std::string& mountpoint, std::string& err);

} // namespace fb
