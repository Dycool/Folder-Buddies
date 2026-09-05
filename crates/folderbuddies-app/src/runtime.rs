use std::{
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, UdpSocket},
    path::{Path, PathBuf},
    sync::Arc,
};

use folderbuddies_core::{
    client::Client,
    compat_host::CompatRoomHost,
    native_transport::NativeQuicClient,
    remote_fs::RemoteFs,
    server::Server,
    signaling::{
        HostedShareTicket, Token, looks_like_room_code, publish_share, remove_published_room,
        resolve_share_code,
    },
    web_client::WebRtcRemoteClient,
    web_compat::looks_like_web_compat_code,
};
use igd_next::{Gateway, PortMappingProtocol, search_gateway};

use crate::mount::Mount;

struct UpnpMapping {
    gateway: Gateway,
    external_port: u16,
}

impl UpnpMapping {
    fn create(local_ip: Ipv4Addr, internal_port: u16) -> Result<(Self, IpAddr, u16), String> {
        let gateway = search_gateway(Default::default())
            .map_err(|error| format!("gateway discovery failed: {error}"))?;
        let local_address = SocketAddr::new(IpAddr::V4(local_ip), internal_port);
        let external_port = gateway
            .add_any_port(PortMappingProtocol::TCP, local_address, 0, "Folder Buddies")
            .map_err(|error| format!("UPnP port mapping failed: {error}"))?;
        let external_ip = match gateway.get_external_ip() {
            Ok(ip) => ip,
            Err(error) => {
                let _ = gateway.remove_port(PortMappingProtocol::TCP, external_port);
                return Err(format!("UPnP external address lookup failed: {error}"));
            }
        };
        Ok((
            Self {
                gateway,
                external_port,
            },
            external_ip,
            external_port,
        ))
    }
}

impl Drop for UpnpMapping {
    fn drop(&mut self) {
        let _ = self
            .gateway
            .remove_port(PortMappingProtocol::TCP, self.external_port);
    }
}

pub(crate) struct HostingSession {
    server: Option<Server>,
    ticket: Option<HostedShareTicket>,
    compat_host: Option<CompatRoomHost>,
    browser_warning: Option<String>,
    upnp_mapping: Option<UpnpMapping>,
}

impl HostingSession {
    pub(crate) fn start(
        folder: impl AsRef<Path>,
        lan_only: bool,
        allow_writes: bool,
    ) -> Result<Self, String> {
        let folder = folder.as_ref();
        let server = Server::start(folder, 0, allow_writes)?;
        let (advertised_ip, advertised_port, reach, upnp_mapping) =
            advertised_endpoint(server.bound_port(), lan_only);
        let token = Token::new(
            advertised_ip.to_string(),
            advertised_port,
            server.secret().to_vec(),
            server.share_name().to_owned(),
            allow_writes,
        )?;
        let ticket = publish_share(&token, reach)?;
        let (compat_host, browser_warning) = if ticket.cloud_published() {
            match CompatRoomHost::start(
                folder,
                ticket.room_code(),
                allow_writes,
                server.bound_port(),
            ) {
                Ok(host) => (Some(host), None),
                Err(error) => (None, Some(error)),
            }
        } else {
            (None, None)
        };
        Ok(Self {
            server: Some(server),
            ticket: Some(ticket),
            compat_host,
            browser_warning,
            upnp_mapping,
        })
    }

    #[must_use]
    pub(crate) fn running(&self) -> bool {
        self.server.as_ref().is_some_and(Server::running)
    }

    #[must_use]
    pub(crate) fn share_name(&self) -> &str {
        self.server.as_ref().map_or("", Server::share_name)
    }

    #[must_use]
    pub(crate) fn connect_code(&self) -> &str {
        self.ticket
            .as_ref()
            .map_or("", HostedShareTicket::connect_code)
    }

    #[must_use]
    pub(crate) fn reach(&self) -> &str {
        self.ticket.as_ref().map_or("", HostedShareTicket::reach)
    }

    #[must_use]
    pub(crate) fn cloud_status(&self) -> &str {
        self.ticket
            .as_ref()
            .map_or("", HostedShareTicket::cloud_status)
    }

    pub(crate) fn take_browser_warning(&mut self) -> Option<String> {
        self.browser_warning.take()
    }

    #[must_use]
    pub(crate) fn client_count(&self) -> usize {
        let native = self.server.as_ref().map_or(0, Server::client_count);
        let browser = self
            .compat_host
            .as_ref()
            .map_or(0, CompatRoomHost::browser_client_count);
        native.saturating_add(browser)
    }

    #[must_use]
    pub(crate) fn allow_writes(&self) -> bool {
        self.server.as_ref().is_some_and(Server::allow_writes)
    }

