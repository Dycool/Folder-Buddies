#include "session.h"

#include "netutil.h"

namespace fb {

bool start_hosting(Server& server, Upnp& upnp, const std::string& folder, int port,
                   int maxClients, bool lanOnly, HostedShareTicket& ticket,
                   std::string& err) {
    if (!server.start(folder, port, maxClients, err)) return false;

    std::string ip;
    uint16_t sharePort = server.boundPort;

    if (lanOnly) {
        ip = best_local_ip(/*lanOnly=*/true);
        ticket.reach = "LAN only — " + ip;
    } else if (std::string v6 = best_local_ip(/*lanOnly=*/false); !v6.empty()) {
        // A global IPv6 is reachable end-to-end with no NAT, so no UPnP needed.
        ip = v6;
        ticket.reach = "Internet (IPv6) — " + ip;
    } else {
        // No public IPv6: open the IPv4 port via UPnP and advertise that.
        std::string extIp, uerr;
        uint16_t extPort = 0;
        if (upnp.map(server.boundPort, extIp, extPort, uerr)) {
            ip = extIp;
            sharePort = extPort;
            ticket.reach = "Internet (IPv4/UPnP) — " + extIp + " :" + std::to_string(extPort);
        } else {
            ip = best_local_ip(/*lanOnly=*/true); // fall back to a LAN-reachable address
            if (ip.empty()) ip = "127.0.0.1";
            ticket.reach = "UPnP failed (" + uerr + ") — only reachable on LAN: " + ip;
        }
    }

    Token tok;
    tok.ip = ip;
    tok.port = sharePort;
    tok.secret = server.secret;
    tok.folder = server.shareName;

    ticket.password = random_room_password();

    std::string e;
    ticket.offlineBlob = encrypt_room_payload(tok, ticket.password, /*aadRoomCode=*/"offline", e);
    if (ticket.offlineBlob.empty()) {
        err = "failed to create encrypted offline room blob: " + e;
        return false;
    }

    SignalingClient sig;
    ticket.cloudPublished = false;
    if (SignalingClient::configured()) {
        for (int attempt = 0; attempt < 12; ++attempt) {
            std::string room = random_room_code();
            std::string payload = encrypt_room_payload(tok, ticket.password, room, e);
            if (payload.empty()) { ticket.cloudStatus = e; continue; }
            if (sig.create_room(room, ticket.password, payload, e)) {
                ticket.roomCode = room;
                ticket.connectCode = room;
                ticket.cloudPublished = true;
                ticket.cloudStatus = "Cloudflare room published; KV expires passively in 30 days";
                break;
            }
            ticket.cloudStatus = e;
        }
    } else {
        ticket.cloudStatus = "Cloudflare signaling URL is not configured; using offline mode";
    }

    if (!ticket.cloudPublished) {
        ticket.connectCode = ticket.offlineBlob;
        if (ticket.cloudStatus.empty()) ticket.cloudStatus = "Cloudflare unavailable; using offline mode";
    }
    return true;
}

bool resolve_share_code(const std::string& codeOrBlob, const std::string& password,
                        Token& tok, std::string& err) {
    if (password.empty()) { err = "password is required"; return false; }
    if (looks_like_room_code(codeOrBlob)) {
        std::string payload;
        SignalingClient sig;
        if (!sig.get_room(codeOrBlob, password, payload, err)) return false;
        return decrypt_room_payload(payload, password, codeOrBlob, tok, err);
    }
    return decrypt_room_payload(codeOrBlob, password, "offline", tok, err);
}

bool start_mounting(Client& client, Mount& mount, const Token& tok, const std::string& mountBase,
                    int nconns, std::string& mountpoint, std::string& err) {
    if (!client.connect(tok, nconns, err)) return false;
    if (!mount.start(&client, mountBase, tok.folder, err)) {
        client.disconnect();
        return false;
    }
    mountpoint = mount.mountpoint();
    return true;
}

} // namespace fb
