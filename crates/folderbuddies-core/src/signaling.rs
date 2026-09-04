use std::{env, time::Duration};

use argon2::{Algorithm, Argon2, Params, Version};
use base64::{Engine as _, engine::general_purpose::URL_SAFE_NO_PAD};
use percent_encoding::{NON_ALPHANUMERIC, utf8_percent_encode};
use reqwest::blocking::Client as HttpClient;
use serde::{Deserialize, Serialize};
use serde_json::Value;

use crate::{
    crypto::{Key256, aead_open, aead_seal, random_array, random_bytes},
    protocol::{Reader, Writer},
};

pub const SHORT_LOOKUP_LEN: usize = 4;
pub const SHORT_SECRET_LEN: usize = 2;
pub const SHORT_CODE_LEN: usize = SHORT_LOOKUP_LEN + SHORT_SECRET_LEN;
pub const LONG_LOOKUP_LEN: usize = 8;
pub const LONG_SECRET_LEN: usize = 8;
pub const LONG_CODE_LEN: usize = LONG_LOOKUP_LEN + LONG_SECRET_LEN;
pub const ROOM_TTL_SECONDS: u64 = 30 * 24 * 60 * 60;
pub const SECRET_BYTES: usize = 32;

const BASE91_ALPHABET: &[u8; 91] =
    b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+,-./:;<=>?@[]^_`{|}~";
const PAYLOAD_MAGIC: &[u8; 5] = b"FBZK1";
const OFFLINE_MAGIC: &[u8; 5] = b"FBOF1";
const PAYLOAD_VERSION: u32 = 2;
const ARGON_MEMORY_KIB: u32 = 64 * 1024;
const ARGON_ITERATIONS: u32 = 3;
const ARGON_SALT_LEN: usize = 16;
const HTTP_TIMEOUT: Duration = Duration::from_secs(10);
const HARDCODED_SIGNALING_URL: &str = match option_env!("FB_SIGNALING_URL") {
    Some(value) => value,
    None => "",
};
const HARDCODED_FIREBASE_URL: &str = match option_env!("FB_FIREBASE_DATABASE_URL") {
    Some(value) => value,
    None => "",
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    ip: String,
    port: u16,
    secret: Vec<u8>,
    folder: String,
    allow_writes: bool,
}

impl Token {
    pub fn new(
        ip: String,
        port: u16,
        secret: Vec<u8>,
        folder: String,
        allow_writes: bool,
    ) -> Result<Self, String> {
        if ip.is_empty() || port == 0 || secret.is_empty() {
            return Err("token fields must not be empty".to_owned());
        }
        Ok(Self {
            ip,
            port,
            secret,
            folder,
            allow_writes,
        })
    }

    #[must_use]
    pub fn ip(&self) -> &str {
        &self.ip
    }

    #[must_use]
    pub const fn port(&self) -> u16 {
        self.port
    }

    #[must_use]
    pub fn secret(&self) -> &[u8] {
        &self.secret
    }

    #[must_use]
    pub fn folder(&self) -> &str {
        &self.folder
    }

    #[must_use]
    pub const fn allow_writes(&self) -> bool {
        self.allow_writes
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct CloudRecord {
    #[serde(rename = "lookup")]
    lookup_id: String,
    salt: String,
    wrapped: String,
    payload: String,
    owner: String,
}

impl CloudRecord {
    #[must_use]
    pub fn lookup_id(&self) -> &str {
        &self.lookup_id
    }

    #[must_use]
    pub fn owner(&self) -> &str {
        &self.owner
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct HostedShareTicket {
    room_code: String,
    offline_blob: String,
    connect_code: String,
    owner_token: String,
    lookup_id: String,
    reach: String,
    cloud_status: String,
    signaling_backend: String,
    cloud_published: bool,
}

impl HostedShareTicket {
    #[must_use]
    pub fn connect_code(&self) -> &str {
        &self.connect_code
    }

    #[must_use]
    pub fn room_code(&self) -> &str {
        &self.room_code
    }

    #[must_use]
    pub fn offline_blob(&self) -> &str {
        &self.offline_blob
    }

    #[must_use]
    pub fn reach(&self) -> &str {
        &self.reach
    }

    #[must_use]
    pub fn cloud_status(&self) -> &str {
        &self.cloud_status
    }

    #[must_use]
    pub fn signaling_backend(&self) -> &str {
        &self.signaling_backend
    }

    #[must_use]
    pub const fn cloud_published(&self) -> bool {
        self.cloud_published
    }

    pub fn set_reach(&mut self, reach: String) {
        self.reach = reach;
    }
}

#[derive(Debug, Serialize)]
struct CloudCreate<'a> {
    lookup: &'a str,
    salt: &'a str,
    wrapped: &'a str,
    payload: &'a str,
    owner: &'a str,
    ttl: u64,
}

#[derive(Debug, Deserialize)]
struct CloudGet {
    salt: String,
    wrapped: String,
    payload: String,
}

#[derive(Debug, Serialize)]
struct FirebaseRecord {
    v: u32,
    lookup: String,
    salt: String,
    wrapped: String,
    payload: String,
    owner: String,
    #[serde(rename = "createdAt")]
    created_at: i64,
}

#[must_use]
pub fn base91_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() * 123) / 100 + 8);
    let mut accumulator = 0_u32;
    let mut bits = 0_u32;
    for &byte in data {
        accumulator |= u32::from(byte) << bits;
        bits += 8;
        if bits > 13 {
            let mut value = accumulator & 8191;
            if value > 88 {
                accumulator >>= 13;
                bits -= 13;
            } else {
                value = accumulator & 16383;
                accumulator >>= 14;
                bits -= 14;
            }
            out.push(char::from(BASE91_ALPHABET[(value % 91) as usize]));
            out.push(char::from(BASE91_ALPHABET[(value / 91) as usize]));
        }
    }
    if bits != 0 {
        out.push(char::from(BASE91_ALPHABET[(accumulator % 91) as usize]));
        if bits > 7 || accumulator > 90 {
            out.push(char::from(BASE91_ALPHABET[(accumulator / 91) as usize]));
        }
    }
    out
}

pub fn base91_decode(text: &str) -> Result<Vec<u8>, String> {
    let mut table = [-1_i16; 256];
    for (index, byte) in BASE91_ALPHABET.iter().enumerate() {
        table[usize::from(*byte)] =
            i16::try_from(index).map_err(|_| "Base91 alphabet index overflow".to_owned())?;
    }

    let mut out = Vec::with_capacity((text.len() * 100) / 123 + 8);
    let mut pending = -1_i32;
    let mut accumulator = 0_u32;
    let mut bits = 0_u32;
    for byte in text.bytes() {
        if byte.is_ascii_whitespace() {
            continue;
        }
        let value = table[usize::from(byte)];
        if value < 0 {
            return Err("invalid Base91 character".to_owned());
        }
        if pending < 0 {
            pending = i32::from(value);
            continue;
        }

        pending += i32::from(value) * 91;
        let combined = u32::try_from(pending).map_err(|_| "invalid Base91 state".to_owned())?;
        accumulator |= combined << bits;
        bits += if (combined & 8191) > 88 { 13 } else { 14 };
        while bits > 7 {
            out.push((accumulator & 0xff) as u8);
            accumulator >>= 8;
            bits -= 8;
        }
        pending = -1;
    }
    if pending >= 0 {
        let pending = u32::try_from(pending).map_err(|_| "invalid Base91 tail".to_owned())?;
        out.push(((accumulator | (pending << bits)) & 0xff) as u8);
    }
    Ok(out)
}

#[must_use]
pub fn base91_is_clean(text: &str) -> bool {
    text.bytes()
        .all(|byte| byte.is_ascii_whitespace() || BASE91_ALPHABET.contains(&byte))
}

pub fn random_room_code(long_code: bool) -> Result<String, String> {
    let len = if long_code {
        LONG_CODE_LEN
    } else {
        SHORT_CODE_LEN
    };
    let mut code = String::with_capacity(len);
    let limit = 256 - (256 % BASE91_ALPHABET.len());
    while code.len() < len {
        for byte in random_bytes(len)? {
            if usize::from(byte) >= limit {
                continue;
            }
            code.push(char::from(
                BASE91_ALPHABET[usize::from(byte) % BASE91_ALPHABET.len()],
            ));
            if code.len() == len {
                break;
            }
        }
    }
    Ok(code)
}

#[must_use]
pub fn looks_like_room_code(text: &str) -> bool {
    let clean: String = text
        .chars()
        .filter(|character| !character.is_whitespace())
        .collect();
    matches!(clean.len(), SHORT_CODE_LEN | LONG_CODE_LEN) && base91_is_clean(&clean)
}

#[must_use]
pub fn room_lookup_id(code: &str) -> String {
    match code_split(code.len()) {
        Some((lookup, _)) => code[..lookup].to_owned(),
        None => code.to_owned(),
    }
}

pub fn seal_for_offline(token: &Token) -> Result<String, String> {
    let (blob_key, bundle) = seal_token(token)?;
    let mut blob = Vec::with_capacity(OFFLINE_MAGIC.len() + blob_key.len() + bundle.len());
    blob.extend_from_slice(OFFLINE_MAGIC);
    blob.extend_from_slice(&blob_key);
    blob.extend_from_slice(&bundle);
    Ok(base91_encode(&blob))
}

pub fn open_offline_blob(blob: &str) -> Result<Token, String> {
    let raw = base91_decode(blob).map_err(|_| "not a valid offline blob".to_owned())?;
    let header = OFFLINE_MAGIC.len() + 32;
    if raw.len() < header || raw.get(..OFFLINE_MAGIC.len()) != Some(OFFLINE_MAGIC) {
        return Err("not a valid offline blob".to_owned());
    }
    let key: Key256 = raw[OFFLINE_MAGIC.len()..header]
        .try_into()
        .map_err(|_| "not a valid offline blob".to_owned())?;
    open_bundle(&key, &raw[header..])
}

pub fn seal_for_cloud(token: &Token, room_code: &str) -> Result<CloudRecord, String> {
    let (lookup_len, key_len) =
        code_split(room_code.len()).ok_or_else(|| "bad room code".to_owned())?;
    let key_part = &room_code[lookup_len..lookup_len + key_len];
    let (blob_key, bundle) = seal_token(token)?;
    let salt: [u8; ARGON_SALT_LEN] = random_array()?;
    let wrap_key = argon2id_key(key_part.as_bytes(), &salt)?;
    let nonce: [u8; 12] = random_array()?;
    let wrapped_key = aead_seal(&wrap_key, &nonce, &blob_key)?;
    let mut wrapped = Vec::with_capacity(nonce.len() + wrapped_key.len());
    wrapped.extend_from_slice(&nonce);
    wrapped.extend_from_slice(&wrapped_key);
    Ok(CloudRecord {
        lookup_id: room_code[..lookup_len].to_owned(),
        salt: base91_encode(&salt),
        wrapped: base91_encode(&wrapped),
        payload: base91_encode(&bundle),
        owner: base91_encode(&random_bytes(16)?),
    })
}

pub fn open_cloud_record(
    room_code: &str,
    salt_text: &str,
    wrapped_text: &str,
    payload_text: &str,
) -> Result<Token, String> {
    let (lookup_len, key_len) =
        code_split(room_code.len()).ok_or_else(|| "bad room code".to_owned())?;
    let key_part = &room_code[lookup_len..lookup_len + key_len];
    let salt = base91_decode(salt_text).map_err(|_| "bad record salt".to_owned())?;
    if salt.len() != ARGON_SALT_LEN {
        return Err("bad record salt".to_owned());
    }
    let wrapped = base91_decode(wrapped_text).map_err(|_| "bad wrapped key".to_owned())?;
    if wrapped.len() != 12 + 32 + 16 {
        return Err("bad wrapped key".to_owned());
    }
    let bundle = base91_decode(payload_text)?;
    let wrap_key = argon2id_key(key_part.as_bytes(), &salt)?;
    let nonce: [u8; 12] = wrapped[..12]
        .try_into()
        .map_err(|_| "bad wrapped key".to_owned())?;
    let blob_key_plain =
        aead_open(&wrap_key, &nonce, &wrapped[12..]).map_err(|_| "wrong code".to_owned())?;
    let blob_key: Key256 = blob_key_plain
        .as_slice()
        .try_into()
        .map_err(|_| "bad wrapped key".to_owned())?;
    open_bundle(&blob_key, &bundle)
}

pub struct SignalingClient {
    http: HttpClient,
    base_url: String,
}

impl SignalingClient {
    pub fn from_environment() -> Result<Option<Self>, String> {
        let base_url = env::var("FOLDERBUDDIES_SIGNALING_URL")
            .ok()
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| HARDCODED_SIGNALING_URL.to_owned());
        let mut base_url = base_url;
        trim_trailing_slashes(&mut base_url);
        if base_url.is_empty() || !base_url.starts_with("https://") {
            return Ok(None);
        }
        Ok(Some(Self {
            http: http_client()?,
            base_url,
        }))
    }

    pub fn new(mut base_url: String) -> Result<Option<Self>, String> {
        trim_trailing_slashes(&mut base_url);
        if base_url.is_empty() {
            return Ok(None);
        }
        require_https(&base_url, "signaling URL")?;
        Ok(Some(Self {
            http: http_client()?,
            base_url,
        }))
    }

    pub fn create(&self, record: &CloudRecord) -> Result<(), String> {
        let request = CloudCreate {
            lookup: &record.lookup_id,
            salt: &record.salt,
            wrapped: &record.wrapped,
            payload: &record.payload,
            owner: &record.owner,
            ttl: ROOM_TTL_SECONDS,
        };
        let response = self
            .http
            .post(format!("{}/create", self.base_url))
            .json(&request)
            .send()
            .map_err(|error| error.to_string())?;
        let status = response.status().as_u16();
        if status == 200 || status == 201 {
            return Ok(());
        }
        if status == 409 {
            return Err("room code collision".to_owned());
        }
        let body = response.text().unwrap_or_default();
        Err(format!("Cloudflare create failed (HTTP {status}): {body}"))
    }

    pub fn get(&self, lookup_id: &str) -> Result<(String, String, String), String> {
        let code = utf8_percent_encode(lookup_id, NON_ALPHANUMERIC);
        let response = self
            .http
            .get(format!("{}/room?code={code}", self.base_url))
            .send()
            .map_err(|error| error.to_string())?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(match status {
                404 => "no share found for that code".to_owned(),
                429 => "rate limited by signaling server; wait a minute and try again".to_owned(),
                _ => format!("Cloudflare room lookup failed (HTTP {status})"),
            });
        }
        let record: CloudGet = response
            .json()
            .map_err(|_| "Cloudflare returned invalid JSON".to_owned())?;
        if record.salt.is_empty() || record.wrapped.is_empty() || record.payload.is_empty() {
            return Err("Cloudflare returned an incomplete record".to_owned());
        }
        Ok((record.salt, record.wrapped, record.payload))
    }

    pub fn remove(&self, lookup_id: &str, owner: &str) -> Result<(), String> {
        let code = utf8_percent_encode(lookup_id, NON_ALPHANUMERIC);
        let response = self
            .http
            .delete(format!("{}/room?code={code}", self.base_url))
            .header("X-FB-Owner", owner)
            .send()
            .map_err(|error| error.to_string())?;
        match response.status().as_u16() {
            200 | 204 | 404 => Ok(()),
            status => Err(format!("Cloudflare room delete failed (HTTP {status})")),
        }
    }
}

pub struct FirebaseSignalingClient {
    http: HttpClient,
    base_url: String,
}

impl FirebaseSignalingClient {
    pub fn from_environment() -> Result<Option<Self>, String> {
        let base_url = env::var("FOLDERBUDDIES_FIREBASE_DATABASE_URL")
            .ok()
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| HARDCODED_FIREBASE_URL.to_owned());
        let mut base_url = base_url;
        trim_trailing_slashes(&mut base_url);
        if base_url.is_empty()
            || !base_url.starts_with("https://")
            || !(base_url.contains("firebasedatabase.app") || base_url.contains("firebaseio.com"))
        {
            return Ok(None);
        }
        Ok(Some(Self {
            http: http_client()?,
            base_url,
        }))
    }

    pub fn new(mut base_url: String) -> Result<Option<Self>, String> {
        trim_trailing_slashes(&mut base_url);
        if base_url.is_empty() {
            return Ok(None);
        }
        require_https(&base_url, "Firebase fallback URL")?;
        if !(base_url.contains("firebasedatabase.app") || base_url.contains("firebaseio.com")) {
            return Err("Firebase fallback URL is invalid".to_owned());
        }
        Ok(Some(Self {
            http: http_client()?,
            base_url,
        }))
    }

    fn room_url(&self, lookup_id: &str) -> String {
        let safe = URL_SAFE_NO_PAD.encode(lookup_id.as_bytes());
        format!("{}/nativeRooms/{safe}.json", self.base_url)
    }

    pub fn create(&self, record: &CloudRecord) -> Result<(), String> {
        let url = self.room_url(&record.lookup_id);
        let existing = self
            .http
            .get(&url)
            .send()
            .map_err(|error| error.to_string())?;
        let status = existing.status().as_u16();
        if status != 200 {
            return Err(format!("Firebase fallback room check failed (HTTP {status})"));
        }
        if existing.text().map_err(|error| error.to_string())?.trim() != "null" {
            return Err("Firebase fallback room code collision".to_owned());
        }

        let record = FirebaseRecord {
            v: 1,
            lookup: record.lookup_id.clone(),
            salt: record.salt.clone(),
            wrapped: record.wrapped.clone(),
            payload: record.payload.clone(),
            owner: record.owner.clone(),
            created_at: unix_seconds()?,
        };
        let response = self
            .http
            .put(&url)
            .json(&record)
            .send()
            .map_err(|error| error.to_string())?;
        let status = response.status().as_u16();
        if status == 200 {
            Ok(())
        } else {
            let body = response.text().unwrap_or_default();
            Err(format!(
                "Firebase fallback create failed (HTTP {status}): {body}"
            ))
        }
    }

    pub fn get(&self, lookup_id: &str) -> Result<(String, String, String), String> {
        let response = self
            .http
            .get(self.room_url(lookup_id))
            .send()
            .map_err(|error| error.to_string())?;
        let status = response.status().as_u16();
        if status != 200 {
            return Err(format!("Firebase fallback lookup failed (HTTP {status})"));
        }
        let text = response.text().map_err(|error| error.to_string())?;
        if text.trim() == "null" {
            return Err("no Firebase fallback share found for that code".to_owned());
        }
        let value: Value = serde_json::from_str(&text)
            .map_err(|_| "Firebase fallback returned invalid JSON".to_owned())?;
        let object = value
            .as_object()
            .ok_or_else(|| "Firebase fallback returned invalid JSON".to_owned())?;
        let created_at_raw = object
            .get("createdAt")
            .and_then(Value::as_f64)
            .unwrap_or(0.0);
        let created_at_ms = firebase_created_at_ms(created_at_raw);
        let now = unix_millis()?;
        let max_age = i64::try_from(ROOM_TTL_SECONDS)
            .map_err(|_| "TTL overflow".to_owned())?
            .saturating_mul(1000);
        if created_at_ms <= 0 || created_at_ms < now.saturating_sub(max_age) {
            let mut ignored = String::new();
            if let Err(error) = self.remove(lookup_id, "") {
                ignored = error;
            }
            drop(ignored);
            return Err("Firebase fallback room expired".to_owned());
        }
        let salt = object
            .get("salt")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned();
        let wrapped = object
            .get("wrapped")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned();
        let payload = object
            .get("payload")
            .and_then(Value::as_str)
            .unwrap_or_default()
            .to_owned();
        if salt.is_empty() || wrapped.is_empty() || payload.is_empty() {
            return Err("Firebase fallback returned an incomplete record".to_owned());
        }
        Ok((salt, wrapped, payload))
    }

    pub fn remove(&self, lookup_id: &str, _owner: &str) -> Result<(), String> {
        let deleted = self
            .http
            .delete(self.room_url(lookup_id))
            .send()
            .map_err(|error| error.to_string())?;
        match deleted.status().as_u16() {
            200 | 204 => Ok(()),
            status => Err(format!("Firebase fallback delete failed (HTTP {status})")),
        }
    }
}

pub fn resolve_share_code(code_or_blob: &str) -> Result<Token, String> {
    if code_or_blob.starts_with("FBS2:")
        || code_or_blob.starts_with("FBW2O:")
        || code_or_blob.starts_with("FBW2A:")
    {
        return Err(
            "that is a web-browser WebRTC code. Native clients need a native room code (6 or 16 chars) or native offline Base91 blob."
                .to_owned(),
        );
    }
    if !looks_like_room_code(code_or_blob) {
        return open_offline_blob(code_or_blob);
    }

    let lookup = room_lookup_id(code_or_blob);
    let cloud_error = match SignalingClient::from_environment()? {
        Some(client) => match client.get(&lookup) {
            Ok((salt, wrapped, payload)) => {
                return open_cloud_record(code_or_blob, &salt, &wrapped, &payload);
            }
            Err(error) => error,
        },
        None => "Cloudflare signaling URL is not configured".to_owned(),
    };
    let firebase_error = match FirebaseSignalingClient::from_environment()? {
        Some(client) => match client.get(&lookup) {
            Ok((salt, wrapped, payload)) => {
                return open_cloud_record(code_or_blob, &salt, &wrapped, &payload);
            }
            Err(error) => error,
        },
        None => "Firebase fallback URL is not configured".to_owned(),
    };
    Err(format!(
        "room lookup failed. Cloudflare: {cloud_error}; Firebase: {firebase_error}"
    ))
}

pub fn publish_share(token: &Token, reach: String) -> Result<HostedShareTicket, String> {
    let offline_blob = seal_for_offline(token)?;
    let mut ticket = HostedShareTicket {
        offline_blob: offline_blob.clone(),
        connect_code: offline_blob,
        reach,
        ..HostedShareTicket::default()
    };

    if let Some(client) = SignalingClient::from_environment()? {
        for _ in 0..12 {
            let room = random_room_code(token.allow_writes())?;
            let record = seal_for_cloud(token, &room)?;
            match client.create(&record) {
                Ok(()) => {
                    fill_published_ticket(&mut ticket, room, &record, "cloudflare");
                    return Ok(ticket);
                }
                Err(error) => ticket.cloud_status = error,
            }
        }
    }
    if let Some(client) = FirebaseSignalingClient::from_environment()? {
        for _ in 0..12 {
            let room = random_room_code(token.allow_writes())?;
            let record = seal_for_cloud(token, &room)?;
            match client.create(&record) {
                Ok(()) => {
                    fill_published_ticket(&mut ticket, room, &record, "firebase");
                    return Ok(ticket);
                }
                Err(error) => ticket.cloud_status = error,
            }
        }
    }
    Ok(ticket)
}

pub fn remove_published_room(ticket: &HostedShareTicket) -> Result<(), String> {
    if !ticket.cloud_published {
        return Ok(());
    }
    if ticket.signaling_backend == "firebase" {
        let client = FirebaseSignalingClient::from_environment()?
            .ok_or_else(|| "Firebase fallback is no longer configured".to_owned())?;
        client.remove(&ticket.lookup_id, &ticket.owner_token)
    } else {
        let client = SignalingClient::from_environment()?
            .ok_or_else(|| "Cloudflare signaling is no longer configured".to_owned())?;
        client.remove(&ticket.lookup_id, &ticket.owner_token)
    }
}

fn fill_published_ticket(
    ticket: &mut HostedShareTicket,
    room: String,
    record: &CloudRecord,
    backend: &str,
) {
    ticket.connect_code.clone_from(&room);
    ticket.room_code = room;
    ticket.owner_token.clone_from(&record.owner);
    ticket.lookup_id.clone_from(&record.lookup_id);
    ticket.signaling_backend = backend.to_owned();
    ticket.cloud_published = true;
    ticket.cloud_status = format!("published via {backend}");
}

fn serialize_token(token: &Token) -> Result<Vec<u8>, String> {
    let mut writer = Writer::new();
    writer.raw(PAYLOAD_MAGIC);
    writer.u32(PAYLOAD_VERSION);
    writer
        .string(token.ip())
        .map_err(|error| error.to_string())?;
    writer.u16(token.port());
    writer
        .string(token.folder())
        .map_err(|error| error.to_string())?;
    writer.u8(u8::from(token.allow_writes()));
    writer
        .bytes(token.secret())
        .map_err(|error| error.to_string())?;
    Ok(writer.into_inner())
}

fn deserialize_token(bytes: &[u8]) -> Result<Token, String> {
    let mut reader = Reader::new(bytes);
    if reader
        .raw(PAYLOAD_MAGIC.len())
        .map_err(|error| error.to_string())?
        != PAYLOAD_MAGIC
    {
        return Err("decrypted payload has bad magic".to_owned());
    }
    let version = reader.u32().map_err(|error| error.to_string())?;
    if version != 1 && version != PAYLOAD_VERSION {
        return Err("unsupported token version".to_owned());
    }
    let ip = reader.string().map_err(|error| error.to_string())?;
    let port = reader.u16().map_err(|error| error.to_string())?;
    let folder = reader.string().map_err(|error| error.to_string())?;
    let allow_writes = if version >= 2 {
        reader.u8().map_err(|error| error.to_string())? != 0
    } else {
        true
    };
    let secret = reader.bytes().map_err(|error| error.to_string())?;
    Token::new(ip, port, secret, folder, allow_writes)
}

fn seal_token(token: &Token) -> Result<(Key256, Vec<u8>), String> {
    let key: Key256 = random_array()?;
    let nonce: [u8; 12] = random_array()?;
    let plaintext = serialize_token(token)?;
    let sealed = aead_seal(&key, &nonce, &plaintext)?;
    let mut bundle = Vec::with_capacity(PAYLOAD_MAGIC.len() + nonce.len() + sealed.len());
    bundle.extend_from_slice(PAYLOAD_MAGIC);
    bundle.extend_from_slice(&nonce);
    bundle.extend_from_slice(&sealed);
    Ok((key, bundle))
}

fn open_bundle(key: &Key256, bundle: &[u8]) -> Result<Token, String> {
    if bundle.len() < PAYLOAD_MAGIC.len() + 12 + 16
        || bundle.get(..PAYLOAD_MAGIC.len()) != Some(PAYLOAD_MAGIC)
    {
        return Err("malformed payload bundle".to_owned());
    }
    let nonce_start = PAYLOAD_MAGIC.len();
    let nonce_end = nonce_start + 12;
    let nonce: [u8; 12] = bundle[nonce_start..nonce_end]
        .try_into()
        .map_err(|_| "malformed payload bundle".to_owned())?;
    let plaintext = aead_open(key, &nonce, &bundle[nonce_end..])
        .map_err(|_| "wrong code or tampered payload".to_owned())?;
    deserialize_token(&plaintext).map_err(|_| "decrypted payload is malformed".to_owned())
}

fn argon2id_key(password: &[u8], salt: &[u8]) -> Result<Key256, String> {
    if salt.len() != ARGON_SALT_LEN {
        return Err("bad salt length".to_owned());
    }
    let params = Params::new(ARGON_MEMORY_KIB, ARGON_ITERATIONS, 1, Some(32))
        .map_err(|error| error.to_string())?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut key = [0_u8; 32];
    argon
        .hash_password_into(password, salt, &mut key)
        .map_err(|error| error.to_string())?;
    Ok(key)
}

fn http_client() -> Result<HttpClient, String> {
    HttpClient::builder()
        .timeout(HTTP_TIMEOUT)
        .user_agent("FolderBuddies/1")
        .build()
        .map_err(|error| error.to_string())
}

fn trim_trailing_slashes(value: &mut String) {
    while value.ends_with('/') {
        value.pop();
    }
}

fn require_https(value: &str, label: &str) -> Result<(), String> {
    if value.starts_with("https://") {
        Ok(())
    } else {
        Err(format!("{label} must use https://"))
    }
}

fn firebase_created_at_ms(raw: f64) -> i64 {
    if raw <= 0.0 {
        0
    } else if raw < 20_000_000_000.0 {
        (raw * 1000.0) as i64
    } else {
        raw as i64
    }
}

fn unix_seconds() -> Result<i64, String> {
    let seconds = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_secs();
    i64::try_from(seconds).map_err(|_| "clock overflow".to_owned())
}

fn unix_millis() -> Result<i64, String> {
    let millis = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|error| error.to_string())?
        .as_millis();
    i64::try_from(millis).map_err(|_| "clock overflow".to_owned())
}

#[must_use]
const fn code_split(total: usize) -> Option<(usize, usize)> {
    match total {
        SHORT_CODE_LEN => Some((SHORT_LOOKUP_LEN, SHORT_SECRET_LEN)),
        LONG_CODE_LEN => Some((LONG_LOOKUP_LEN, LONG_SECRET_LEN)),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base91_round_trip_and_whitespace() {
        let source = b"Folder Buddies base91 compatibility";
        let encoded = base91_encode(source);
        assert_eq!(base91_decode(&encoded).expect("decode"), source);
        assert_eq!(
            base91_decode(&format!("  {encoded}\n")).expect("decode"),
            source
        );
    }

    #[test]
    fn offline_blob_round_trip() {
        let token = Token::new(
            "127.0.0.1".to_owned(),
            4242,
            vec![9; SECRET_BYTES],
            "demo".to_owned(),
            true,
        )
        .expect("token");
        let blob = seal_for_offline(&token).expect("seal");
        assert_eq!(open_offline_blob(&blob).expect("open"), token);
    }

    #[test]
    fn legacy_empty_folder_and_trailing_payload_are_accepted() {
        let token = Token::new(
            "127.0.0.1".to_owned(),
            4242,
            vec![1; SECRET_BYTES],
            String::new(),
            true,
        )
        .expect("token");
        let mut payload = serialize_token(&token).expect("serialize");
        payload.extend_from_slice(b"ignored");
        assert_eq!(deserialize_token(&payload).expect("deserialize"), token);
    }

    #[test]
    fn browser_code_native_error_matches_cpp() {
        assert_eq!(
            resolve_share_code("FBS2:anything").expect_err("must reject"),
            "that is a web-browser WebRTC code. Native clients need a native room code (6 or 16 chars) or native offline Base91 blob."
        );
    }
}