    #[must_use]
    pub(crate) fn bytes_sent(&self) -> u64 {
        let native = self.server.as_ref().map_or(0, Server::bytes_sent);
        let browser = self
            .compat_host
            .as_ref()
            .map_or(0, CompatRoomHost::bytes_out);
        native.saturating_add(browser)
    }

    #[must_use]
    pub(crate) fn bytes_received(&self) -> u64 {
        let native = self.server.as_ref().map_or(0, Server::bytes_received);
        let browser = self
            .compat_host
            .as_ref()
            .map_or(0, CompatRoomHost::bytes_in);
        native.saturating_add(browser)
    }

    pub(crate) fn stop(&mut self) {
        if let Some(mut compat_host) = self.compat_host.take() {
            compat_host.stop();
        }
        if let Some(ticket) = self.ticket.take()
            && ticket.cloud_published()
        {
            let _ = remove_published_room(&ticket);
        }
        if let Some(mut server) = self.server.take() {
            server.stop();
        }
        self.upnp_mapping.take();
    }
}

impl Drop for HostingSession {
    fn drop(&mut self) {
        self.stop();
    }
}

enum ConnectedRemote {
    Native(Arc<Client>),
    Web(Arc<WebRtcRemoteClient>),
}

impl ConnectedRemote {
    fn connected(&self) -> bool {
        match self {
            Self::Native(client) => client.connected(),
            Self::Web(client) => client.connected(),
        }
    }

    fn disconnect(&self) {
        match self {
            Self::Native(client) => client.disconnect(),
            Self::Web(client) => client.disconnect(),
        }
    }

    fn bytes_read(&self) -> u64 {
        match self {
            Self::Native(client) => client.bytes_read(),
            Self::Web(client) => client.bytes_read(),
        }
    }

    fn bytes_written(&self) -> u64 {
        match self {
            Self::Native(client) => client.bytes_written(),
            Self::Web(client) => client.bytes_written(),
        }
    }
}

pub(crate) struct ConnectedSession {
    remote: ConnectedRemote,
    quic_client: Option<NativeQuicClient>,
    mount: Option<Mount>,
    mount_path: PathBuf,
    folder: String,
    allow_writes: bool,
}

impl ConnectedSession {
    pub(crate) fn start(code: &str, _connections: usize) -> Result<Self, String> {
        let mut decode_error = String::new();
        let mut connection_error = String::new();

        match resolve_share_code(code) {
            Ok(token) => {
                let mut quic_error = String::new();
                if looks_like_room_code(code) {
                    match NativeQuicClient::connect(code, &token) {
                        Ok(mut quic_client) => {
                            let client = quic_client.client();
                            match Mount::start(
                                Arc::clone(&client),
                                token.folder(),
                                token.allow_writes(),
                            ) {
                                Ok(mount) => {
                                    let mount_path = mount.mount_path().to_owned();
                                    return Ok(Self {
                                        remote: ConnectedRemote::Native(client),
                                        quic_client: Some(quic_client),
                                        mount: Some(mount),
                                        mount_path,
                                        folder: token.folder().to_owned(),
                                        allow_writes: token.allow_writes(),
                                    });
                                }
                                Err(_) => quic_client.disconnect(),
                            }
                        }
                        Err(error) => quic_error = error,
                    }
                }

                match Client::connect_default(&token) {
                    Ok(client) => {
                        let client = Arc::new(client);
                        match Mount::start(
                            Arc::clone(&client),
                            token.folder(),
                            token.allow_writes(),
                        ) {
                            Ok(mount) => {
                                let mount_path = mount.mount_path().to_owned();
                                return Ok(Self {
                                    remote: ConnectedRemote::Native(client),
                                    quic_client: None,
                                    mount: Some(mount),
                                    mount_path,
                                    folder: token.folder().to_owned(),
                                    allow_writes: token.allow_writes(),
                                });
                            }
                            Err(tcp_error) => {
                                client.disconnect();
                                connection_error = direct_failure(&quic_error, &tcp_error);
                            }
                        }
                    }
                    Err(tcp_error) => {
                        connection_error = direct_failure(&quic_error, &tcp_error);
                    }
                }
            }
            Err(error) => decode_error = error,
        }

        if !decode_error.is_empty() && looks_like_web_compat_code(code) {
            match WebRtcRemoteClient::connect(code) {
                Ok(client) => {
                    let allow_writes = client.can_write();
                    let remote_for_mount: Arc<dyn RemoteFs> = client.clone();
                    match Mount::start_remote(remote_for_mount, "Web share", allow_writes) {
                        Ok(mount) => {
                            let mount_path = mount.mount_path().to_owned();
                            return Ok(Self {
                                remote: ConnectedRemote::Web(client),
                                quic_client: None,
                                mount: Some(mount),
                                mount_path,
                                folder: "Web share".to_owned(),
                                allow_writes,
                            });
                        }
                        Err(error) => {
                            client.disconnect();
                            connection_error = error;
                        }
                    }
                }
                Err(error) => connection_error = error,
            }
        }

        Err(if connection_error.is_empty() {
            decode_error
        } else {
            connection_error
        })
    }

