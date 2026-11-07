#include "dusk/twinpad/Channels/TwinpadPadMap.h"

#include <algorithm>

namespace dusk::twinpad {
namespace {

float stickAxis(int8_t value) {
    return static_cast<float>(value) / 127.0f;
}

float triggerAxis(uint8_t value) {
    return static_cast<float>(value) / 255.0f;
}

}  // namespace

bool PadState::active() const {
    return a || b || x || y || start || z || l || r || up || down || left || right ||
        stickX != 0.0f || stickY != 0.0f || substickX != 0.0f || substickY != 0.0f ||
        triggerLeft != 0.0f || triggerRight != 0.0f;
}

PadState mapInput(const InputReport& report) {
    PadState pad;
    pad.a = (report.buttons & kButtonA) != 0;
    pad.b = (report.buttons & kButtonB) != 0;
    pad.x = (report.buttons & kButtonX) != 0;
    pad.y = (report.buttons & kButtonY) != 0;
    pad.start = (report.buttons & kButtonStart) != 0;
    pad.z = (report.buttons & kButtonExtra1) != 0 || (report.buttons & kButtonExtra2) != 0;

    pad.l = (report.buttons & kButtonL1) != 0 || report.triggerL2 != 0;
    pad.r = (report.buttons & kButtonR1) != 0 || report.triggerR2 != 0;
    pad.triggerLeft = std::max((report.buttons & kButtonL1) != 0 ? 1.0f : 0.0f,
        triggerAxis(report.triggerL2));
    pad.triggerRight = std::max((report.buttons & kButtonR1) != 0 ? 1.0f : 0.0f,
        triggerAxis(report.triggerR2));

    // d-pad 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW; 8 = neutro (SPEC-004 §4).
    if (report.dpad <= 7) {
        pad.up = report.dpad == 0 || report.dpad == 1 || report.dpad == 7;
        pad.right = report.dpad == 1 || report.dpad == 2 || report.dpad == 3;
        pad.down = report.dpad == 3 || report.dpad == 4 || report.dpad == 5;
        pad.left = report.dpad == 5 || report.dpad == 6 || report.dpad == 7;
    }

    pad.stickX = stickAxis(report.stickLx);
    pad.stickY = -stickAxis(report.stickLy);
    pad.substickX = stickAxis(report.stickRx);
    pad.substickY = -stickAxis(report.stickRy);
    return pad;
}

}  // namespace dusk::twinpad
