// Folder Buddies — Cloudflare-blind share codes and self-contained offline blobs.
//
// The host seals the connection Token (ip/port/folder/data-path secret) once.
// The data-path secret is delivered to the client without the client ever typing
// a password: either embedded in a long offline blob, or wrapped behind the
// secret half of a short room code that Cloudflare never receives.
#pragma once

#include "token.h"

#include <string>

namespace fb {

// Connect codes come in two tiers, told apart purely by total length so a
// connecting client needs no out-of-band hint:
//   * read-only (default): 4-char public lookup + 2-char secret half  ( 6 total)
//   * read-write:          8-char public lookup + 8-char secret half  (16 total)
// The host issues the long, ~52-bit tier exactly when it grants write access,
// because tampering is the higher-stakes capability; read-only stays short for
// convenience. The lookup is the public Cloudflare/Firebase key; the secret half
// is never sent to the server.
constexpr int kShortLookupLen = 4;
constexpr int kShortKeyPartLen = 2;
constexpr int kShortCodeLength = kShortLookupLen + kShortKeyPartLen;   // 6
constexpr int kLongLookupLen = 8;
constexpr int kLongKeyPartLen = 8;
constexpr int kLongCodeLength = kLongLookupLen + kLongKeyPartLen;      // 16
constexpr int kRoomTtlSeconds = 30 * 24 * 60 * 60;

struct HostedShareTicket {
    std::string roomCode;       // the issued connect code when Cloudflare publish succeeds
    std::string offlineBlob;    // long self-contained Base91 blob
    std::string connectCode;    // roomCode if cloudPublished, otherwise offlineBlob
    std::string ownerToken;     // delete credential, not shown to the user
    std::string lookupId;       // Cloudflare KV key (public half of the code)
    std::string reach;
    std::string cloudStatus;
    std::string signalingBackend; // "cloudflare" or "firebase" when published remotely
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

// Generate a connect code: the long read-write tier when `longCode` is set,
// otherwise the short read-only tier.
std::string random_room_code(bool longCode);
// True for a clean Base91 code of either tier (6 or 16 chars).
bool looks_like_room_code(const std::string& text);
// The public lookup half (Cloudflare/Firebase key) of a connect code.
std::string room_lookup_id(const std::string& code);

std::string encode_url_query_value(const std::string& value);

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

class FirebaseSignalingClient {
public:
    // Optional native fallback. Runtime env wins over build-time macro.
    // Example: https://PROJECT.europe-west1.firebasedatabase.app
    static std::string base_url();
    static bool configured();

    bool create(const CloudRecord& rec, std::string& err);
    bool get(const std::string& lookupId, std::string& salt, std::string& wrapped,
             std::string& payload, std::string& err);
    bool remove(const std::string& lookupId, const std::string& owner, std::string& err);
};

} // namespace fb
