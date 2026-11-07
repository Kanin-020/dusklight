#include "dusk/twinpad/Core/TwinpadSocket.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dusk::twinpad {
namespace {

#ifdef _WIN32

using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

void ensureWinsock() {
    // Winsock exige WSAStartup una vez por proceso. Sin cerrarlo: el adaptador
    // vive lo que vive el juego y WSAStartup es idempotente.
    static const bool started = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}

void closeNative(NativeSocket socket) {
    closesocket(socket);
}

bool wouldBlock() {
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

#else

using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;

void ensureWinsock() {}

void closeNative(NativeSocket socket) {
    ::close(socket);
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS;
}

#endif

}  // namespace

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : mHandle(other.mHandle) {
    other.mHandle = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        mHandle = other.mHandle;
        other.mHandle = -1;
    }
    return *this;
}

bool Socket::connectLoopback(uint16_t port) {
    close();
    ensureWinsock();

    const NativeSocket handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == kInvalidSocket) {
        return false;
    }
    mHandle = static_cast<std::intptr_t>(handle);

#ifdef _WIN32
    u_long nonBlocking = 1;
    if (ioctlsocket(handle, FIONBIO, &nonBlocking) != 0) {
        close();
        return false;
    }
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        return false;
    }
#endif

    // Un frame JPEG por escritura sin Nagle: el stream es de baja latencia y
    // cada línea ya es un mensaje completo.
    const int noDelay = 1;
    setsockopt(handle, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    if (!wouldBlock()) {
        close();
        return false;
    }
    if (!wait(false, true, 250)) {
        close();
        return false;
    }
    // select() de escritura también se levanta con el error de conexión: el
    // SO_ERROR es lo único que distingue "conectado" de "rechazado".
    int pendingError = 0;
#ifdef _WIN32
    int length = sizeof(pendingError);
    if (getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pendingError),
            &length) != 0) {
        close();
        return false;
    }
#else
    socklen_t length = sizeof(pendingError);
    if (getsockopt(handle, SOL_SOCKET, SO_ERROR, &pendingError, &length) != 0) {
        close();
        return false;
    }
#endif
    if (pendingError != 0) {
        close();
        return false;
    }
    return true;
}

bool Socket::wait(bool readable, bool writable, int timeoutMs) {
    if (!valid()) {
        return false;
    }
    const NativeSocket handle = static_cast<NativeSocket>(mHandle);

    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    if (readable) {
        FD_SET(handle, &readSet);
    }
    if (writable) {
        FD_SET(handle, &writeSet);
    }

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int ready = ::select(static_cast<int>(handle) + 1, readable ? &readSet : nullptr,
        writable ? &writeSet : nullptr, nullptr, &timeout);
    if (ready < 0) {
        return false;
    }
    return ready > 0;
}

IoStatus Socket::send(const char* data, size_t size, size_t& sent) {
    sent = 0;
    if (!valid() || size == 0) {
        return IoStatus::WouldBlock;
    }
    const auto result =
        ::send(static_cast<NativeSocket>(mHandle), data, static_cast<int>(size), 0);
    if (result > 0) {
        sent = static_cast<size_t>(result);
        return IoStatus::Ok;
    }
    if (wouldBlock()) {
        return IoStatus::WouldBlock;
    }
    return IoStatus::Error;
}

IoStatus Socket::receive(char* data, size_t size, size_t& received) {
    received = 0;
    if (!valid() || size == 0) {
        return IoStatus::WouldBlock;
    }
    const auto result =
        ::recv(static_cast<NativeSocket>(mHandle), data, static_cast<int>(size), 0);
    if (result > 0) {
        received = static_cast<size_t>(result);
        return IoStatus::Ok;
    }
    if (result == 0) {
        // EOF: el servidor cerró (anuncio rechazado, juego duplicado o reinicio).
        return IoStatus::Closed;
    }
    if (wouldBlock()) {
        return IoStatus::WouldBlock;
    }
    return IoStatus::Error;
}

bool Socket::valid() const {
    return mHandle != -1;
}

void Socket::close() {
    if (mHandle != -1) {
        closeNative(static_cast<NativeSocket>(mHandle));
        mHandle = -1;
    }
}

}  // namespace dusk::twinpad
