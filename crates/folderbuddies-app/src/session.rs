use std::{
    collections::{HashMap, HashSet},
    net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, UdpSocket},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::Duration,
};

use folderbuddies_core::{
    client::Client,
    native_transport::{NativeQuicClient, NativeQuicHost},
    server::Server,
    signaling::{
        Token, looks_like_room_code, publish_share, remove_published_room, resolve_share_code,
    },
};
use igd_next::{Gateway, PortMappingProtocol, search_gateway};

use super::mount::Mount;

const POLL_INTERVAL: Duration = Duration::from_millis(200);

#[derive(Debug, Default)]
struct CommandArgs {
    positional: String,
    options: HashMap<String, String>,
    flags: HashSet<String>,
}

impl CommandArgs {
    fn get(&self, name: &str) -> Option<&str> {
        self.options.get(name).map(String::as_str)
    }

    fn has(&self, name: &str) -> bool {
        self.flags.contains(name)
    }
}

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

pub(crate) fn run() -> i32 {
    let args: Vec<String> = std::env::args().collect();
    if !is_cli_invocation(&args) {
        return match super::gui::run_gui() {
            Ok(()) => 0,
            Err(error) => {
                eprintln!("Folder Buddies: {error}");
                1
            }
        };
    }
    let command = &args[1];
    if matches!(command.as_str(), "help" | "--help" | "-h") {
        print_usage();
        return 0;
    }
    let parsed = match parse_arguments(args[2..].to_vec()) {
        Ok(parsed) => parsed,
        Err(error) => {
            eprintln!(
                "{}: {error}",
                if command == "host" { "host" } else { "connect" }
            );
            return 2;
        }
    };
    match command.as_str() {
        "host" => {
            if parsed.positional.is_empty() {
                eprintln!("host: missing <folder>");
                return 2;
            }
            let port = match parsed.get("--port") {
                Some(value) => match cxx_stoi(value) {
                    Ok(port) => port as u16,
                    Err(error) => {
                        eprintln!("error: {error}");
                        return 2;
                    }
                },
                None => 0,
            };
            match run_host(parsed, port) {
                Ok(()) => 0,
                Err(error) => {
                    eprintln!("host failed: {error}");
                    1
                }
            }
        }
        "connect" => {
            if parsed.positional.is_empty() {
                eprintln!("connect: missing <room-code-or-offline-blob>");
                return 2;
            }
            match run_connect(parsed) {
                Ok(()) => 0,
                Err(error) => {
                    eprintln!("connect failed: {error}");
                    1
                }
            }
        }
        _ => match super::gui::run_gui() {
            Ok(()) => 0,
            Err(error) => {
                eprintln!("Folder Buddies: {error}");
                1
            }
        },
    }
}

fn is_cli_invocation(args: &[String]) -> bool {
    args.get(1).is_some_and(|command| {
        matches!(
            command.as_str(),
            "host" | "connect" | "help" | "--help" | "-h"
        )
    })
}

fn parse_arguments(args: Vec<String>) -> Result<CommandArgs, String> {
    let mut parsed = CommandArgs::default();
    let mut index = 0_usize;
    while index < args.len() {
        let token = &args[index];
        if token.starts_with("--") {
            if takes_value(token) {
                index += 1;
                let value = args
                    .get(index)
                    .ok_or_else(|| format!("missing value for {token}"))?;
                parsed.options.insert(token.clone(), value.clone());
            } else {
                parsed.flags.insert(token.clone());
            }
        } else if parsed.positional.is_empty() {
            parsed.positional.clone_from(token);
        } else {
            return Err(format!("unexpected argument: {token}"));
        }
        index += 1;
    }
    Ok(parsed)
}

fn takes_value(flag: &str) -> bool {
    matches!(flag, "--port" | "--conns")
}

