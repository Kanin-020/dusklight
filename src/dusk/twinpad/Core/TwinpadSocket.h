#pragma once

#include <cstddef>
#include <cstdint>

// Envoltorio mínimo de socket TCP no bloqueante. Confina Winsock/POSIX a una
// sola traducción: el cliente no ve handles ni errno, igual que la capa de
// transporte de Cemu (DES-004), para que el mismo código compile en Linux,
// Windows y Android sin ramas fuera de este archivo.
namespace dusk::twinpad {

enum class IoStatus {
    Ok,
    WouldBlock,
    Closed,
    Error,
};

class Socket {
public:
    Socket() = default;
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // Conecta a 127.0.0.1:port. Sin servidor escuchando falla de inmediato
    // (loopback), así que llamarlo desde el hilo de red es barato.
    bool connectLoopback(uint16_t port);

    // Espera a que el socket esté listo. `writable` debe pedirse solo con
    // bytes pendientes: pedir escritura siempre haría busy-loop.
    bool wait(bool readable, bool writable, int timeoutMs);

    IoStatus send(const char* data, size_t size, size_t& sent);
    IoStatus receive(char* data, size_t size, size_t& received);

    bool valid() const;
    void close();

private:
    std::intptr_t mHandle = -1;
};

}  // namespace dusk::twinpad
