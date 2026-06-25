// Folder Buddies — high-level host/mount orchestration shared by the GUI and
// the CLI. Keeping it in one place means both front-ends pick an available
// port, drive UPnP, publish the zero-knowledge room and mount identically.
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
// offline encrypted Base91 blob. The same strong password decrypts either form.
bool start_hosting(Server& server, Upnp& upnp, const std::string& folder, int port,
                   int maxClients, bool lanOnly, HostedShareTicket& ticket,
                   std::string& err);

// Resolve a user-entered connect code. Exactly 6 clean Base91 chars trigger the
// Worker GET path; any longer Base91 string is decrypted locally as offline mode.
bool resolve_share_code(const std::string& codeOrBlob, const std::string& password,
                        Token& tok, std::string& err);

// Connect to and mount the share described by `tok` under `mountBase`. On
// success `mountpoint` is the full path/volume the folder appears at.
bool start_mounting(Client& client, Mount& mount, const Token& tok, const std::string& mountBase,
                    int nconns, std::string& mountpoint, std::string& err);

} // namespace fb
