#include "cli.h"

#include "session.h"

#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace fb {
namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

QTextStream& out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream& err() {
    static QTextStream s(stderr);
    return s;
}

void print_usage() {
    out() << "Folder Buddies — share a folder as a real, mounted disk.\n\n"
          << "Usage:\n"
          << "  folderbuddies host <folder> [options]\n"
          << "      --lan               share on this LAN only (don't expose to the internet)\n"
          << "      --port <n>          listen port (default: auto / OS-chosen)\n"
          << "      --max-clients <n>   limit distinct clients (default: unlimited)\n"
          << "      (prints a 6-char room code; if the Cloudflare Worker is unavailable,\n"
          << "       prints a long self-contained offline Base91 blob instead)\n\n"
          << "  folderbuddies connect <room-code-or-offline-blob> [options]\n"
          << "      --mount <dir>       base mount directory (default: ~/FolderBuddies)\n"
          << "      --conns <n>         parallel connections (default: "
          << kDefaultConns << ")\n\n"
          << "  With no subcommand the graphical app is launched.\n";
    out().flush();
}

struct Args {
    std::string positional;
    std::optional<std::string> get(std::string_view name) const {
        auto it = opts.find(std::string(name));
        return it == opts.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
    bool has(std::string_view name) const { return flags.count(std::string(name)) > 0; }
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;
};

bool takes_value(std::string_view f) {
    return f == "--port" || f == "--max-clients" || f == "--mount" || f == "--conns";
}

bool parse(int argc, char** argv, int start, Args& a, std::string& perr) {
    for (int i = start; i < argc; ++i) {
        std::string tok = argv[i];
        if (tok.rfind("--", 0) == 0) {
            if (takes_value(tok)) {
                if (i + 1 >= argc) { perr = "missing value for " + tok; return false; }
                a.opts[tok] = argv[++i];
            } else {
                a.flags.insert(tok);
            }
        } else if (a.positional.empty()) {
            a.positional = tok;
        } else {
            perr = "unexpected argument: " + tok;
            return false;
        }
    }
    return true;
}

int run_until_signal() {
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [] {
        if (g_stop) QCoreApplication::quit();
    });
    poll.start(200);
    return QCoreApplication::exec();
}

int cli_host(const Args& a) {
    if (a.positional.empty()) { err() << "host: missing <folder>\n"; err().flush(); return 2; }

    int port = a.get("--port") ? std::stoi(*a.get("--port")) : 0;
    int maxClients = a.get("--max-clients") ? std::stoi(*a.get("--max-clients")) : 0;

    Server server;
    Upnp upnp;
    server.onClientsChanged = [&server] {
        out() << "[clients: " << server.clientCount() << "]\n";
        out().flush();
    };

    HostedShareTicket ticket;
    std::string e;
    if (!start_hosting(server, upnp, a.positional, port, maxClients, a.has("--lan"),
                       a.has("--write"), ticket, e)) {
        err() << "host failed: " << QString::fromStdString(e) << "\n";
        err().flush();
        return 1;
    }

    out() << "Sharing \"" << QString::fromStdString(server.shareName) << "\" on port "
          << server.boundPort << "\n"
          << "  " << QString::fromStdString(ticket.reach) << "\n"
          << "  signaling: " << QString::fromStdString(ticket.cloudStatus) << "\n"
          << "  access: " << (a.has("--write") ? "read/write" : "read-only") << "\n"
          << "  encryption: ChaCha20-Poly1305 (always on)\n\n";
    if (ticket.cloudPublished) {
        out() << "Room code (exactly 6 Base91 chars):\n  "
              << QString::fromStdString(ticket.roomCode) << "\n\n";
    } else {
        out() << "Offline Base91 blob:\n  "
              << QString::fromStdString(ticket.offlineBlob) << "\n\n";
    }
    out() << "Share only the code/blob — no password. Cloudflare never receives the\n"
          << "IP, port, data-path secret, or the secret half of the code.\n"
          << "Press Ctrl+C to stop sharing.\n";
    out().flush();

    int rc = run_until_signal();
    out() << "\nStopping…\n";
    out().flush();
    if (ticket.cloudPublished) {
        std::string derr;
        SignalingClient().remove(ticket.lookupId, ticket.ownerToken, derr);
    }
    upnp.unmap();
    server.stop();
    return rc;
}

int cli_connect(const Args& a) {
    if (a.positional.empty()) { err() << "connect: missing <room-code-or-offline-blob>\n"; err().flush(); return 2; }

    Token tok;
    std::string decodeErr;
    if (!resolve_share_code(a.positional, tok, decodeErr)) {
        err() << "connect: " << QString::fromStdString(decodeErr) << "\n";
        err().flush();
        return 2;
    }

    std::string mountBase =
        a.get("--mount").value_or((QDir::homePath() + "/FolderBuddies").toStdString());
    int conns = a.get("--conns") ? std::stoi(*a.get("--conns")) : kDefaultConns;

    Client client;
    Mount mount;
    std::string mountpoint, e;
    if (!start_mounting(client, mount, tok, mountBase, conns, mountpoint, e)) {
        err() << "connect failed: " << QString::fromStdString(e) << "\n";
        err().flush();
        return 1;
    }

    out() << "Mounted \"" << QString::fromStdString(tok.folder) << "\" as "
          << QString::fromStdString(mountpoint) << "\n"
          << "It behaves like a local disk; only the bytes apps actually read cross the wire.\n\n"
          << "Press Ctrl+C to unmount.\n";
    out().flush();

    int rc = run_until_signal();
    out() << "\nUnmounting…\n";
    out().flush();
    mount.stop();
    client.disconnect();
    return rc;
}

} // namespace

bool is_cli_invocation(int argc, char** argv) {
    if (argc < 2) return false;
    std::string_view c = argv[1];
    return c == "host" || c == "connect" || c == "help" || c == "--help" || c == "-h";
}

int run_cli(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Folder Buddies");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::string_view cmd = argv[1];
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        print_usage();
        return 0;
    }

    Args a;
    std::string perr;
    if (!parse(argc, argv, 2, a, perr)) {
        err() << QString::fromStdString(cmd == "host" ? "host: " : "connect: ")
              << QString::fromStdString(perr) << "\n";
        err().flush();
        return 2;
    }

    try {
        return cmd == "host" ? cli_host(a) : cli_connect(a);
    } catch (const std::exception& ex) {
        err() << "error: " << ex.what() << "\n";
        err().flush();
        return 2;
    }
}

} // namespace fb
