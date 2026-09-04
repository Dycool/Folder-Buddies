use std::{
    fmt,
    io::{self, IoSliceMut},
    net::SocketAddr,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll},
    time::Duration,
};

use quinn::{
    AsyncUdpSocket, ClientConfig, Endpoint, EndpointConfig, RecvStream, SendStream, ServerConfig,
    TokioRuntime, TransportConfig, UdpPoller, VarInt,
    crypto::rustls::{QuicClientConfig, QuicServerConfig},
    udp::{RecvMeta, Transmit},
};
use rcgen::generate_simple_self_signed;
use rustls::{
    DigitallySignedStruct, SignatureScheme,
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    pki_types::{CertificateDer, PrivatePkcs8KeyDer, ServerName, UnixTime},
};
use tokio::sync::{mpsc, oneshot};
use webrtc_ice::{
    agent::{Agent, agent_config::AgentConfig},
    candidate::{Candidate, CandidateType, candidate_base::unmarshal_candidate},
    network_type::NetworkType,
    url::Url,
};
use webrtc_util::Conn;

const STUN_SERVER: &str = "stun:stun.l.google.com:19302";
const ICE_GATHER_TIMEOUT: Duration = Duration::from_secs(15);
const ICE_CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const QUIC_CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const QUIC_IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const QUIC_ALPN: &[u8] = b"fbq";
const STREAM_WINDOW: u64 = 16 * 1024 * 1024;
const CONNECTION_WINDOW: u64 = 128 * 1024 * 1024;
const MAX_BIDI_STREAMS: u64 = 64;
const MAX_DATAGRAM: usize = 1350;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NativeQuicRole {
    Client,
    Server,
}

impl NativeQuicRole {
    const fn is_controlling(self) -> bool {
        matches!(self, Self::Client)
    }
}

pub struct NativeQuicEndpoint {
    role: NativeQuicRole,
    agent: Arc<Agent>,
    local_description: String,
    endpoint: Option<Endpoint>,
    connection: Option<quinn::Connection>,
}

impl fmt::Debug for NativeQuicEndpoint {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("NativeQuicEndpoint")
            .field("role", &self.role)
            .field("connected", &self.connected())
            .finish_non_exhaustive()
    }
}

impl NativeQuicEndpoint {
    pub async fn start(role: NativeQuicRole) -> Result<Self, String> {
        let stun = Url::parse_url(STUN_SERVER).map_err(|error| error.to_string())?;
        let config = AgentConfig {
            urls: vec![stun],
            network_types: vec![NetworkType::Udp4, NetworkType::Udp6],
            candidate_types: vec![CandidateType::Host, CandidateType::ServerReflexive],
            is_controlling: role.is_controlling(),
            include_loopback: true,
            ..AgentConfig::default()
        };
        let agent = Arc::new(Agent::new(config).await.map_err(|error| error.to_string())?);

        let (gathered_tx, gathered_rx) = oneshot::channel::<()>();
        let gathered_tx = Arc::new(Mutex::new(Some(gathered_tx)));
        agent.on_candidate(Box::new(move |candidate| {
            let gathered_tx = Arc::clone(&gathered_tx);
            Box::pin(async move {
                if candidate.is_none()
                    && let Ok(mut sender) = gathered_tx.lock()
                    && let Some(sender) = sender.take()
                {
                    let _ = sender.send(());
                }
            })
        }));
        agent.gather_candidates().map_err(|error| error.to_string())?;
        tokio::time::timeout(ICE_GATHER_TIMEOUT, gathered_rx)
            .await
            .map_err(|_| "ICE candidate gathering timed out".to_owned())?
            .map_err(|_| "ICE candidate gathering stopped unexpectedly".to_owned())?;

        let local_description = local_description(&agent).await?;
        Ok(Self {
            role,
            agent,
            local_description,
            endpoint: None,
            connection: None,
        })
    }

    #[must_use]
    pub fn local_description(&self) -> &str {
        &self.local_description
    }