fn run_host(args: CommandArgs, port: u16) -> Result<(), String> {
    let mut server = Server::start(&args.positional, port, args.has("--write"))?;

    let (advertised_ip, advertised_port, reach, _upnp_mapping) =
        advertised_endpoint(server.bound_port(), args.has("--lan"));
    let token = Token::new(
        advertised_ip.to_string(),
        advertised_port,
        server.secret().to_vec(),
        server.share_name().to_owned(),
        args.has("--write"),
    )?;
    let ticket = publish_share(&token, reach)?;
    let mut quic_host = if ticket.cloud_published() {
        NativeQuicHost::start(ticket.room_code(), server.bound_port()).ok()
    } else {
        None
    };

    println!(
        "Sharing \"{}\" on port {}\n  {}\n  signaling: {}\n  access: {}\n  encryption: ChaCha20-Poly1305 (always on)\n\nConnect code:\n  {}\n\nShare only that code — no password. Cloudflare never receives the\nIP, port, data-path secret, or the secret half of the code.\nPress Ctrl+C to stop sharing.",
        server.share_name(),
        server.bound_port(),
        ticket.reach(),
        ticket.cloud_status(),
        if args.has("--write") {
            "read/write"
        } else {
            "read-only"
        },
        ticket.connect_code(),
    );

    let stop = install_stop_handler()?;
    let mut last_clients = server.client_count();
    while !stop.load(Ordering::Acquire) && server.running() {
        let clients = server.client_count();
        if clients != last_clients {
            println!("[clients: {clients}]");
            last_clients = clients;
        }
        thread::sleep(POLL_INTERVAL);
    }

    println!("\nStopping…");
    if let Some(host) = quic_host.as_mut() {
        host.stop();
    }
    if ticket.cloud_published()
        && let Err(error) = remove_published_room(&ticket)
    {
        eprintln!("Folder Buddies: failed to remove published room: {error}");
    }
    server.stop();
    Ok(())
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

fn run_connect(args: CommandArgs) -> Result<(), String> {
    let code = args.positional.as_str();
    let token = resolve_share_code(code)?;
    let mut quic_error = String::new();
    let mut quic_client = None;
    let mut mounted = None;

    if looks_like_room_code(code) {
        match NativeQuicClient::connect(code, &token) {
            Ok(mut quic) => {
                let client = quic.client();
                match Mount::start(Arc::clone(&client), token.folder(), token.allow_writes()) {
                    Ok(mount) => {
                        mounted = Some((client, mount, "direct native QUIC via ICE/STUN"));
                        quic_client = Some(quic);
                    }
                    Err(_) => quic.disconnect(),
                }
            }
            Err(error) => quic_error = error,
        }
    }

    if mounted.is_none() {
        let client = Arc::new(Client::connect_default(&token).map_err(|tcp_error| {
            format!(
                "Direct QUIC failed: {}; direct TCP failed: {tcp_error}",
                if quic_error.is_empty() {
                    "unavailable"
                } else {
                    quic_error.as_str()
                }
            )
        })?);
        match Mount::start(Arc::clone(&client), token.folder(), token.allow_writes()) {
            Ok(mount) => mounted = Some((client, mount, "direct native TCP")),
            Err(tcp_error) => {
                client.disconnect();
                return Err(format!(
                    "Direct QUIC failed: {}; direct TCP failed: {tcp_error}",
                    if quic_error.is_empty() {
                        "unavailable"
                    } else {
                        quic_error.as_str()
                    }
                ));
            }
        }
    }

    let (client, mount, transport) = mounted.ok_or_else(|| "connect failed".to_owned())?;
    let mount_path: PathBuf = mount.mount_path().to_owned();

    println!(
        "Mounted \"{}\" as {}\nTransport: {transport}.\nIt behaves like a local disk; only the bytes apps actually read cross the wire.\n\nPress Ctrl+C to unmount, or eject the drive/volume in the OS.",
        token.folder(),
        mount_path.display(),
    );

    let stop = install_stop_handler()?;
    while !stop.load(Ordering::Acquire) && client.connected() {
        thread::sleep(POLL_INTERVAL);
    }

    println!("\nUnmounting…");
    let unmount_result = mount.unmount();
    if let Some(mut quic) = quic_client {
        quic.disconnect();
    } else {
        client.disconnect();
    }
    unmount_result
}

fn cxx_stoi(text: &str) -> Result<i32, String> {
    let text = text.trim_start();
    let bytes = text.as_bytes();
    let mut index = 0_usize;
    let negative = match bytes.first() {
        Some(b'+') => {
            index = 1;
            false
        }
        Some(b'-') => {
            index = 1;
            true
        }
        _ => false,
    };
    let start = index;
    let mut value = 0_i64;
    while let Some(digit) = bytes
        .get(index)
        .and_then(|byte| byte.is_ascii_digit().then_some(*byte))
    {
        value = value
            .checked_mul(10)
            .and_then(|value| value.checked_add(i64::from(digit - b'0')))
            .ok_or_else(|| "stoi".to_owned())?;
        index += 1;
    }
    if index == start {
        return Err("stoi".to_owned());
    }
    let value = if negative { -value } else { value };
    i32::try_from(value).map_err(|_| "stoi".to_owned())
}

fn install_stop_handler() -> Result<Arc<AtomicBool>, String> {
    let stop = Arc::new(AtomicBool::new(false));
    let signal_stop = Arc::clone(&stop);
    ctrlc::set_handler(move || signal_stop.store(true, Ordering::Release))
        .map_err(|error| format!("failed to install Ctrl+C handler: {error}"))?;
    Ok(stop)
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

fn print_usage() {
    println!(
        "Folder Buddies — share a folder as a real, mounted disk.\n\nUsage:\n  folderbuddies host <folder> [options]\n      --lan               share on this LAN only (don't expose to the internet)\n      --port <n>          listen port (default: auto / OS-chosen)\n      --write             allow clients to upload, edit, and delete files\n      (prints a connect code; a short room code when published via Cloudflare/Firebase,\n       or a longer self-contained offline Base91 blob when they are unavailable)\n\n  folderbuddies connect <room-code-or-offline-blob>\n      mounts automatically as a drive/volume at the platform default\n\n  With no subcommand the graphical app is launched."
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parser_matches_cpp_unknown_flag_and_unused_conns_behavior() {
        let parsed = parse_arguments(vec![
            "share-code".to_owned(),
            "--conns".to_owned(),
            "not-a-number".to_owned(),
            "--future-flag".to_owned(),
        ])
        .expect("parse");
        assert_eq!(parsed.positional, "share-code");
        assert_eq!(parsed.get("--conns"), Some("not-a-number"));
        assert!(parsed.has("--future-flag"));
    }

    #[test]
    fn parser_rejects_second_positional_like_cpp() {
        assert_eq!(
            parse_arguments(vec!["one".to_owned(), "two".to_owned()])
                .expect_err("second positional")
                .to_string(),
            "unexpected argument: two"
        );
    }

    #[test]
    fn cxx_stoi_accepts_trailing_text() {
        assert_eq!(cxx_stoi("  -123garbage").expect("stoi"), -123);
        assert_eq!(cxx_stoi("+42x").expect("stoi"), 42);
        assert!(cxx_stoi("x42").is_err());
    }
}
