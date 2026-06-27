#include "cli.h"

#include "session.h"
#include "web_compat.h"

#include <QCoreApplication>
#include <QString>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace fb {
namespace {

volatile std::sig_atomic_t g_stop = 0;
std::atomic_bool g_ejected{false};
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
          << "      --write             allow clients to upload, edit, and delete files\n"
          << "      (prints a connect code; a short room code when published via Cloudflare/Firebase,\n"
          << "       or a longer self-contained offline Base91 blob when they are unavailable)\n\n"
          << "  folderbuddies connect <room-code-or-offline-blob>\n"
          << "      mounts automatically as a drive/volume at the platform default\n\n"
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
    return f == "--port" || f == "--conns";
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
        if (g_stop || g_ejected.load()) QCoreApplication::quit();
    });
    poll.start(200);
    return QCoreApplication::exec();
}

int cli_host(const Args& a) {
    if (a.positional.empty()) { err() << "host: missing <folder>\n"; err().flush(); return 2; }

    int port = a.get("--port") ? std::stoi(*a.get("--port")) : 0;

    Server server;
    Upnp upnp;
    std::unique_ptr<WebRtcCompatHost> webCompat;

    auto printClientCount = [&server, &webCompat] {
        const int nativeClients = server.clientCount();
        const int browserClients = webCompat ? webCompat->clientCount() : 0;
        const int totalClients = nativeClients + browserClients;

        out() << "[clients: " << totalClients;
        if (browserClients > 0) {
            out() << " (" << nativeClients << " native, "
                  << browserClients << " browser)";
        }
        out() << "]\n";
        out().flush();
    };

    server.onClientsChanged = printClientCount;

    HostedShareTicket ticket;
    std::string e;
    if (!start_hosting(server, upnp, a.positional, port, a.has("--lan"),
                       a.has("--write"), ticket, e)) {
        err() << "host failed: " << QString::fromStdString(e) << "\n";
        err().flush();
        return 1;
    }

    if (ticket.cloudPublished && web_compat_available()) {
        webCompat = std::make_unique<WebRtcCompatHost>();
        webCompat->onClientsChanged = printClientCount;
        std::string werr;
        if (!webCompat->start(a.positional, ticket.roomCode, a.has("--write"), werr)) {
            out() << "  WebRTC compatibility disabled: " << QString::fromStdString(werr) << "\n";
        }
    }

    out() << "Sharing \"" << QString::fromStdString(server.shareName) << "\" on port "
          << server.boundPort << "\n"
          << "  " << QString::fromStdString(ticket.reach) << "\n"
          << "  signaling: " << QString::fromStdString(ticket.cloudStatus) << "\n"
          << "  access: " << (a.has("--write") ? "read/write" : "read-only") << "\n"
          << "  encryption: ChaCha20-Poly1305 (always on)\n\n"
          << "Connect code:\n  "
          << QString::fromStdString(ticket.connectCode) << "\n\n"
          << "Share only that code — no password. Cloudflare never receives the\n"
          << "IP, port, data-path secret, or the secret half of the code.\n"
          << "Press Ctrl+C to stop sharing.\n";
    out().flush();

    int rc = run_until_signal();
    out() << "\nStopping…\n";
    out().flush();
    if (webCompat) webCompat->stop();
    if (ticket.cloudPublished) {
        std::string derr;
        remove_published_room(ticket, derr);
    }
    upnp.unmap();
    server.stop();
    return rc;
}

int cli_connect(const Args& a) {
    if (a.positional.empty()) { err() << "connect: missing <room-code-or-offline-blob>\n"; err().flush(); return 2; }

    Token tok;
    std::string decodeErr, e, mountpoint;
    Mount mount;
    std::unique_ptr<Client> client;
    std::unique_ptr<WebRtcRemoteClient> webClient;
    std::string label = "share";
    g_ejected.store(false);
    mount.setEjectedCallback([] { g_ejected.store(true); });

    if (resolve_share_code(a.positional, tok, decodeErr)) {
        client = std::make_unique<Client>();
        if (start_mounting(*client, mount, tok, mountpoint, e)) {
            label = tok.folder;
        } else {
            client->disconnect();
            client.reset();
        }
    }

    if (!mount.active() && web_compat_available() && looks_like_web_compat_code(a.positional)) {
        webClient = std::make_unique<WebRtcRemoteClient>();
        if (webClient->connect(a.positional, e)) {
            label = "Web share";
            if (!mount.start(webClient.get(), "", label, webClient->canWrite(), e)) {
                webClient->disconnect();
                webClient.reset();
            } else {
                mountpoint = mount.mountpoint();
            }
        }
    }

    if (!mount.active()) {
        err() << "connect failed: " << QString::fromStdString(e.empty() ? decodeErr : e) << "\n";
        err().flush();
        return 1;
    }

    out() << "Mounted \"" << QString::fromStdString(label) << "\" as "
          << QString::fromStdString(mountpoint) << "\n"
          << (webClient ? "Transport: WebRTC compatibility (browser/native).\n" : "Transport: native TCP.\n")
          << "It behaves like a local disk; only the bytes apps actually read cross the wire.\n\n"
          << "Press Ctrl+C to unmount, or eject the drive/volume in the OS.\n";
    out().flush();

    int rc = run_until_signal();
    out() << (g_ejected.load() ? "\nEjected; disconnecting…\n" : "\nUnmounting…\n");
    out().flush();
    mount.stop();
    if (client) client->disconnect();
    if (webClient) webClient->disconnect();
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
