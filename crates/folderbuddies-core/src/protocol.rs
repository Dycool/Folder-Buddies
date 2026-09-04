use std::io::{self, Read, Write};

pub const MAGIC: u32 = 0x4642_4459;
pub const PROTOCOL_VERSION: u32 = 2;
pub const MAX_IO: u32 = 1 << 20;
pub const DEFAULT_CONNECTIONS: usize = 8;
pub const MAX_HANDSHAKE_MESSAGE: u32 = 64 * 1024;
pub const MAX_SECURE_RECORD: u32 = 256 << 20;
pub const HEADER_LEN: usize = 20;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum Op {
    Hello = 1,
    Challenge = 2,
    Auth = 3,
    AuthOk = 4,
    AuthFail = 5,
    GetAttr = 10,
    ReadDir = 11,
    Open = 12,
    Read = 13,
    Write = 14,
    Create = 15,
    Mkdir = 16,
    Unlink = 17,
    Rmdir = 18,
    Rename = 19,
    Truncate = 20,
    Release = 21,
    Fsync = 22,
    StatFs = 23,
    Utimens = 24,
    Chmod = 25,
    Flush = 26,
    Access = 27,
    Invalidate = 28,
}

impl Op {
    #[must_use]
    pub const fn code(self) -> u16 {
        self as u16
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Header {
    op: u16,
    status: i16,
    request_id: u64,
    payload_len: u32,
}

impl Header {
    #[must_use]
    pub const fn new(op: u16, status: i16, request_id: u64, payload_len: u32) -> Self {
        Self {
            op,
            status,
            request_id,
            payload_len,
        }
    }

    #[must_use]
    pub const fn op(&self) -> u16 {
        self.op
    }

    #[must_use]
    pub const fn status(&self) -> i16 {
        self.status
    }

    #[must_use]
    pub const fn request_id(&self) -> u64 {
        self.request_id
    }

    #[must_use]
    pub const fn payload_len(&self) -> u32 {
        self.payload_len
    }

    #[must_use]
    pub fn encode(&self) -> [u8; HEADER_LEN] {
        let mut out = [0_u8; HEADER_LEN];
        out[0..4].copy_from_slice(&MAGIC.to_le_bytes());
        out[4..6].copy_from_slice(&self.op.to_le_bytes());
        out[6..8].copy_from_slice(&self.status.to_le_bytes());
        out[8..16].copy_from_slice(&self.request_id.to_le_bytes());
        out[16..20].copy_from_slice(&self.payload_len.to_le_bytes());
        out
    }

    pub fn decode(bytes: &[u8]) -> io::Result<Self> {
        if bytes.len() != HEADER_LEN {
            return Err(invalid_data("message header has the wrong size"));
        }
        let magic = u32::from_le_bytes(bytes[0..4].try_into().map_err(|_| invalid_data("magic"))?);
        if magic != MAGIC {
            return Err(invalid_data("message magic mismatch"));
        }
        Ok(Self {
            op: u16::from_le_bytes(bytes[4..6].try_into().map_err(|_| invalid_data("op"))?),
            status: i16::from_le_bytes(bytes[6..8].try_into().map_err(|_| invalid_data("status"))?),
            request_id: u64::from_le_bytes(
                bytes[8..16]
                    .try_into()
                    .map_err(|_| invalid_data("request id"))?,
            ),
            payload_len: u32::from_le_bytes(
                bytes[16..20]
                    .try_into()
                    .map_err(|_| invalid_data("payload length"))?,
            ),
        })
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct WireAttr {
    pub(crate) ino: u64,
    pub(crate) size: u64,
    pub(crate) blocks: u64,
    pub(crate) atime: i64,
    pub(crate) mtime: i64,
    pub(crate) ctime: i64,
    pub(crate) mode: u32,
    pub(crate) nlink: u32,
    pub(crate) uid: u32,
    pub(crate) gid: u32,
}

impl WireAttr {
    pub const LEN: usize = 64;

    #[must_use]
    pub const fn ino(&self) -> u64 {
        self.ino
    }

    #[must_use]
    pub const fn size(&self) -> u64 {
        self.size
    }

    #[must_use]
    pub const fn blocks(&self) -> u64 {
        self.blocks
    }

    #[must_use]
    pub const fn atime(&self) -> i64 {
        self.atime
    }

    #[must_use]
    pub const fn mtime(&self) -> i64 {
        self.mtime
    }

    #[must_use]
    pub const fn ctime(&self) -> i64 {
        self.ctime
    }

    #[must_use]
    pub const fn mode(&self) -> u32 {
        self.mode
    }

    #[must_use]
    pub const fn nlink(&self) -> u32 {
        self.nlink
    }

    #[must_use]
    pub const fn uid(&self) -> u32 {
        self.uid
    }

    #[must_use]
    pub const fn gid(&self) -> u32 {
        self.gid
    }

    pub fn write_to(&self, writer: &mut Writer) {
        writer.u64(self.ino);
        writer.u64(self.size);
        writer.u64(self.blocks);
        writer.i64(self.atime);
        writer.i64(self.mtime);
        writer.i64(self.ctime);
        writer.u32(self.mode);
        writer.u32(self.nlink);
        writer.u32(self.uid);
        writer.u32(self.gid);
    }

    pub fn read_from(reader: &mut Reader<'_>) -> io::Result<Self> {
        Ok(Self {
            ino: reader.u64()?,
            size: reader.u64()?,
            blocks: reader.u64()?,
            atime: reader.i64()?,
            mtime: reader.i64()?,
            ctime: reader.i64()?,
            mode: reader.u32()?,
            nlink: reader.u32()?,
            uid: reader.u32()?,
            gid: reader.u32()?,
        })
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct WireStatFs {
    pub(crate) bsize: u64,
    pub(crate) frsize: u64,
    pub(crate) blocks: u64,
    pub(crate) bfree: u64,
    pub(crate) bavail: u64,
    pub(crate) files: u64,
    pub(crate) ffree: u64,
    pub(crate) namemax: u64,
}

impl WireStatFs {
    pub const LEN: usize = 64;

    #[must_use]
    pub const fn block_size(&self) -> u64 {
        self.bsize
    }

    #[must_use]
    pub const fn fragment_size(&self) -> u64 {
        self.frsize
    }

    #[must_use]
    pub const fn blocks(&self) -> u64 {
        self.blocks
    }

    #[must_use]
    pub const fn blocks_free(&self) -> u64 {
        self.bfree
    }

    #[must_use]
    pub const fn blocks_available(&self) -> u64 {
        self.bavail
    }

    #[must_use]
    pub const fn files(&self) -> u64 {
        self.files
    }

    #[must_use]
    pub const fn files_free(&self) -> u64 {
        self.ffree
    }

    #[must_use]
    pub const fn name_max(&self) -> u64 {
        self.namemax
    }

    pub fn write_to(&self, writer: &mut Writer) {
        writer.u64(self.bsize);
        writer.u64(self.frsize);
        writer.u64(self.blocks);
        writer.u64(self.bfree);
        writer.u64(self.bavail);
        writer.u64(self.files);
        writer.u64(self.ffree);
        writer.u64(self.namemax);
    }

    pub fn read_from(reader: &mut Reader<'_>) -> io::Result<Self> {
        Ok(Self {
            bsize: reader.u64()?,
            frsize: reader.u64()?,
            blocks: reader.u64()?,
            bfree: reader.u64()?,
            bavail: reader.u64()?,
            files: reader.u64()?,
            ffree: reader.u64()?,
            namemax: reader.u64()?,
        })
    }
}

#[derive(Debug, Default)]
pub struct Writer {
    bytes: Vec<u8>,
}

impl Writer {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    pub fn raw(&mut self, bytes: &[u8]) {
        self.bytes.extend_from_slice(bytes);
    }

    pub fn u8(&mut self, value: u8) {
        self.bytes.push(value);
    }

    pub fn u16(&mut self, value: u16) {
        self.raw(&value.to_le_bytes());
    }

    pub fn i32(&mut self, value: i32) {
        self.raw(&value.to_le_bytes());
    }

    pub fn u32(&mut self, value: u32) {
        self.raw(&value.to_le_bytes());
    }

    pub fn i64(&mut self, value: i64) {
        self.raw(&value.to_le_bytes());
    }

    pub fn u64(&mut self, value: u64) {
        self.raw(&value.to_le_bytes());
    }

    pub fn string(&mut self, value: &str) -> io::Result<()> {
        let len = u32::try_from(value.len()).map_err(|_| invalid_data("string too large"))?;
        self.u32(len);
        self.raw(value.as_bytes());
        Ok(())
    }

    pub fn bytes(&mut self, value: &[u8]) -> io::Result<()> {
        let len = u32::try_from(value.len()).map_err(|_| invalid_data("byte string too large"))?;
        self.u32(len);
        self.raw(value);
        Ok(())
    }

    #[must_use]
    pub fn into_inner(self) -> Vec<u8> {
        self.bytes
    }
}

#[derive(Debug)]
pub struct Reader<'a> {
    bytes: &'a [u8],
    cursor: usize,
}

impl<'a> Reader<'a> {
    #[must_use]
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, cursor: 0 }
    }

    pub fn raw(&mut self, len: usize) -> io::Result<&'a [u8]> {
        let end = self
            .cursor
            .checked_add(len)
            .ok_or_else(|| invalid_data("payload length overflow"))?;
        let out = self
            .bytes
            .get(self.cursor..end)
            .ok_or_else(|| invalid_data("truncated payload"))?;
        self.cursor = end;
        Ok(out)
    }

    pub fn u8(&mut self) -> io::Result<u8> {
        Ok(self.raw(1)?[0])
    }

    pub fn u16(&mut self) -> io::Result<u16> {
        Ok(u16::from_le_bytes(
            self.raw(2)?.try_into().map_err(|_| invalid_data("u16"))?,
        ))
    }

    pub fn i32(&mut self) -> io::Result<i32> {
        Ok(i32::from_le_bytes(
            self.raw(4)?.try_into().map_err(|_| invalid_data("i32"))?,
        ))
    }

    pub fn u32(&mut self) -> io::Result<u32> {
        Ok(u32::from_le_bytes(
            self.raw(4)?.try_into().map_err(|_| invalid_data("u32"))?,
        ))
    }

    pub fn i64(&mut self) -> io::Result<i64> {
        Ok(i64::from_le_bytes(
            self.raw(8)?.try_into().map_err(|_| invalid_data("i64"))?,
        ))
    }

    pub fn u64(&mut self) -> io::Result<u64> {
        Ok(u64::from_le_bytes(
            self.raw(8)?.try_into().map_err(|_| invalid_data("u64"))?,
        ))
    }

    pub fn string(&mut self) -> io::Result<String> {
        let len = usize::try_from(self.u32()?).map_err(|_| invalid_data("string length"))?;
        let bytes = self.raw(len)?;
        String::from_utf8(bytes.to_vec()).map_err(|_| invalid_data("invalid UTF-8 string"))
    }

    pub fn bytes(&mut self) -> io::Result<Vec<u8>> {
        let len = usize::try_from(self.u32()?).map_err(|_| invalid_data("bytes length"))?;
        Ok(self.raw(len)?.to_vec())
    }

    #[must_use]
    pub fn remaining(&self) -> &'a [u8] {
        &self.bytes[self.cursor..]
    }

    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.cursor == self.bytes.len()
    }
}

