#include "dusk/twinpad/Client/TwinpadClient.h"

#include "dusk/twinpad/Core/TwinpadSocket.h"
#include "dusk/twinpad/Video/TwinpadH264.h"

#include <algorithm>
#include <utility>

namespace dusk::twinpad {
namespace {

// Reconexión del adaptador ante servidor ausente o reiniciado (SPEC-006 §9).
constexpr auto kReconnectDelay = std::chrono::seconds(2);

// Techo del buffer de salida: si el socket no drena, se dejan de encolar
// frames en vez de crecer sin límite. Telemetría y frames comparten cola.
constexpr size_t kMaxOutboxBytes = 8u * 1024u * 1024u;

// Pacing del bucle de red: sin datos, despierta 15 veces por segundo para
// mirar si el hilo de juego dejó un frame nuevo.
constexpr int kIdleWaitMs = 15;

}  // namespace

Client::~Client() {
    stop();
}

void Client::start() {
    bool expected = false;
    if (!mRunning.compare_exchange_strong(expected, true)) {
        return;
    }
    mWorker = std::thread([this] { workerMain(); });
}

void Client::stop() {
    if (!mRunning.exchange(false)) {
        std::lock_guard lock{mMutex};
        mPendingFrame.reset();
        return;
    }
    mWake.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
    std::lock_guard lock{mMutex};
    mPendingFrame.reset();
}

bool Client::publishFrame(std::vector<uint8_t> rgba, uint32_t width, uint32_t height) {
    if (!mConnected.load() || width == 0 || height == 0 ||
        rgba.size() != static_cast<size_t>(width) * height * 4) {
        return false;
    }
    {
        std::lock_guard lock{mMutex};
        mPendingFrame = Frame{std::move(rgba), width, height};
    }
    mWake.notify_one();
    return true;
}

bool Client::takeInput(InputReport& out) const {
    std::lock_guard lock{mMutex};
    if (!mHasInput) {
        return false;
    }
    out = mInput;
    return true;
}

void Client::resetConnectionState() {
    mConnected = false;
    std::lock_guard lock{mMutex};
    // Sin conexión no hay demanda vigente: el pad virtual se limpia en el
    // llamador y el teléfono no reanuda con una pulsación vieja.
    mHasInput = false;
    mHasSeq = false;
    mPendingFrame.reset();
}

void Client::workerMain() {
    Socket socket;
    LineBuffer lines;
    std::string outbox;
    char chunk[64 * 1024];
    uint64_t gfn = 0;
    auto nextAttempt = std::chrono::steady_clock::now();

    while (mRunning.load()) {
        if (!socket.valid()) {
            const auto now = std::chrono::steady_clock::now();
            if (now < nextAttempt) {
                std::unique_lock lock{mMutex};
                mWake.wait_for(lock, std::chrono::milliseconds(50),
                    [this] { return !mRunning.load(); });
                continue;
            }
            nextAttempt = now + kReconnectDelay;
            if (!socket.connectLoopback(kGameIpcPort)) {
                continue;
            }
            // Conexión nueva = anuncio nuevo; `gfn` es monotónico POR conexión
            // (SPEC-003 §4), no por proceso.
            gfn = 0;
            lines.clear();
            // Conexión nueva = el cliente que entra necesita SPS/PPS y un IDR.
            mForceIdr = true;
            outbox.assign(BuildAnnounceLine());
            outbox.push_back('\n');
            mConnected = true;
            continue;
        }

        // Codificar aquí (no en el hilo de juego) y solo si la cola de salida
        // cabe: con el enlace atascado se prefiere perder frames a encolar.
        if (outbox.size() < kMaxOutboxBytes) {
            std::optional<Frame> frame;
            {
                std::lock_guard lock{mMutex};
                if (mPendingFrame) {
                    frame = std::move(mPendingFrame);
                    mPendingFrame.reset();
                }
            }
            if (frame) {
                std::vector<uint8_t> annexB;
                uint64_t timestampUs = 0;
                bool idr = false;
                if (mEncoder.encode(frame->rgba.data(), frame->width, frame->height,
                        mForceIdr, annexB, timestampUs, idr)) {
                    // Si se pidió keyframe y este frame no lo llevó, se reintenta
                    // en el siguiente; una vez emitido no se vuelve a forzar.
                    mForceIdr = mForceIdr && !idr;
                    outbox += BuildFrameLine(gfn, static_cast<uint16_t>(mEncoder.width()),
                        static_cast<uint16_t>(mEncoder.height()), timestampUs, idr,
                        Base64UrlEncode(annexB.data(), annexB.size()));
                    outbox.push_back('\n');
                    ++gfn;
                    mFramesSent.fetch_add(1);
                }
            }
        }

        // Pacing: select() sobre lectura (y escritura si hay algo pendiente). El
        // resultado no se usa; ambas operaciones de abajo son no bloqueantes y
        // así el bucle no depende de que select despierte por el lado correcto.
        socket.wait(true, !outbox.empty(), kIdleWaitMs);

        size_t received = 0;
        const IoStatus rx = socket.receive(chunk, sizeof(chunk), received);
        if (rx == IoStatus::Closed || rx == IoStatus::Error) {
            socket.close();
            resetConnectionState();
            continue;
        }
        if (rx == IoStatus::Ok && received > 0) {
            lines.append(chunk, received);
            std::string line;
            while (lines.next(line)) {
                if (IsForceIdrLine(line)) {
                    mForceIdr = true;
                    continue;
                }
                InputReport report;
                if (!ParseInputReport(line, report)) {
                    // Línea ajena o inválida: por este canal el servidor solo
                    // manda `input_report` y `video_force_idr`, así que se
                    // descarta.
                    continue;
                }
                std::lock_guard lock{mMutex};
                // El orden lo determina `seq` (SPEC-004 R2): un repetido o un
                // reporte viejo no reemplaza al vigente.
                if (mHasSeq && static_cast<int32_t>(report.seq - mLastSeq) <= 0) {
                    continue;
                }
                mLastSeq = report.seq;
                mHasSeq = true;
                mInput = report;
                mHasInput = true;
            }
        }

        if (!outbox.empty()) {
            size_t sent = 0;
            const IoStatus tx = socket.send(outbox.data(), outbox.size(), sent);
            if (tx == IoStatus::Error || tx == IoStatus::Closed) {
                socket.close();
                resetConnectionState();
                continue;
            }
            if (sent > 0) {
                outbox.erase(0, sent);
            }
        }
    }

    socket.close();
    resetConnectionState();
}

}  // namespace dusk::twinpad