    pub async fn set_remote_description(&mut self, description: &str) -> Result<(), String> {
        if self.connection.is_some() {
            return Err("native QUIC endpoint is already connected".to_owned());
        }
        let remote = parse_description(description)?;
        for candidate in remote.candidates {
            self.agent
                .add_remote_candidate(&candidate)
                .map_err(|error| error.to_string())?;
        }

        let (_cancel_tx, cancel_rx) = mpsc::channel(1);
        let ice_conn: Arc<dyn Conn + Send + Sync> = match self.role {
            NativeQuicRole::Client => tokio::time::timeout(
                ICE_CONNECT_TIMEOUT,
                self.agent.dial(cancel_rx, remote.ufrag, remote.password),
            )
            .await
            .map_err(|_| "ICE connection timed out".to_owned())?
            .map_err(|error| error.to_string())?,
            NativeQuicRole::Server => tokio::time::timeout(
                ICE_CONNECT_TIMEOUT,
                self.agent.accept(cancel_rx, remote.ufrag, remote.password),
            )
            .await
            .map_err(|_| "ICE connection timed out".to_owned())?
            .map_err(|error| error.to_string())?,
        };

        let socket = IceQuinnSocket::start(ice_conn)?;
        let remote_addr = socket.remote_addr();
        let mut endpoint = Endpoint::new_with_abstract_socket(
            EndpointConfig::default(),
            match self.role {
                NativeQuicRole::Client => None,
                NativeQuicRole::Server => Some(server_config()?),
            },
            Arc::new(socket),
            Arc::new(TokioRuntime),
        )
        .map_err(|error| error.to_string())?;

        let connection = match self.role {
            NativeQuicRole::Client => {
                endpoint.set_default_client_config(client_config()?);
                let connecting = endpoint
                    .connect(remote_addr, "folderbuddies")
                    .map_err(|error| error.to_string())?;
                tokio::time::timeout(QUIC_CONNECT_TIMEOUT, connecting)
                    .await
                    .map_err(|_| "QUIC connection timed out".to_owned())?
                    .map_err(|error| error.to_string())?
            }
            NativeQuicRole::Server => {
                let incoming = tokio::time::timeout(QUIC_CONNECT_TIMEOUT, endpoint.accept())
                    .await
                    .map_err(|_| "QUIC connection timed out".to_owned())?
                    .ok_or_else(|| "QUIC endpoint closed before a connection arrived".to_owned())?;
                tokio::time::timeout(QUIC_CONNECT_TIMEOUT, incoming)
                    .await
                    .map_err(|_| "QUIC handshake timed out".to_owned())?
                    .map_err(|error| error.to_string())?
            }
        };

        self.connection = Some(connection);
        self.endpoint = Some(endpoint);
        Ok(())
    }

    #[must_use]
    pub fn connected(&self) -> bool {
        self.connection
            .as_ref()
            .is_some_and(|connection| connection.close_reason().is_none())
    }

    pub async fn open_stream(&self) -> Result<(SendStream, RecvStream), String> {
        let connection = self
            .connection
            .as_ref()
            .ok_or_else(|| "native QUIC endpoint is not connected".to_owned())?;
        connection.open_bi().await.map_err(|error| error.to_string())
    }

    pub async fn open_streams(
        &self,
        count: usize,
    ) -> Result<Vec<(SendStream, RecvStream)>, String> {
        let mut streams = Vec::with_capacity(count);
        for _ in 0..count {
            streams.push(self.open_stream().await?);
        }
        Ok(streams)
    }

    pub async fn accept_stream(&self) -> Result<(SendStream, RecvStream), String> {
        let connection = self
            .connection
            .as_ref()
            .ok_or_else(|| "native QUIC endpoint is not connected".to_owned())?;
        connection.accept_bi().await.map_err(|error| error.to_string())
    }

    pub async fn close(&mut self) {
        if let Some(connection) = self.connection.take() {
            connection.close(VarInt::from_u32(0), b"Folder Buddies shutdown");
        }
        if let Some(endpoint) = self.endpoint.take() {
            endpoint.close(VarInt::from_u32(0), b"Folder Buddies shutdown");
            endpoint.wait_idle().await;
        }
        let _ = self.agent.close().await;
    }
}

struct RemoteDescription {
    ufrag: String,
    password: String,
    candidates: Vec<Arc<dyn Candidate + Send + Sync>>,
}

async fn local_description(agent: &Agent) -> Result<String, String> {
    let (ufrag, password) = agent.get_local_user_credentials().await;
    if ufrag.is_empty() || password.is_empty() {
        return Err("ICE returned empty local credentials".to_owned());
    }
    let candidates = agent
        .get_local_candidates()
        .await
        .map_err(|error| error.to_string())?;
    if candidates.is_empty() {
        return Err("ICE did not gather any usable candidates".to_owned());
    }

    let mut sdp = String::from(
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Folder Buddies\r\nt=0 0\r\nm=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n",
    );
    sdp.push_str("a=ice-ufrag:");
    sdp.push_str(&ufrag);
    sdp.push_str("\r\na=ice-pwd:");
    sdp.push_str(&password);
    sdp.push_str("\r\n");
    for candidate in candidates {
        sdp.push_str("a=candidate:");
        sdp.push_str(&candidate.marshal());
        sdp.push_str("\r\n");
    }
    sdp.push_str("a=end-of-candidates\r\n");
    Ok(sdp)
}

