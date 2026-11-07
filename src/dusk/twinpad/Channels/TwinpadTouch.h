#pragma once

#include "dusk/twinpad/Core/TwinpadProtocol.h"

// Traducción pura del táctil del `input_report` (SPEC-004 §4) a la convención
// de gestos que ya consume la capa táctil del panel: 0 = down, 1 = move,
// 2 = up. Sin dependencias del juego ni del companion, para poder probarla en
// host como el resto del contrato.
namespace dusk::twinpad {

struct TouchEvent {
    int action = 0;
    float u = 0.0f;
    float v = 0.0f;
};

class TouchTracker {
public:
    // Devuelve true cuando el reporte produce un gesto nuevo. Como mucho uno
    // por llamada (el panel consume un gesto por frame): un down y un up del
    // mismo frame no pueden entregarse juntos.
    bool update(const InputReport& report, TouchEvent& out);

    // Suelta un dedo que quedó apoyado (desconexión del teléfono): sin esto el
    // panel retendría la pulsación para siempre.
    bool release(TouchEvent& out);

private:
    static float normalize(uint16_t value);

    bool mDown = false;
    uint16_t mX = 0;
    uint16_t mY = 0;
};

}  // namespace dusk::twinpad
