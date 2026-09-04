use std::{
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
    server::Server,
    signaling::{Token, publish_share, remove_published_room, resolve_share_code},
};
use igd_next::{Gateway, PortMappingProtocol, search_gateway};

use super::mount::Mount;

const POLL_INTERVAL: Duration = Duration::from_millis(200);

#[derive(Debug, Default)]
struct CommandArgs {
    positional: Option<String>,
    port: Option<u16>,
    connections: Option<usize>,
    lan_only: bool,
    allow_writes: bool,
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
            .add_any_port(
                PortMappingProtocol::TCP,
                local_address,
                0,
                "Folder Buddies",
            )
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

pub(crate) fn run() -> Result<(), String> {
    let mut args = std::env::args();
    let _program = args.next();
    let Some(command) = args.next() else {
        return super::gui::run_gui();
    };
    if matches!(command.as_str(), "help" | "--help" | "-h") {
        print_usage();
        return Ok(());
    }
    let parsed = parse_arguments(args.collect())?;
    match command.as_str() {
        "host" => run_host(parsed),
        "connect" => run_connect(parsed),
        _ => Err(format!("unknown command: {command}")),
    }
}

fn parse_arguments(args: Vec<String>) -> Result<CommandArgs, String> {
    let mut parsed = CommandArgs::default();
    let mut index = 0_usize;
    while index < args.len() {
        let token = &args[index];
        match token.as_str() {
            "--port" => {
                index += 1;
                let value = args
                    .get(index)
                    .ok_or_else(|| "missing value for --port".to_owned())?;
                parsed.port = Some(
                    value
                        .parse::<u16>()
                        .map_err(|_| format!("invalid port: {value}"))?,
                );
            }
            "--conns" => {
                index += 1;
                let value = args
                    .get(index)
                    .ok_or_else(|| "missing value for --conns".to_owned())?;
                let connections = value
                    .parse::<usize>()
                    .map_err(|_| format!("invalid connection count: {value}"))?;
                if connections == 0 || connections > 64 {
                    return Err("--conns must be between 1 and 64".to_owned());
                }
                parsed.connections = Some(connections);
            }
            "--lan" => parsed.lan_only = true,
            "--write" => parsed.allow_writes = true,
            flag if flag.starts_with("--") => return Err(format!("unknown option: {flag}")),
            positional if parsed.positional.is_none() => {
                parsed.positional = Some(positional.to_owned());
            }
            unexpected => return Err(format!("unexpected argument: {unexpected}")),
        }
        index += 1;
    }
    Ok(parsed)
}

fn run_host(args: CommandArgs) -> Result<(), String> {
    if args.connections.is_some() {
        return Err("--conns is only valid with connect".to_owned());
    }
    let folder = args
        .positional
        .as_deref()
        .ok_or_else(|| "host: missing <folder>".to_owned())?;
    let mut server = Server::start(folder, args.port.unwrap_or(0), args.allow_writes)?;

    let (advertised_ip, advertised_port, reach, _upnp_mapping) =
        advertised_endpoint(server.bound_port(), args.lan_only);
    let token = Token::new(
        advertised_ip.to_string(),
        advertised_port,
        server.secret().to_vec(),
        server.share_name().to_owned(),
        args.allow_writes,
    )?;
    let ticket = publish_share(&token, reach)?;

    println!(
        "Sharing \"{}\" on port {}\n  {}\n  signaling: {}\n  access: {}\n  encryption: ChaCha20-Poly1305 (always on)\n\nConnect code:\n  {}\n\nShare only that code — no password.\nPress Ctrl+C to stop sharing.",
        server.share_name(),
        server.bound_port(),
        ticket.reach(),
        ticket.cloud_status(),
        if args.allow_writes {
            "read/write"
        } else {
            "read-only"
        },
        ticket.connect_code(),
    );

    let stop = install_stop_handler()?;
    let mut last_clients = usize::MAX;
    while !stop.load(Ordering::Acquire) && server.running() {
        let clients = server.client_count();
        if clients != last_clients {
            println!("[clients: {clients}]");
            last_clients = clients;
        }
        thread::sleep(POLL_INTERVAL);
    }

    println!("\nStopping…");
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
        return (
            lan_ip,
            internal_port,
            format!("LAN only — {lan_ip}"),
            None,
        );
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
    if args.port.is_some() || args.lan_only || args.allow_writes {
        return Err("connect accepts only --conns".to_owned());
    }
    let code = args
        .positional
        .as_deref()
        .ok_or_else(|| "connect: missing <room-code-or-offline-blob>".to_owned())?;
    let token = resolve_share_code(code)?;
    let connections = args.connections.unwrap_or(0);
    let client = Arc::new(Client::connect(&token, connections)?);
    let mount = match Mount::start(Arc::clone(&client), token.folder(), token.allow_writes()) {
        Ok(mount) => mount,
        Err(error) => {
            client.disconnect();
            return Err(error);
        }
    };
    let mount_path: PathBuf = mount.mount_path().to_owned();

    println!(
        "Mounted \"{}\" as {}\nTransport: direct native TCP.\nIt behaves like a local disk; only the bytes apps actually read cross the wire.\n\nPress Ctrl+C to unmount.",
        token.folder(),
        mount_path.display(),
    );

    let stop = install_stop_handler()?;
    while !stop.load(Ordering::Acquire) && client.connected() {
        thread::sleep(POLL_INTERVAL);
    }

    println!("\nUnmounting…");
    let unmount_result = mount.unmount();
    client.disconnect();
    unmount_result
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
        "Folder Buddies — share a folder as a real, mounted disk.\n\nUsage:\n  folderbuddies host <folder> [options]\n      --lan               share on this LAN only (don't expose to the internet)\n      --port <n>          listen port (default: auto / OS-chosen)\n      --write             allow clients to upload, edit, and delete files\n\n  folderbuddies connect <room-code-or-offline-blob> [--conns <n>]\n      mounts automatically as a drive/volume\n\n  With no subcommand the graphical app is launched."
    );
}
