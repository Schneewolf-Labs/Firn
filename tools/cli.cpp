// firn-cli: the program's actions from a shell. It speaks the same socket
// the in-app driver listens on, so anything the API can do can be scripted
// without a mouse, and `describe` says what that is.
//
//   firn-cli describe                       every action, with its parameters
//   firn-cli do layer.new                   run an action
//   firn-cli do image.resize '{"width":800}'
//   firn-cli state                          what is open, as JSON
//   firn-cli shot out.png                   save what the window shows
//   firn-cli raw 'click 10 20' ...        the driver's own wire steps
//   firn-cli --launch [file] ...            start the app first and wait for it
//
// The socket is $FIRN_DRIVE, or /tmp/firn-drive.sock.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string socket_path() {
    if (const char* env = std::getenv("FIRN_DRIVE")) return env;
    return "/tmp/firn-drive.sock";
}

int connect_to(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { close(fd); return -1; }
    return fd;
}

// Starts the app in the background and waits for it to answer.
int launch(const std::string& path, const char* image) {
    const char* exe = std::getenv("FIRN_APP");
    std::string binary = exe ? exe : "firn";
    if (!exe) {
        // Prefer a build sitting beside this tool.
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            std::string dir(self);
            dir = dir.substr(0, dir.find_last_of('/'));
            struct stat st {};
            if (stat((dir + "/../app/firn").c_str(), &st) == 0) binary = dir + "/../app/firn";
        }
    }
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        setenv("FIRN_DRIVE", path.c_str(), 1);
        if (!std::getenv("FIRN_WINDOW")) setenv("FIRN_WINDOW", "1280x800", 1);
        const int null = open("/dev/null", O_WRONLY);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        if (image) execlp(binary.c_str(), binary.c_str(), image, nullptr);
        else execlp(binary.c_str(), binary.c_str(), nullptr);
        _exit(127);
    }
    for (int i = 0; i < 400; ++i) {   // up to 20 seconds
        const int fd = connect_to(path);
        if (fd >= 0) return fd;
        usleep(50000);
    }
    return -1;
}

// Sends one line and returns the reply, which the app writes once the frames
// for that command have run.
bool exchange(int fd, const std::string& line, std::string& reply) {
    const std::string out = line + "\n";
    size_t off = 0;
    while (off < out.size()) {
        const ssize_t n = write(fd, out.data() + off, out.size() - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    reply.clear();
    char buf[4096];
    while (reply.find('\n') == std::string::npos) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return false;
        reply.append(buf, static_cast<size_t>(n));
    }
    reply.erase(reply.find('\n'));
    return true;
}

int usage() {
    std::fprintf(stderr,
                 "usage: firn-cli [--socket PATH] [--launch [IMAGE]] <command>\n"
                 "  describe              every action and its parameters, as JSON\n"
                 "  do NAME [JSON]        run an action (describe lists them)\n"
                 "  state                 what is open, as JSON\n"
                 "  shot PATH             write what the window shows to a PNG\n"
                 "  raw STEP [STEP ...]   the driver's own wire steps, for replaying input\n"
                 "                        (\"click 100 200\", \"key z ctrl\", \"wait 2\")\n"
                 "  quit                  close the program\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = socket_path();
    bool do_launch = false;
    const char* image = nullptr;
    int i = 1;
    for (; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--socket" && i + 1 < argc) path = argv[++i];
        else if (arg == "--launch") {
            do_launch = true;
            if (i + 1 < argc && argv[i + 1][0] != '-' && std::string(argv[i + 1]).find('.') != std::string::npos &&
                std::string(argv[i + 1]) != "describe") image = argv[++i];
        } else break;
    }
    if (i >= argc) return usage();

    const int fd = do_launch ? launch(path, image) : connect_to(path);
    if (fd < 0) {
        std::fprintf(stderr, "firn-cli: no program listening on %s%s\n", path.c_str(),
                     do_launch ? " (it did not start)" : " (start it with FIRN_DRIVE set, or pass --launch)");
        return 1;
    }

    const std::string verb = argv[i++];
    std::vector<std::string> lines;
    if (verb == "describe") lines.push_back("do app.describe {}");
    else if (verb == "state") lines.push_back("state_json");
    else if (verb == "quit") lines.push_back("quit");
    else if (verb == "shot") {
        if (i >= argc) return usage();
        lines.push_back(std::string("shot ") + argv[i]);
    } else if (verb == "do") {
        if (i >= argc) return usage();
        const std::string name = argv[i++];
        const std::string params = i < argc ? argv[i] : "{}";
        lines.push_back("do " + name + " " + params);
    } else if (verb == "raw") {
        for (; i < argc; ++i) lines.push_back(argv[i]);
    } else {
        return usage();
    }

    int status = 0;
    for (const std::string& line : lines) {
        std::string reply;
        if (!exchange(fd, line, reply)) { std::fprintf(stderr, "firn-cli: the program stopped answering\n"); status = 1; break; }
        // Replies are "ok <payload>"; the payload is JSON for the action calls.
        const std::string payload = reply.rfind("ok ", 0) == 0 ? reply.substr(3) : reply;
        if (payload.rfind("error", 0) == 0 || payload.rfind("result error", 0) == 0) {
            std::fprintf(stderr, "%s\n", payload.c_str());
            status = 1;
            break;
        }
        std::printf("%s\n", payload.rfind("result ", 0) == 0 ? payload.substr(7).c_str() : payload.c_str());
    }
    close(fd);
    return status;
}
