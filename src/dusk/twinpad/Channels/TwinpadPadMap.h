#pragma once

#include "dusk/twinpad/Core/TwinpadProtocol.h"

// Traducción pura de `input_report` a la convención de pad del juego. No
// incluye <dolphin/pad.h> ni toca PADSetVirtualStatus a propósito: así el mapeo
// se prueba en host (tests/) sin Aurora, igual que los tests de wire.
namespace dusk::twinpad {

// Estado de un pad GameCube ya traducido. Sin dependencias del ABI del juego.
struct PadState {
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool start = false;
    bool z = false;
    bool l = false;
    bool r = false;
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    // -1..1 en la convención del juego (Y+ = arriba).
    float stickX = 0.0f;
    float stickY = 0.0f;
    float substickX = 0.0f;
    float substickY = 0.0f;
    // 0..1 de los gatillos analógicos.
    float triggerLeft = 0.0f;
    float triggerRight = 0.0f;

    // true si algo está presionado o movido: sin esto, el adaptador limpia el
    // pad virtual en vez de dejar un botón pulsado para siempre.
    bool active() const;
};

// Mapeo v1 del teléfono al pad de GameCube:
//   A/B/X/Y        -> A/B/X/Y
//   START          -> START
//   L1 / L2        -> L digital + gatillo analógico izquierdo
//   R1 / R2        -> R digital + gatillo analógico derecho
//   EXTRA_1/2      -> Z (el teléfono no tiene un botón Z propio)
//   L3/R3/SELECT   -> sin equivalente en el pad de GameCube; se ignoran
//   d-pad 0..7     -> dirección (8 = neutro)
//   stick_l*       -> stick principal (Y invertida: Android Y+ = abajo)
//   stick_r*       -> C-stick (Y invertida)
// El táctil del `input_report` no se usa: el panel ya se maneja con el táctil
// nativo del companion (src/dusk/companion_touch.cpp).
PadState mapInput(const InputReport& report);

}  // namespace dusk::twinpad
