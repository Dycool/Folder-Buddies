// Folder Buddies — zero-knowledge Cloudflare signaling and offline room blobs.
//
// The Worker only stores:
//   key   = 6-character Base91 room code
//   value = encrypted payload + password-derived verifier
// It never receives the plaintext IP/port/folder/session secret. The client
// resolves a 6-char code through the Worker, or decrypts a long Base91 blob
// locally when offline mode is used.
#pragma once

#include "token.h"

#include <optional>
#include <string>

namespace fb {

constexpr int kRoomCodeLength = 6;
constexpr int kRoomTtlSeconds = 30 * 24 * 60 * 60;

struct HostedShareTicket {
    std::string roomCode;       // exactly 6 Base91 chars when Cloudflare publish succeeds
    std::string password;       // strong random password; never sent plaintext to Cloudflare
    std::string offlineBlob;    // massive Base91 encrypted fallback blob
    std::string connectCode;    // roomCode if cloudPublished, otherwise offlineBlob
    std::string reach;
    std::string cloudStatus;
    bool cloudPublished = false;
};

std::string random_room_code();
std::string random_room_password();
bool looks_like_room_code(const std::string& text);

// Payload encryption used for both Worker KV values and offline fallback blobs.
std::string encrypt_room_payload(const Token& tok, const std::string& password,
                                 const std::string& aadRoomCode, std::string& err);
bool decrypt_room_payload(const std::string& encryptedBase91, const std::string& password,
                          const std::string& aadRoomCode, Token& out, std::string& err);

// Password-derived proof sent to the Worker. The Worker stores the verifier,
// not the plaintext password, and GET/DELETE send only an HMAC proof.
std::string worker_auth_verifier(const std::string& password);
std::string worker_auth_proof(const std::string& password, const std::string& method,
                              const std::string& roomCode);

class SignalingClient {
public:
    // The Cloudflare Worker URL is intentionally centralized here so you can
    // hardcode your deployed endpoint once. It can also be overridden for local
    // tests with FOLDERBUDDIES_SIGNALING_URL.
    static std::string base_url();
    static bool configured();

    bool create_room(const std::string& roomCode, const std::string& password,
                     const std::string& encryptedPayload, std::string& err);
    bool get_room(const std::string& roomCode, const std::string& password,
                  std::string& encryptedPayload, std::string& err);
    bool delete_room(const std::string& roomCode, const std::string& password, std::string& err);
};

} // namespace fb
