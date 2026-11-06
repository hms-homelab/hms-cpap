// Sockets for the tests' one-shot loopback HTTP servers, on Winsock as well.
//
// Same split as src/services/PreflightService.cpp: Winsock on Windows, BSD
// sockets elsewhere. A socket is kept in an int on both, as the tests always
// have; closeSocket() closes it, and Winsock is started once per process.
// setsockopt's option pointer is const char* on Winsock and const void* on
// POSIX, so pass it as reinterpret_cast<const char*>(&opt) to suit both.
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>
#pragma comment(lib, "Ws2_32.lib")
typedef SSIZE_T ssize_t;

namespace test_sockets {
struct WinsockInit {
    WinsockInit() { WSADATA wsa; ::WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~WinsockInit() { ::WSACleanup(); }
};
inline WinsockInit& winsockInit() { static WinsockInit init; return init; }
// Started before any test runs, from every file that includes this header.
static const WinsockInit& kWinsock = winsockInit();
}  // namespace test_sockets

inline int closeSocket(int s) { return ::closesocket(static_cast<SOCKET>(s)); }

// Wakes a thread blocked in accept() on fd. Winsock's shutdown() does not, so
// the socket is closed here and fd set to -1 (the later close skips it).
inline void stopListening(int& fd) {
    if (fd < 0) return;
    closeSocket(fd);
    fd = -1;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

inline int closeSocket(int s) { return ::close(s); }

// Wakes a thread blocked in accept() on fd; the fd is still closed after.
inline void stopListening(int& fd) {
    if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
}
#endif
