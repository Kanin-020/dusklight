#include "dusk/twinpad/TwinpadHooks.h"

// El adaptador entero se compila siempre (files.cmake) pero no existe en el
// binario si el usuario no activa DUSK_TWINPAD, igual que el spike del teléfono.
#ifndef DUSK_TWINPAD
#define DUSK_TWINPAD 0
#endif

#if DUSK_TWINPAD

#include "dusk/logging.h"
#include "dusk/settings.h"
#include "dusk/twinpad/Channels/TwinpadPadMap.h"
#include "dusk/twinpad/Client/TwinpadClient.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <aurora/aux_window.hpp>
#include <dolphin/pad.h>

namespace dusk::twinpad {
namespace {

Client sClient;
bool sStarted = false;
bool sCaptureArmed = false;
bool sWasConnected = false;
// Si este adaptador fue el último en escribir el pad virtual. El punto de
// inyección es compartido con los controles táctiles, así que solo se limpia
// cuando había algo aplicado: limpiar en cada frame sin input pisaría al táctil.
bool sPadApplied = false;
// Puerto al que se aplicó el pad virtual, para limpiar el correcto si el
// usuario cambia de dispositivo con el teléfono conectado.
int sAppliedPort = -1;
bool sLoggedFirstInput = false;
uint64_t sLastLoggedFrames = 0;

// Puerto que Twinpad ocupa al conectar sin que el usuario haya elegido otro.
constexpr int kAutoPort = 0;
bool sAutoSelected = false;
s32 sPreviousIndex = -1;
bool sPreviousKeyboard = false;

s8 stickValue(float value) {
    return static_cast<s8>(std::clamp(std::lround(value * 127.0f), -127l, 127l));
}

// Aplica el pad del teléfono en el puerto elegido: el mismo punto de inyección
// que ya usan los controles táctiles (touch_controls.cpp) y el spike del
// teléfono, así que el juego no necesita ningún cambio para leerlo. PADRead
// fusiona el pad virtual de cada canal, tenga o no un mando SDL asignado.
void applyPad(const PadState* pad, int port) {
    if (pad == nullptr || !pad->active()) {
        // Sin reporte vigente o tras una desconexión: nunca dejar un botón
        // pulsado para siempre.
        PADClearVirtualStatus(static_cast<u32>(port));
        return;
    }

    PADStatus status{};
    status.err = PAD_ERR_NONE;
    if (pad->a) {
        status.button |= PAD_BUTTON_A;
    }
    if (pad->b) {
        status.button |= PAD_BUTTON_B;
    }
    if (pad->x) {
        status.button |= PAD_BUTTON_X;
    }
    if (pad->y) {
        status.button |= PAD_BUTTON_Y;
    }
    if (pad->start) {
        status.button |= PAD_BUTTON_START;
    }
    if (pad->z) {
        status.button |= PAD_TRIGGER_Z;
    }
    if (pad->l) {
        status.button |= PAD_TRIGGER_L;
    }
    if (pad->r) {
        status.button |= PAD_TRIGGER_R;
    }
    if (pad->up) {
        status.button |= PAD_BUTTON_UP;
    }
    if (pad->down) {
        status.button |= PAD_BUTTON_DOWN;
    }
    if (pad->left) {
        status.button |= PAD_BUTTON_LEFT;
    }
    if (pad->right) {
        status.button |= PAD_BUTTON_RIGHT;
    }

    status.stickX = stickValue(pad->stickX);
    status.stickY = stickValue(pad->stickY);
    status.substickX = stickValue(pad->substickX);
    status.substickY = stickValue(pad->substickY);
    status.triggerLeft = static_cast<u8>(std::clamp(pad->triggerLeft, 0.0f, 1.0f) * 255.0f);
    status.triggerRight = static_cast<u8>(std::clamp(pad->triggerRight, 0.0f, 1.0f) * 255.0f);
    PADSetVirtualStatus(static_cast<u32>(port), &status);
}

void releasePad() {
    if (sPadApplied && sAppliedPort >= 0) {
        PADClearVirtualStatus(static_cast<u32>(sAppliedPort));
    }
    sPadApplied = false;
    sAppliedPort = -1;
}

// Autoactivarse (decisión del owner): al conectar, Twinpad pasa a ser el
// dispositivo del puerto 1 y se recuerda quién lo ocupaba para devolverlo al
// desconectar. Si el usuario ya eligió un puerto, no se toca.
void autoSelect() {
    if (getSettings().game.twinpadInputPort.getValue() >= 0) {
        return;
    }
    u32 count = 0;
    sPreviousKeyboard = PADGetKeyButtonBindings(kAutoPort, &count) != nullptr;
    sPreviousIndex = PADGetIndexForPort(kAutoPort);
    // El puerto queda para Twinpad: un mando SDL o el teclado seguirían
    // escribiendo el mismo canal.
    PADClearPort(kAutoPort);
    PADSetKeyboardActive(kAutoPort, FALSE);
    PADSerializeMappings();
    getSettings().game.twinpadInputPort.setValue(kAutoPort);
    sAutoSelected = true;
    DuskLog.info("twinpad: seleccionado como dispositivo del puerto 1 al conectar");
}

void revertAutoSelection() {
    if (!sAutoSelected) {
        return;
    }
    sAutoSelected = false;
    getSettings().game.twinpadInputPort.setValue(-1);
    if (sPreviousKeyboard) {
        PADSetKeyboardActive(kAutoPort, TRUE);
    } else if (sPreviousIndex >= 0) {
        PADSetPortForIndex(static_cast<u32>(sPreviousIndex), kAutoPort);
    } else {
        PADClearPort(kAutoPort);
    }
    PADSerializeMappings();
    sPreviousIndex = -1;
    sPreviousKeyboard = false;
}

}  // namespace

void applyInput() {
    if (!sStarted || !getSettings().game.twinpadEnabled.getValue()) {
        return;
    }

    const int port = getSettings().game.twinpadInputPort.getValue();
    if (port < 0 || port >= PAD_CHANMAX) {
        // Sin Twinpad seleccionado en «Device» nadie le pidió el mando: el canal
        // pertenece al táctil o a un gamepad y no se escribe.
        releasePad();
        return;
    }
    if (sPadApplied && sAppliedPort != port) {
        // Cambió el puerto con el teléfono conectado: no dejar el anterior pulsado.
        PADClearVirtualStatus(static_cast<u32>(sAppliedPort));
        sPadApplied = false;
    }

    InputReport report;
    const bool haveInput = sClient.takeInput(report);
    if (haveInput) {
        if (!sLoggedFirstInput) {
            sLoggedFirstInput = true;
            // Diagnóstico: sin esta línea, un mando que no responde no se
            // distingue de un teléfono que no manda reportes.
            DuskLog.info("twinpad: primer input_report del teléfono aplicado al pad");
        }
        const PadState pad = mapInput(report);
        applyPad(&pad, port);
        sPadApplied = true;
        sAppliedPort = port;
    } else if (sPadApplied) {
        // Se perdió la demanda (desconexión o sin reporte vigente): se suelta
        // una sola vez para no dejar un botón pulsado para siempre.
        applyPad(nullptr, port);
        sPadApplied = false;
    }
}

void tick(bool hudEnabled) {
    // Ajuste «Twinpad» (Ajustes → Twinpad): apagado no hay hilo, ni pad, ni
    // captura. Se comprueba cada frame porque es conmutable sin reiniciar.
    if (!getSettings().game.twinpadEnabled.getValue()) {
        if (sStarted) {
            DuskLog.info("twinpad: desactivado en los ajustes; cliente detenido");
            shutdown();
        }
        return;
    }

    if (!sStarted) {
        sClient.start();
        sStarted = true;
    }

    const bool connected = sClient.connected();
    if (connected != sWasConnected) {
        sWasConnected = connected;
        if (connected) {
            DuskLog.info("twinpad: servidor conectado en 127.0.0.1:{}", kGameIpcPort);
            autoSelect();
        } else {
            DuskLog.info("twinpad: sin servidor; reintento cada 2 s");
            releasePad();
            revertAutoSelection();
        }
    }

    if (!hudEnabled || !connected) {
        // Sin panel presentándose no se arma captura: el readback se paga por
        // frame y no tiene destino.
        sCaptureArmed = false;
        return;
    }

    if (!sCaptureArmed) {
        aurora::auxwin::request_capture();
        sCaptureArmed = true;
        return;
    }

    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    if (!aurora::auxwin::take_capture(pixels, &width, &height) || width == 0 || height == 0) {
        return;  // readback del frame anterior todavía en vuelo
    }
    sCaptureArmed = false;

    // El panel llega en RGBA (el mismo formato que usa la captura de
    // referencia del companion); el cliente lo convierte y codifica en su hilo.
    if (sClient.publishFrame(std::move(pixels), width, height)) {
        const uint64_t frames = sClient.framesSent();
        if (frames >= sLastLoggedFrames + 300) {
            sLastLoggedFrames = frames;
            DuskLog.info("twinpad: {} frames del panel enviados", frames);
        }
    }
}

void shutdown() {
    if (!sStarted) {
        return;
    }
    sClient.stop();
    sStarted = false;
    sCaptureArmed = false;
    sWasConnected = false;
    releasePad();
    revertAutoSelection();
    sLoggedFirstInput = false;
}

}  // namespace dusk::twinpad

#endif  // DUSK_TWINPAD