fn parse_description(description: &str) -> Result<RemoteDescription, String> {
    let mut ufrag = None;
    let mut password = None;
    let mut candidates = Vec::new();
    for raw_line in description.lines() {
        let line = raw_line.trim().trim_end_matches('\r');
        if let Some(value) = line.strip_prefix("a=ice-ufrag:") {
            if !value.is_empty() {
                ufrag = Some(value.to_owned());
            }
        } else if let Some(value) = line.strip_prefix("a=ice-pwd:") {
            if !value.is_empty() {
                password = Some(value.to_owned());
            }
        } else if let Some(value) = line.strip_prefix("a=candidate:") {
            let candidate = unmarshal_candidate(value).map_err(|error| error.to_string())?;
            let candidate: Arc<dyn Candidate + Send + Sync> = Arc::new(candidate);
            candidates.push(candidate);
        }
    }
    if candidates.is_empty() {
        return Err("ICE candidate description contains no candidates".to_owned());
    }
    Ok(RemoteDescription {
        ufrag: ufrag.ok_or_else(|| "ICE candidate description is missing ufrag".to_owned())?,
        password: password
            .ok_or_else(|| "ICE candidate description is missing password".to_owned())?,
        candidates,
    })
}

#[derive(Debug)]
struct IceQuinnSocket {
    local_addr: SocketAddr,
    remote_addr: SocketAddr,
    incoming: Mutex<mpsc::UnboundedReceiver<Vec<u8>>>,
    outgoing: mpsc::UnboundedSender<Vec<u8>>,
}

impl IceQuinnSocket {
    fn start(conn: Arc<dyn Conn + Send + Sync>) -> Result<Self, String> {
        let local_addr = conn.local_addr().map_err(|error| error.to_string())?;
        let remote_addr = conn
            .remote_addr()
            .ok_or_else(|| "ICE selected pair has no remote address".to_owned())?;
        let (incoming_tx, incoming_rx) = mpsc::unbounded_channel();
        let (outgoing_tx, mut outgoing_rx) = mpsc::unbounded_channel::<Vec<u8>>();

        let read_conn = Arc::clone(&conn);
        tokio::spawn(async move {
            let mut packet = vec![0_u8; 65_535];
            while let Ok(size) = read_conn.recv(&mut packet).await {
                if incoming_tx.send(packet[..size].to_vec()).is_err() {
                    break;
                }
            }
        });
        tokio::spawn(async move {
            while let Some(packet) = outgoing_rx.recv().await {
                if conn.send(&packet).await.is_err() {
                    break;
                }
            }
        });

        Ok(Self {
            local_addr,
            remote_addr,
            incoming: Mutex::new(incoming_rx),
            outgoing: outgoing_tx,
        })
    }

    const fn remote_addr(&self) -> SocketAddr {
        self.remote_addr
    }
}

impl AsyncUdpSocket for IceQuinnSocket {
    fn create_io_poller(self: Arc<Self>) -> Pin<Box<dyn UdpPoller>> {
        Box::pin(AlwaysWritable)
    }

    fn try_send(&self, transmit: &Transmit<'_>) -> io::Result<()> {
        if transmit.destination != self.remote_addr {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "QUIC attempted to send outside the selected ICE pair",
            ));
        }
        let segment_size = transmit
            .segment_size
            .filter(|size| *size > 0 && *size < transmit.contents.len());
        if let Some(segment_size) = segment_size {
            for segment in transmit.contents.chunks(segment_size) {
                self.outgoing.send(segment.to_vec()).map_err(|_| {
                    io::Error::new(io::ErrorKind::BrokenPipe, "ICE send task stopped")
                })?;
            }
        } else {
            self.outgoing
                .send(transmit.contents.to_vec())
                .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "ICE send task stopped"))?;
        }
        Ok(())
    }

    fn poll_recv(
        &self,
        cx: &mut Context<'_>,
        bufs: &mut [IoSliceMut<'_>],
        meta: &mut [RecvMeta],
    ) -> Poll<io::Result<usize>> {
        if bufs.is_empty() || meta.is_empty() {
            return Poll::Ready(Ok(0));
        }
        let Ok(mut incoming) = self.incoming.lock() else {
            return Poll::Ready(Err(io::Error::other("ICE receive queue lock poisoned")));
        };
        match Pin::new(&mut *incoming).poll_recv(cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(None) => Poll::Ready(Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "ICE receive task stopped",
            ))),
            Poll::Ready(Some(packet)) => {
                if packet.len() > bufs[0].len() {
                    return Poll::Ready(Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "ICE datagram exceeds Quinn receive buffer",
                    )));
                }
                bufs[0][..packet.len()].copy_from_slice(&packet);
                meta[0] = RecvMeta::default();
                meta[0].addr = self.remote_addr;
                meta[0].len = packet.len();
                meta[0].stride = packet.len().max(1);
                Poll::Ready(Ok(1))
            }
        }
    }

    fn local_addr(&self) -> io::Result<SocketAddr> {
        Ok(self.local_addr)
    }

    fn may_fragment(&self) -> bool {
        false
    }
}

