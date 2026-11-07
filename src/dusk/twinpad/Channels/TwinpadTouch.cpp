#include "dusk/twinpad/Channels/TwinpadTouch.h"

namespace dusk::twinpad {

float TouchTracker::normalize(uint16_t value) {
    return static_cast<float>(value) / 65535.0f;
}

bool TouchTracker::update(const InputReport& report, TouchEvent& out) {
    bool emit = false;
    if (report.touchPressed && !mDown) {
        out.action = 0;
        emit = true;
    } else if (!report.touchPressed && mDown) {
        out.action = 2;
        emit = true;
    } else if (report.touchPressed && (report.touchX != mX || report.touchY != mY)) {
        out.action = 1;
        emit = true;
    }

    // Al soltar, el reporte conserva la última coordenada (SPEC-004 §4), así
    // que el up cae donde estaba el dedo y no en el origen.
    const uint16_t x = report.touchX;
    const uint16_t y = report.touchY;
    mDown = report.touchPressed;
    mX = x;
    mY = y;

    if (!emit) {
        return false;
    }
    out.u = normalize(x);
    out.v = normalize(y);
    return true;
}

bool TouchTracker::release(TouchEvent& out) {
    if (!mDown) {
        return false;
    }
    mDown = false;
    out.action = 2;
    out.u = normalize(mX);
    out.v = normalize(mY);
    return true;
}

}  // namespace dusk::twinpad
