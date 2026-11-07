#pragma once

// Punto de entrada del adaptador Twinpad desde el juego. Tres funciones, dos
// sitios de llamada, a propósito: el adaptador no conoce el companion, solo la
// superficie que ese archivo ya presenta y el pad virtual que ya usa el táctil
// (ver README.md).
namespace dusk::twinpad {

// Antes de JUTGamePad::read() (m_Do_controller_pad.cpp): aplica el último
// reporte del teléfono al pad virtual. El ORDEN es funcional: el táctil limpia
// PAD_CHAN0 al principio de mDoCPd_c::read(), y PADRead solo ve el pad virtual
// vigente en ese instante — aplicarlo al dibujar lo dejaba invisible al juego.
void applyInput();

// Una vez por frame, en el hilo de juego. `hudEnabled` es "el segundo panel se
// está presentando este frame" (dualscreen::isEnabled): sin panel no hay nada
// que publicar y el adaptador deja de capturar. También es donde se arranca o
// se detiene el cliente según el ajuste «Twinpad».
void tick(bool hudEnabled);

// Cierra el hilo de red. Llamado al salir del juego.
void shutdown();

}  // namespace dusk::twinpad