#[derive(Debug)]
struct AlwaysWritable;

impl UdpPoller for AlwaysWritable {
    fn poll_writable(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

fn transport_config() -> Result<Arc<TransportConfig>, String> {
    let mut transport = TransportConfig::default();
    transport.max_concurrent_bidi_streams(
        VarInt::from_u64(MAX_BIDI_STREAMS).map_err(|error| error.to_string())?,
    );
    transport.max_concurrent_uni_streams(VarInt::from_u32(0));
    transport.stream_receive_window(
        VarInt::from_u64(STREAM_WINDOW).map_err(|error| error.to_string())?,
    );
    transport.receive_window(
        VarInt::from_u64(CONNECTION_WINDOW).map_err(|error| error.to_string())?,
    );
    transport.max_idle_timeout(Some(
        QUIC_IDLE_TIMEOUT
            .try_into()
            .map_err(|error: quinn::VarIntBoundsExceeded| error.to_string())?,
    ));
    transport.initial_mtu(MAX_DATAGRAM as u16);
    transport.min_mtu(MAX_DATAGRAM as u16);
    transport.mtu_discovery_config(None);
    Ok(Arc::new(transport))
}

fn server_config() -> Result<ServerConfig, String> {
    let certified = generate_simple_self_signed(vec!["folderbuddies".to_owned()])
        .map_err(|error| error.to_string())?;
    let key = PrivatePkcs8KeyDer::from(certified.signing_key.serialize_der());
    let certificate = CertificateDer::from(certified.cert);
    let mut tls = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![certificate], key.into())
        .map_err(|error| error.to_string())?;
    tls.alpn_protocols = vec![QUIC_ALPN.to_vec()];
    let crypto = QuicServerConfig::try_from(tls).map_err(|error| error.to_string())?;
    let mut config = ServerConfig::with_crypto(Arc::new(crypto));
    config.transport = transport_config()?;
    config.migration(false);
    Ok(config)
}

fn client_config() -> Result<ClientConfig, String> {
    let mut tls = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(FolderBuddiesCertificateVerifier))
        .with_no_client_auth();
    tls.alpn_protocols = vec![QUIC_ALPN.to_vec()];
    let crypto = QuicClientConfig::try_from(tls).map_err(|error| error.to_string())?;
    let mut config = ClientConfig::new(Arc::new(crypto));
    config.transport_config(transport_config()?);
    Ok(config)
}

#[derive(Debug)]
struct FolderBuddiesCertificateVerifier;

impl ServerCertVerifier for FolderBuddiesCertificateVerifier {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn verify_tls13_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Ok(HandshakeSignatureValid::assertion())
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![
            SignatureScheme::ECDSA_NISTP256_SHA256,
            SignatureScheme::ED25519,
            SignatureScheme::RSA_PSS_SHA256,
            SignatureScheme::RSA_PSS_SHA384,
            SignatureScheme::RSA_PSS_SHA512,
        ]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn description_parser_accepts_libjuice_shape() {
        let description = concat!(
            "v=0\r\n",
            "a=ice-ufrag:demo\r\n",
            "a=ice-pwd:secret\r\n",
            "a=candidate:1 1 UDP 2130706431 127.0.0.1 5000 typ host\r\n",
        );
        let parsed = parse_description(description).expect("parse");
        assert_eq!(parsed.ufrag, "demo");
        assert_eq!(parsed.password, "secret");
        assert_eq!(parsed.candidates.len(), 1);
    }

    #[test]
    fn description_parser_fails_closed() {
        assert!(parse_description("v=0\r\n").is_err());
        assert!(parse_description("a=ice-ufrag:x\r\na=ice-pwd:y\r\n").is_err());
    }
}
