#pragma once

#include "dusk/twinpad/Core/TwinpadProtocol.h"
#include "dusk/twinpad/Video/TwinpadH264.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Cliente del canal primario del Servidor Twinpad (loopback 8421). Un solo
// hilo de red hace todo (conectar, leer, codificar, escribir) para que no haya
// carreras entre canales y el hilo de juego nunca espere por un socket:
//
//   hilo de juego  --publishFrame()-->  [cola de 1 frame, drop-old]
//                                          |
//   hilo de red    <---- toma el frame ---+
//                  codifica H.264, base64url, escribe la línea `game_frame`
//                  lee `input_report` / `video_force_idr` y deja el último
//                  snapshot
//                                          |
//   hilo de juego  <----takeInput()--------+
//
// La vida del anuncio es la conexión (liveness de SPEC-006 §6): al caer el
// servidor se reintenta cada 2 s y se re-anuncia; mientras tanto el pad del
// teléfono queda limpio.
namespace dusk::twinpad {

class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Idempotente. Arranca el hilo de red, que no conecta hasta el primer tick.
    void start();
    // Detiene y espera al hilo. Seguro aunque nunca se haya llamado a start().
    void stop();

    // Ofrece un frame del panel. Sustituye al pendiente (drop-old): si el hilo
    // de red no da abasto, la Segunda pantalla se pierde frames, nunca se
    // acumulan. Devuelve false si no hay conexión o el frame no tiene tamaño.
    bool publishFrame(std::vector<uint8_t> rgba, uint32_t width, uint32_t height);

    // Último snapshot aceptado. Devuelve false antes del primer reporte y tras
    // una desconexión (el llamador debe limpiar el pad virtual).
    bool takeInput(InputReport& out) const;

    bool connected() const { return mConnected.load(); }
    uint64_t framesSent() const { return mFramesSent.load(); }

private:
    struct Frame {
        std::vector<uint8_t> rgba;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    void workerMain();
    void resetConnectionState();

    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::optional<Frame> mPendingFrame;
    InputReport mInput{};
    bool mHasInput = false;
    uint32_t mLastSeq = 0;
    bool mHasSeq = false;

    std::atomic<bool> mRunning{false};
    std::atomic<bool> mConnected{false};
    std::atomic<uint64_t> mFramesSent{0};
    std::thread mWorker;

    // Solo los toca el hilo de red: el frame se codifica en él y la petición de
    // keyframe llega por su propia lectura del socket.
    H264Encoder mEncoder;
    bool mForceIdr = true;
    // Última `video_config` de la sesión (SPEC-003 §4): resolución de emisión y
    // bitrate que el drawer de la app pidió. Por defecto, panel nativo a
    // 8.000 kbps — el mismo valor que anuncia el Servidor.
    VideoConfig mVideo;
};

}  // namespace dusk::twinpad
