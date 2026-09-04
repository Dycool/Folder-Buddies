use std::env;

use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use reqwest::Url;
use serde::Deserialize;
use serde_json::{Value, json};

use crate::signaling::{LONG_LOOKUP_LEN, SHORT_LOOKUP_LEN, base91_is_clean};

const MAX_SIGNAL_BYTES: usize = 96 * 1024;
const MIN_PEER_ID_BYTES: usize = 8;
const MAX_PEER_ID_BYTES: usize = 80;
const MIN_CIPHERTEXT_BYTES: usize = 20;
const PLAIN_PREFIX: &str = "plain:";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RoomRole {
    Host,
    Client,
}

impl RoomRole {
    #[must_use]
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Host => "host",
            Self::Client => "client",
        }
    }

    fn parse(value: &str) -> Result<Self, String> {
        match value {
            "host" => Ok(Self::Host),
            "client" => Ok(Self::Client),
            _ => Err("signaling event has an invalid role".to_owned()),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum RoomEvent {
    Ready {
        role: RoomRole,
        room: String,
        peer_id: Option<String>,
    },
    ClientJoined {
        peer_id: String,
    },
    ClientLeft {
        peer_id: String,
    },
    HostJoined,
    HostLeft,
    Signal {
        peer_id: String,
        payload: Value,
    },
    Error {
        error: String,
    },
}

#[derive(Debug, Deserialize)]
struct RawRoomEvent {
    kind: String,
    role: Option<String>,
    room: Option<String>,
    #[serde(rename = "peerId")]
    peer_id: Option<String>,
    ciphertext: Option<String>,
    error: Option<String>,
}

pub fn configured_room_websocket_url(
    lookup_id: &str,
    role: RoomRole,
) -> Result<Option<String>, String> {
    let base_url = env::var("FOLDERBUDDIES_SIGNALING_URL").unwrap_or_default();
    if base_url.trim().is_empty() {
        return Ok(None);
    }
    room_websocket_url(&base_url, lookup_id, role).map(Some)
}

pub fn room_websocket_url(
    base_url: &str,
    lookup_id: &str,
    role: RoomRole,
) -> Result<String, String> {
    validate_lookup_id(lookup_id)?;
    let mut url = Url::parse(base_url).map_err(|error| format!("invalid signaling URL: {error}"))?;
    if url.scheme() != "https" {
        return Err("signaling URL must use https://".to_owned());
    }
    url.set_scheme("wss")
        .map_err(|_| "failed to convert signaling URL to wss://".to_owned())?;
    let mut path = url.path().trim_end_matches('/').to_owned();
    path.push_str("/room");
    url.set_path(&path);
    url.set_query(None);
    url.set_fragment(None);
    url.query_pairs_mut()
        .append_pair("code", lookup_id)
        .append_pair("role", role.as_str())
        .append_pair("web", "1")
        .append_pair("compat", "native");
    Ok(url.to_string())
}

pub fn encode_signal_message(peer_id: &str, payload: &Value) -> Result<String, String> {
    validate_peer_id(peer_id)?;
    let ciphertext = encode_plain_payload(payload)?;
    if ciphertext.len() < MIN_CIPHERTEXT_BYTES || ciphertext.len() > MAX_SIGNAL_BYTES {
        return Err("signaling payload size is outside the worker limits".to_owned());
    }
    let message = serde_json::to_string(&json!({
        "kind": "signal",
        "peerId": peer_id,
        "ciphertext": ciphertext,
    }))
    .map_err(|error| error.to_string())?;
    if message.len() > MAX_SIGNAL_BYTES {
        return Err("signaling message exceeds the worker limit".to_owned());
    }
    Ok(message)
}

pub fn encode_plain_payload(payload: &Value) -> Result<String, String> {
    let encoded = serde_json::to_vec(payload).map_err(|error| error.to_string())?;
    let text = format!("{PLAIN_PREFIX}{}", URL_SAFE_NO_PAD.encode(encoded));
    if text.len() > MAX_SIGNAL_BYTES {
        return Err("signaling payload exceeds the worker limit".to_owned());
    }
    Ok(text)
}

pub fn decode_plain_payload(ciphertext: &str) -> Result<Value, String> {
    let encoded = ciphertext
        .strip_prefix(PLAIN_PREFIX)
        .ok_or_else(|| "unsupported signaling payload encoding".to_owned())?;
    let decoded = URL_SAFE_NO_PAD
        .decode(encoded)
        .map_err(|error| format!("invalid signaling Base64URL payload: {error}"))?;
    serde_json::from_slice(&decoded).map_err(|error| format!("invalid signaling JSON payload: {error}"))
}

pub fn parse_room_event(text: &str) -> Result<RoomEvent, String> {
    if text.len() > MAX_SIGNAL_BYTES {
        return Err("signaling event exceeds the worker limit".to_owned());
    }
    let raw: RawRoomEvent =
        serde_json::from_str(text).map_err(|error| format!("invalid signaling event: {error}"))?;
    match raw.kind.as_str() {
        "ready" => {
            let role = RoomRole::parse(
                raw.role
                    .as_deref()
                    .ok_or_else(|| "ready event is missing role".to_owned())?,
            )?;
            let room = raw
                .room
                .ok_or_else(|| "ready event is missing room".to_owned())?;
            validate_lookup_id(&room)?;
            if role == RoomRole::Client {
                validate_peer_id(
                    raw.peer_id
                        .as_deref()
                        .ok_or_else(|| "client ready event is missing peerId".to_owned())?,
                )?;
            }
            Ok(RoomEvent::Ready {
                role,
                room,
                peer_id: raw.peer_id,
            })
        }
        "client-joined" => Ok(RoomEvent::ClientJoined {
            peer_id: required_peer_id(raw.peer_id)?,
        }),
        "client-left" => Ok(RoomEvent::ClientLeft {
            peer_id: required_peer_id(raw.peer_id)?,
        }),
        "host-joined" => Ok(RoomEvent::HostJoined),
        "host-left" => Ok(RoomEvent::HostLeft),
        "signal" => {
            let peer_id = required_peer_id(raw.peer_id)?;
            let ciphertext = raw
                .ciphertext
                .ok_or_else(|| "signal event is missing ciphertext".to_owned())?;
            if ciphertext.len() < MIN_CIPHERTEXT_BYTES || ciphertext.len() > MAX_SIGNAL_BYTES {
                return Err("signal event ciphertext size is invalid".to_owned());
            }
            let payload = decode_plain_payload(&ciphertext)?;
            Ok(RoomEvent::Signal { peer_id, payload })
        }
        "error" => Ok(RoomEvent::Error {
            error: raw
                .error
                .filter(|value| !value.is_empty())
                .ok_or_else(|| "error event is missing error text".to_owned())?,
        }),
        _ => Err("unknown signaling event kind".to_owned()),
    }
}

fn required_peer_id(peer_id: Option<String>) -> Result<String, String> {
    let peer_id = peer_id.ok_or_else(|| "signaling event is missing peerId".to_owned())?;
    validate_peer_id(&peer_id)?;
    Ok(peer_id)
}

fn validate_peer_id(peer_id: &str) -> Result<(), String> {
    if (MIN_PEER_ID_BYTES..=MAX_PEER_ID_BYTES).contains(&peer_id.len()) {
        Ok(())
    } else {
        Err("signaling peerId length is invalid".to_owned())
    }
}

fn validate_lookup_id(lookup_id: &str) -> Result<(), String> {
    if !matches!(lookup_id.len(), SHORT_LOOKUP_LEN | LONG_LOOKUP_LEN)
        || lookup_id.chars().any(char::is_whitespace)
        || !base91_is_clean(lookup_id)
    {
        return Err("invalid signaling room lookup".to_owned());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn websocket_url_matches_native_compat_contract() {
        let url = room_websocket_url(
            "https://signal.example/api/",
            "A+B?",
            RoomRole::Client,
        )
        .expect("URL");
        assert_eq!(
            url,
            "wss://signal.example/api/room?code=A%2BB%3F&role=client&web=1&compat=native"
        );
    }

    #[test]
    fn signal_round_trip_uses_plain_payload_envelope() {
        let peer_id = "01234567-89ab-cdef";
        let payload = json!({
            "type": "native-quic-description",
            "description": "candidate-data"
        });
        let outbound = encode_signal_message(peer_id, &payload).expect("encode");
        let envelope: Value = serde_json::from_str(&outbound).expect("envelope");
        let incoming = json!({
            "kind": "signal",
            "peerId": peer_id,
            "ciphertext": envelope["ciphertext"]
        });
        assert_eq!(
            parse_room_event(&incoming.to_string()).expect("parse"),
            RoomEvent::Signal {
                peer_id: peer_id.to_owned(),
                payload,
            }
        );
    }

    #[test]
    fn parses_presence_events() {
        assert_eq!(
            parse_room_event(
                r#"{"kind":"ready","role":"client","room":"ABCD","peerId":"01234567"}"#
            )
            .expect("ready"),
            RoomEvent::Ready {
                role: RoomRole::Client,
                room: "ABCD".to_owned(),
                peer_id: Some("01234567".to_owned()),
            }
        );
        assert_eq!(
            parse_room_event(r#"{"kind":"host-left"}"#).expect("host-left"),
            RoomEvent::HostLeft
        );
    }

    #[test]
    fn rejects_invalid_room_and_peer_ids() {
        assert!(room_websocket_url("https://signal.example", "bad", RoomRole::Host).is_err());
        assert!(encode_signal_message("short", &json!({"type":"x"})).is_err());
    }
}
