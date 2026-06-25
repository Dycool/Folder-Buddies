// Folder Buddies — Cloudflare-blind share codes and self-contained offline blobs.
//
// The host seals the connection Token (ip/port/folder/data-path secret) once.
// The data-path secret is delivered to the client without the client ever typing
// a password: either embedded in a long offline blob, or wrapped behind the
// secret half of a short 6-char code that Cloudflare never receives.
#pragma once

#include "token.h"

#include <string>

namespace fb {

constexpr int kRoomCodeLength = 6;
constexpr int kLookupLen = 2;   // public half: the Cloudflare KV key
constexpr int kKeyPartLen = 4;  // secret half: never sent to Cloudflare
constexpr int kRoomTtlSeconds = 30 * 24 * 60 * 60;

struct HostedShareTicket {
    std::string roomCode;       // 6 Base91 chars when Cloudflare publish succeeds
    std::string offlineBlob;    // long self-contained Base91 blob
    std::string connectCode;    // roomCode if cloudPublished, otherwise offlineBlob
    std::string ownerToken;     // delete credential, not shown to the user
    std::string lookupId;       // Cloudflare KV key (public half of the code)
    std::string reach;
    std::string cloudStatus;
    bool cloudPublished = false;
};

// A sealed record ready to upload to Cloudflare. All fields are Base91 text.
struct CloudRecord {
    std::string lookupId;   // KV key
    std::string salt;       // Argon2id salt for the wrap key
    std::string wrapped;    // blobKey sealed under argon2id(keyPart, salt)
    std::string payload;    // the Token ciphertext bundle
    std::string owner;      // delete credential
};

std::string random_room_code();                       // 6 Base91 chars
bool looks_like_room_code(const std::string& text);   // exactly 6 clean Base91

// Seal `tok` into a self-contained offline blob (embeds its own key).
bool seal_for_offline(const Token& tok, std::string& offlineBlob, std::string& err);

// Seal `tok` for Cloudflare under `roomCode`. `ownerOut` is the delete credential.
bool seal_for_cloud(const Token& tok, const std::string& roomCode, CloudRecord& rec,
                    std::string& ownerOut, std::string& err);

// Open a long offline blob.
bool open_offline_blob(const std::string& blob, Token& out, std::string& err);

// Open a Cloudflare record fetched for `roomCode` (uses the secret half of the code).
bool open_cloud_record(const std::string& roomCode, const std::string& salt,
                       const std::string& wrapped, const std::string& payload,
                       Token& out, std::string& err);

class SignalingClient {
public:
    // Hardcoded at build time; overridable with FOLDERBUDDIES_SIGNALING_URL.
    static std::string base_url();
    static bool configured();

    bool create(const CloudRecord& rec, std::string& err);
    bool get(const std::string& lookupId, std::string& salt, std::string& wrapped,
             std::string& payload, std::string& err);
    bool remove(const std::string& lookupId, const std::string& owner, std::string& err);
};

} // namespace fb
