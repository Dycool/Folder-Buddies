use std::io::{self, Read, Write};

use chacha20poly1305::{
    ChaCha20Poly1305, Nonce,
    aead::{Aead, KeyInit},
};
use hmac::{Hmac, KeyInit as HmacKeyInit, Mac};
use sha2::{Digest, Sha256};

use crate::protocol::{HEADER_LEN, Header, MAX_SECURE_RECORD};

pub type Key256 = [u8; 32];
type HmacSha256 = Hmac<Sha256>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SessionKeys {
    tx: Key256,
    rx: Key256,
}

impl SessionKeys {
    #[must_use]
    pub const fn tx(&self) -> &Key256 {
        &self.tx
    }

    #[must_use]
    pub const fn rx(&self) -> &Key256 {
        &self.rx
    }
}

#[must_use]
pub fn sha256(bytes: &[u8]) -> Key256 {
    Sha256::digest(bytes).into()
}

pub fn hmac_sha256(key: &[u8], data: &[u8]) -> Result<Key256, String> {
    let mut mac = HmacSha256::new_from_slice(key).map_err(|_| "invalid HMAC key".to_owned())?;
    mac.update(data);
    Ok(mac.finalize().into_bytes().into())
}

pub fn auth_proof(key: &[u8], nonce_client: &[u8], nonce_server: &[u8]) -> Result<Key256, String> {
    let mut data = Vec::with_capacity(nonce_client.len() + nonce_server.len());
    data.extend_from_slice(nonce_client);
    data.extend_from_slice(nonce_server);
    hmac_sha256(key, &data)
}

pub fn derive_session_keys(
    auth_key: &[u8],
    nonce_client: &[u8],
    nonce_server: &[u8],
    is_server: bool,
) -> Result<SessionKeys, String> {
    let mut seed = Vec::with_capacity(nonce_client.len() + nonce_server.len() + 13);
    seed.extend_from_slice(nonce_client);
    seed.extend_from_slice(nonce_server);
    seed.extend_from_slice(b"FB-session-v2");
    let master = hmac_sha256(auth_key, &seed)?;
    let client_to_server = hmac_sha256(&master, b"client->server")?;
    let server_to_client = hmac_sha256(&master, b"server->client")?;
    let (tx, rx) = if is_server {
        (server_to_client, client_to_server)
    } else {
        (client_to_server, server_to_client)
    };
    Ok(SessionKeys { tx, rx })
}

pub fn random_bytes(len: usize) -> Result<Vec<u8>, String> {
    let mut bytes = vec![0_u8; len];
    getrandom::fill(&mut bytes).map_err(|error| format!("OS random source failed: {error}"))?;
    Ok(bytes)
}

pub fn random_array<const N: usize>() -> Result<[u8; N], String> {
    let mut bytes = [0_u8; N];
    getrandom::fill(&mut bytes).map_err(|error| format!("OS random source failed: {error}"))?;
    Ok(bytes)
}

pub fn aead_seal(key: &Key256, nonce: &[u8; 12], plaintext: &[u8]) -> Result<Vec<u8>, String> {
    let cipher = ChaCha20Poly1305::new_from_slice(key).map_err(|_| "invalid ChaCha20 key".to_owned())?;
    let nonce = Nonce::from(*nonce);
    cipher
        .encrypt(&nonce, plaintext)
        .map_err(|_| "ChaCha20-Poly1305 encryption failed".to_owned())
}

pub fn aead_open(key: &Key256, nonce: &[u8; 12], record: &[u8]) -> Result<Vec<u8>, String> {
    let cipher = ChaCha20Poly1305::new_from_slice(key).map_err(|_| "invalid ChaCha20 key".to_owned())?;
    let nonce = Nonce::from(*nonce);
    cipher
        .decrypt(&nonce, record)
        .map_err(|_| "authentication failed".to_owned())
}

#[derive(Debug)]
pub struct SecureSender {
    key: Key256,
    counter: u64,
}

impl SecureSender {
    #[must_use]
    pub const fn new(key: Key256) -> Self {
        Self { key, counter: 0 }
    }

    pub fn send<W: Write>(
        &mut self,
        writer: &mut W,
        op: u16,
        status: i16,
        request_id: u64,
        payload: &[u8],
    ) -> io::Result<()> {
        let payload_len = u32::try_from(payload.len())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "payload too large"))?;
        let mut plain = Vec::with_capacity(HEADER_LEN + payload.len());
        plain.extend_from_slice(&Header::new(op, status, request_id, payload_len).encode());
        plain.extend_from_slice(payload);
        let nonce = nonce_from_counter(self.counter);
        self.counter = self
            .counter
            .checked_add(1)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "nonce counter exhausted"))?;
        let record = aead_seal(&self.key, &nonce, &plain)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
        let len = u32::try_from(record.len())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "secure record too large"))?;
        if len > MAX_SECURE_RECORD {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "secure record too large"));
        }
        writer.write_all(&len.to_le_bytes())?;
        writer.write_all(&record)
    }
}

#[derive(Debug)]
pub struct SecureReceiver {
    key: Key256,
    counter: u64,
}

impl SecureReceiver {
    #[must_use]
    pub const fn new(key: Key256) -> Self {
        Self { key, counter: 0 }
    }

    pub fn recv<R: Read>(&mut self, reader: &mut R) -> io::Result<(Header, Vec<u8>)> {
        let mut len_bytes = [0_u8; 4];
        reader.read_exact(&mut len_bytes)?;
        let record_len = u32::from_le_bytes(len_bytes);
        if record_len < (HEADER_LEN + 16) as u32 || record_len > MAX_SECURE_RECORD {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid secure record length"));
        }
        let mut record = vec![0_u8; record_len as usize];
        reader.read_exact(&mut record)?;
        let nonce = nonce_from_counter(self.counter);
        self.counter = self
            .counter
            .checked_add(1)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "nonce counter exhausted"))?;
        let plain = aead_open(&self.key, &nonce, &record)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))?;
        if plain.len() < HEADER_LEN {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "truncated secure header"));
        }
        let header = Header::decode(&plain[..HEADER_LEN])?;
        let payload = plain[HEADER_LEN..].to_vec();
        if header.payload_len() as usize != payload.len() {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "secure payload length mismatch"));
        }
        Ok((header, payload))
    }
}

#[must_use]
fn nonce_from_counter(counter: u64) -> [u8; 12] {
    let mut nonce = [0_u8; 12];
    nonce[..8].copy_from_slice(&counter.to_le_bytes());
    nonce
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Op;

    #[test]
    fn rfc8439_vector_matches() {
        let key: Key256 = [
            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
            0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
            0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
        ];
        let nonce = [0x07, 0, 0, 0, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47];
        let message = b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        let record = aead_seal(&key, &nonce, message).expect("seal");
        assert_eq!(&record[..8], &[0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb]);
        assert_eq!(aead_open(&key, &nonce, &record).expect("open"), message);
    }

    #[test]
    fn secure_framing_round_trip() {
        let key = [7_u8; 32];
        let mut sender = SecureSender::new(key);
        let mut receiver = SecureReceiver::new(key);
        let mut bytes = Vec::new();
        sender
            .send(&mut bytes, Op::Read.code(), 0, 42, &[1, 2, 3, 4, 5])
            .expect("send");
        let (header, payload) = receiver.recv(&mut bytes.as_slice()).expect("recv");
        assert_eq!(header.op(), Op::Read.code());
        assert_eq!(header.request_id(), 42);
        assert_eq!(payload, [1, 2, 3, 4, 5]);
    }
}
