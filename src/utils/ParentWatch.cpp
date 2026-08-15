#include "utils/ParentWatch.h"

#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

namespace hms_cpap {

namespace {

/// One blocking read. Returns bytes read, 0 for EOF, -1 for a real error.
long readOnce(int fd, char* buf, unsigned long len) {
#ifdef _WIN32
    return ::_read(fd, buf, static_cast<unsigned int>(len));
#else
    for (;;) {
        const auto n = ::read(fd, buf, len);
        // A signal interrupting the read is not the parent dying. Retrying is
        // the difference between surviving a SIGWINCH and shutting the service
        // down because someone resized a terminal.
        if (n < 0 && errno == EINTR) continue;
        return static_cast<long>(n);
    }
#endif
}

}  // namespace

void watchParentPipe(int fd, std::function<void()> on_parent_gone) {
    std::thread([fd, cb = std::move(on_parent_gone)]() {
        char buf[256];
        for (;;) {
            const long n = readOnce(fd, buf, sizeof(buf));
            if (n > 0) continue;   // data is not the signal; only closure is
            break;                 // 0 = EOF, <0 = the far end is unusable
        }
        if (cb) cb();
    }).detach();
}

}  // namespace hms_cpap