pub fn write_plain_message<W: Write>(
    writer: &mut W,
    op: u16,
    status: i16,
    request_id: u64,
    payload: &[u8],
) -> io::Result<()> {
    let payload_len =
        u32::try_from(payload.len()).map_err(|_| invalid_data("payload too large"))?;
    if payload_len > MAX_HANDSHAKE_MESSAGE {
        return Err(invalid_data("handshake payload too large"));
    }
    let header = Header::new(op, status, request_id, payload_len).encode();
    writer.write_all(&header)?;
    writer.write_all(payload)
}

pub fn read_plain_message<R: Read>(reader: &mut R) -> io::Result<(Header, Vec<u8>)> {
    let mut header_bytes = [0_u8; HEADER_LEN];
    reader.read_exact(&mut header_bytes)?;
    let header = Header::decode(&header_bytes)?;
    if header.payload_len() > MAX_HANDSHAKE_MESSAGE {
        return Err(invalid_data("handshake payload too large"));
    }
    let mut payload = vec![0_u8; header.payload_len() as usize];
    reader.read_exact(&mut payload)?;
    Ok((header, payload))
}

fn invalid_data(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn header_wire_layout_is_stable() {
        let header = Header::new(Op::Read.code(), -7, 0x0102_0304_0506_0708, 9);
        let encoded = header.encode();
        assert_eq!(encoded.len(), HEADER_LEN);
        assert_eq!(&encoded[0..4], &MAGIC.to_le_bytes());
        assert_eq!(Header::decode(&encoded).expect("decode"), header);
    }

    #[test]
    fn wire_structs_remain_64_bytes() {
        let mut attr = Writer::new();
        WireAttr::default().write_to(&mut attr);
        assert_eq!(attr.into_inner().len(), WireAttr::LEN);
        let mut stat = Writer::new();
        WireStatFs::default().write_to(&mut stat);
        assert_eq!(stat.into_inner().len(), WireStatFs::LEN);
    }
}
