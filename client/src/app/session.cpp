#include "session.h"

#include "netutil.h"

namespace fb {

bool start_hosting(Server& server, Upnp& upnp, const std::string& folder, int port,
                   bool lanOnly, bool allowWrites, HostedShareTicket& ticket, std::string& err) {
    server.allowWrites = allowWrites;
    if (!server.start(folder, port, err)) return false;

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
    tok.allowWrites = allowWrites;

    std::string e;
    if (!seal_for_offline(tok, ticket.offlineBlob, e) || ticket.offlineBlob.empty()) {
        err = "failed to create offline room blob: " + e;
        return false;
    }

    ticket.cloudPublished = false;
    SignalingClient sig;
    if (SignalingClient::configured()) {
        for (int attempt = 0; attempt < 12; ++attempt) {
            std::string room = random_room_code(allowWrites);
            CloudRecord rec;
            std::string owner;
            if (!seal_for_cloud(tok, room, rec, owner, e)) { ticket.cloudStatus = e; continue; }
            if (sig.create(rec, e)) {
                ticket.roomCode = room;
                ticket.connectCode = room;
                ticket.lookupId = rec.lookupId;
                ticket.ownerToken = owner;
                ticket.signalingBackend = "cloudflare";
                ticket.cloudPublished = true;
                break;
            }
            ticket.cloudStatus = e;
        }
    }

    if (!ticket.cloudPublished && FirebaseSignalingClient::configured()) {
        FirebaseSignalingClient fb;
        for (int attempt = 0; attempt < 12; ++attempt) {
            std::string room = random_room_code(allowWrites);
            CloudRecord rec;
            std::string owner;
            if (!seal_for_cloud(tok, room, rec, owner, e)) { continue; }
            if (fb.create(rec, e)) {
                ticket.roomCode = room;
                ticket.connectCode = room;
                ticket.lookupId = rec.lookupId;
                ticket.ownerToken = owner;
                ticket.signalingBackend = "firebase";
                ticket.cloudPublished = true;
                break;
            }
        }
    }

    if (!ticket.cloudPublished) {
        ticket.connectCode = ticket.offlineBlob;
    }
    return true;
}

bool remove_published_room(const HostedShareTicket& ticket, std::string& err) {
    if (!ticket.cloudPublished) return true;
    if (ticket.signalingBackend == "firebase")
        return FirebaseSignalingClient().remove(ticket.lookupId, ticket.ownerToken, err);
    return SignalingClient().remove(ticket.lookupId, ticket.ownerToken, err);
}

bool resolve_share_code(const std::string& codeOrBlob, Token& tok, std::string& err) {
    if (codeOrBlob.rfind("FBS2:", 0) == 0 || codeOrBlob.rfind("FBW2O:", 0) == 0 || codeOrBlob.rfind("FBW2A:", 0) == 0) {
        err = "that is a web-browser WebRTC code. Native clients need a native room code (6 or 16 chars) or native offline Base91 blob.";
        return false;
    }
    if (looks_like_room_code(codeOrBlob)) {
        std::string lookupId = room_lookup_id(codeOrBlob);
        std::string salt, wrapped, payload;
        std::string cloudErr, firebaseErr;
        SignalingClient sig;
        if (sig.get(lookupId, salt, wrapped, payload, cloudErr) &&
            open_cloud_record(codeOrBlob, salt, wrapped, payload, tok, err)) {
            return true;
        }
        FirebaseSignalingClient fb;
        if (fb.get(lookupId, salt, wrapped, payload, firebaseErr) &&
            open_cloud_record(codeOrBlob, salt, wrapped, payload, tok, err)) {
            return true;
        }
        err = "room lookup failed. Cloudflare: " + cloudErr + "; Firebase: " + firebaseErr;
        return false;
    }
    return open_offline_blob(codeOrBlob, tok, err);
}

bool start_mounting(Client& client, Mount& mount, const Token& tok,
                    std::string& mountpoint, std::string& err) {
    if (!client.connect(tok, kDefaultConns, err)) return false;
    if (!mount.start(&client, "", tok.folder, tok.allowWrites, err)) {
        client.disconnect();
        return false;
    }
    mountpoint = mount.mountpoint();
    return true;
}

} // namespace fb
