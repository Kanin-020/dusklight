#pragma once

#include <cstdint>

// Punto de entrada del adaptador Twinpad desde el juego. El adaptador no
// conoce el companion: expone el gesto táctil ya normalizado y es
// `dualscreen.cpp` — que sí es dueño del panel — quien lo entrega (ver
// README.md).
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

// Siguiente gesto táctil del teléfono, en coordenadas normalizadas 0…1 sobre
// el panel. `action` usa la convención del companion: 0 = down, 1 = move,
// 2 = up. Devuelve como mucho un evento por frame (el panel consume uno solo).
bool takeTouch(float& u, float& v, int& action);

// True mientras el teléfono Twinpad está conectado al servidor local.
bool isConnected();

// Botones (máscara PADButton) que el teléfono mantiene pulsados en el pad
// virtual del puerto 1 en este frame. Lo usa la navegación de la pantalla de
// inicio, que no lee el pad del juego. 0 si no hay reporte vigente.
uint32_t heldButtons();

// Cierra el hilo de red. Llamado al salir del juego.
void shutdown();

}  // namespace dusk::twinpad
