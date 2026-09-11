#pragma once
// The driver's address on Windows, shared by the app (Drive.cpp) and
// firn-cli (tools/cli.cpp); scripts/drive.py reads the same file.
//
// Windows has Unix sockets, but Python there cannot open one, so the driver
// listens on a loopback TCP port instead and FIRN_DRIVE names a file holding
// "firn-drive <port> <token>". A client connects to the port and sends the
// token as its first line; the driver drops a connection that does not, so
// neither another user nor a web page that finds the port can drive the
// program. The file sits in the user's own temp folder, which other users
// cannot read.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <fstream>
#include <string>

namespace drive_address {

inline bool read(const std::string& path, int& port, std::string& token) {
    std::ifstream in(path);
    std::string magic;
    return static_cast<bool>(in >> magic >> port >> token) && magic == "firn-drive" && port > 0 && port < 65536;
}

// Connects to the driver the file names and presents its token.
// INVALID_SOCKET when the file is not an address or nothing listens there.
inline SOCKET connect(const std::string& path) {
    int port = 0;
    std::string token;
    if (!read(path, port, token)) return INVALID_SOCKET;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    // One short line each way per command: Nagle would hold the second of
    // two back-to-back writes (the token, then a command) for the peer's ACK.
    const BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const std::string hello = token + "\n";
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::send(s, hello.data(), static_cast<int>(hello.size()), 0) != static_cast<int>(hello.size())) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

}  // namespace drive_address
#endif
