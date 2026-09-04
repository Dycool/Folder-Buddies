use std::{
    net::{IpAddr, Ipv4Addr, SocketAddr, UdpSocket},
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
    signaling::{publish_share, remove_published_room, resolve_share_code},
};

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

    let advertised_ip = best_local_ip().unwrap_or(IpAddr::V4(Ipv4Addr::LOCALHOST));
    let reach = if args.lan_only {
        format!("LAN only — {advertised_ip}")
    } else {
        format!(
            "LAN reachable — {advertised_ip} (safe Rust UPnP/public endpoint discovery is not active yet)"
        )
    };
    let token = server.token(advertised_ip)?;
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
    if ticket.cloud_published() {
        if let Err(error) = remove_published_room(&ticket) {
            eprintln!("Folder Buddies: failed to remove published room: {error}");
        }
    }
    server.stop();
    Ok(())
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

fn best_local_ip() -> Option<IpAddr> {
    route_local_ip("[2001:4860:4860::8888]:80")
        .filter(is_usable_ip)
        .or_else(|| route_local_ip("1.1.1.1:80").filter(is_usable_ip))
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

fn print_usage() {
    println!(
        "Folder Buddies — share a folder as a real, mounted disk.\n\nUsage:\n  folderbuddies host <folder> [options]\n      --lan               share on this LAN only\n      --port <n>          listen port (default: auto / OS-chosen)\n      --write             allow clients to upload, edit, and delete files\n\n  folderbuddies connect <room-code-or-offline-blob> [--conns <n>]\n      mounts automatically as a drive/volume\n\n  With no subcommand the graphical app is launched."
    );
}