    #[must_use]
    pub(crate) fn connected(&self) -> bool {
        self.remote.connected() && !self.ejected()
    }

    #[must_use]
    pub(crate) fn ejected(&self) -> bool {
        self.mount.as_ref().is_some_and(Mount::ejected)
    }

    #[must_use]
    pub(crate) fn mount_path(&self) -> &Path {
        &self.mount_path
    }

    #[must_use]
    pub(crate) fn folder(&self) -> &str {
        &self.folder
    }

    #[must_use]
    pub(crate) const fn allow_writes(&self) -> bool {
        self.allow_writes
    }

    #[must_use]
    pub(crate) fn bytes_read(&self) -> u64 {
        self.remote.bytes_read()
    }

    #[must_use]
    pub(crate) fn bytes_written(&self) -> u64 {
        self.remote.bytes_written()
    }

    pub(crate) fn disconnect(&mut self) {
        if let Some(mount) = self.mount.take() {
            let _ = mount.unmount();
        }
        if let Some(mut quic_client) = self.quic_client.take() {
            quic_client.disconnect();
        } else {
            self.remote.disconnect();
        }
    }
}

impl Drop for ConnectedSession {
    fn drop(&mut self) {
        self.disconnect();
    }
}

fn direct_failure(quic_error: &str, tcp_error: &str) -> String {
    format!(
        "Direct QUIC failed: {}; direct TCP failed: {tcp_error}",
        if quic_error.is_empty() {
            "unavailable"
        } else {
            quic_error
        }
    )
}

fn advertised_endpoint(
    internal_port: u16,
    lan_only: bool,
) -> (IpAddr, u16, String, Option<UpnpMapping>) {
    let lan_ip = best_lan_ip().unwrap_or(IpAddr::V4(Ipv4Addr::LOCALHOST));
    if lan_only {
        return (lan_ip, internal_port, format!("LAN only — {lan_ip}"), None);
    }

    if let Some(ip) = best_global_ipv6() {
        return (
            IpAddr::V6(ip),
            internal_port,
            format!("Internet (IPv6) — {ip}"),
            None,
        );
    }

    if let Some(local_v4) = best_local_ipv4() {
        match UpnpMapping::create(local_v4, internal_port) {
            Ok((mapping, external_ip, external_port)) => {
                return (
                    external_ip,
                    external_port,
                    format!("Internet (IPv4/UPnP) — {external_ip} :{external_port}"),
                    Some(mapping),
                );
            }
            Err(error) => {
                return (
                    lan_ip,
                    internal_port,
                    format!("UPnP failed ({error}) — only reachable on LAN: {lan_ip}"),
                    None,
                );
            }
        }
    }

    (
        lan_ip,
        internal_port,
        format!("No public route found — only reachable on LAN: {lan_ip}"),
        None,
    )
}

fn best_lan_ip() -> Option<IpAddr> {
    best_local_ipv4()
        .map(IpAddr::V4)
        .or_else(|| route_local_ip("[2001:4860:4860::8888]:80").filter(is_usable_ip))
}

fn best_local_ipv4() -> Option<Ipv4Addr> {
    match route_local_ip("1.1.1.1:80")? {
        IpAddr::V4(ip) if !ip.is_loopback() && !ip.is_unspecified() => Some(ip),
        _ => None,
    }
}

fn best_global_ipv6() -> Option<Ipv6Addr> {
    match route_local_ip("[2001:4860:4860::8888]:80")? {
        IpAddr::V6(ip) if is_global_ipv6(&ip) => Some(ip),
        _ => None,
    }
}

fn route_local_ip(destination: &str) -> Option<IpAddr> {
    let destination: SocketAddr = destination.parse().ok()?;
    let bind_address = if destination.is_ipv6() {
        "[::]:0"
    } else {
        "0.0.0.0:0"
    };
    let socket = UdpSocket::bind(bind_address).ok()?;
    socket.connect(destination).ok()?;
    socket.local_addr().ok().map(|address| address.ip())
}

fn is_usable_ip(ip: &IpAddr) -> bool {
    !ip.is_loopback() && !ip.is_unspecified()
}

fn is_global_ipv6(ip: &Ipv6Addr) -> bool {
    !ip.is_loopback()
        && !ip.is_unspecified()
        && !ip.is_unique_local()
        && !ip.is_unicast_link_local()
        && !ip.is_multicast()
}
