# Adaptador Twinpad para Dusklight

Segunda pantalla + gamepad del teléfono sobre el **dual-screen que ya existe
en este fork**. El adaptador no reimplementa el dashboard ni el companion: lee
el panel que `dualscreen.cpp` ya presenta y le inyecta al juego el pad que llega
del teléfono.

Contrato de wire: repo Twinpad, `docs/specs/SPEC-006-plugin-sdk-detection.md`
(anuncio, frames, inputs), `SPEC-004 §4` (`input_report`) y `SPEC-003 §4`
(`game_frame`). Este módulo no cambia el protocolo: es un host más, igual que
Cemu.

## Por qué está separado

| Regla | Cómo se respeta |
|---|---|
| Sin acoplarse al companion | Cero `#include` de `companion*`. Dos enganches de juego: `twinpad::applyInput()` en `mDoCPd_c::read()` y `twinpad::tick()` en `dualscreen::beginHudCapture()`. |
| Sin acoplarse a Aurora fuera del borde | Solo `TwinpadHooks.cpp` incluye `<aurora/aux_window.hpp>` y `<dolphin/pad.h>`. `Core/`, `Client/`, `Video/` y `Channels/TwinpadPadMap` son std puro y se prueban en host. |
| Nunca bloquear el hilo de juego | `publishFrame()` solo deja el frame en una cola de 1 (drop-old). El H.264, el base64 y el socket viven en el hilo de red del cliente. |
| Una sola fuente del contrato | Todo nombre de campo y rango vive en `Core/TwinpadProtocol.h`; el transporte, el cliente y los tests lo usan, nadie lo repite. |

## Estructura

```text
src/dusk/twinpad/
├── Core/      TwinpadProtocol (wire estricto, base64url, NDJSON) · TwinpadSocket
├── Client/    TwinpadClient: máquina de conexión/reconexión y hilo de red
├── Video/     TwinpadH264: RGBA del panel → H.264 Annex B (OpenH264)
├── Channels/  TwinpadPadMap: input_report → pad GameCube (puro, testeable)
├── TwinpadHooks.{h,cpp}   borde con el juego: applyInput() + tick() + shutdown()
└── README.md
```

## Flujo

```textmDoCPd_c::read()                         (hilo de juego, 1/frame, ANTES de PADRead)
        └── twinpad::applyInput()
              └── Client::takeInput() ──► mapInput() ──► PADSetVirtualStatus(game.twinpadInputPort)

dualscreen::beginHudCapture()            (hilo de juego, 1/frame, al dibujar)
        └── twinpad::tick(hudEnabled)
              └── auxwin::request_capture()/take_capture() ──► Client::publishFrame(RGBA)

Client (hilo de red)
        └── game_announce → game_frame(fmt=h264, ts_capture_us, idr) → 127.0.0.1:8421
        └── input_report → último snapshot (orden por `seq`)
        └── video_force_idr → fuerza SPS/PPS + IDR en el siguiente frame
```

- La conexión viva **es** el anuncio (`game_id=dusklight`): si el juego cae, el
  servidor lo retira solo. Sin servidor se reintenta cada 2 s y se re-anuncia.
- `gfn` empieza en 0 en cada conexión (SPEC-003 §4), no por proceso.
- Al desconectarse se limpia el pad virtual y el último `input_report`, para no
  dejar un botón pulsado ni reanudar con una pulsación vieja.

## Activar

```bash
cmake -B build            # DUSK_TWINPAD=ON por defecto
ninja -C build dusklight
```

Por defecto está **on** y se conmuta en caliente en **Ajustes → Twinpad**
(`game.twinpadEnabled`): apagado, el adaptador no arranca el hilo de red ni
toca el pad. Requiere el `twinpad-server` del repo Twinpad corriendo en el PC
(`127.0.0.1:8421`), igual que Cemu, y `openh264-devel` para compilar (el mismo
`pkg-config` que usa el adaptador de Cemu). El panel solo se publica con el
dual-screen activo (`game.dualScreen`), porque la fuente es el panel que ese
modo presenta; el **mando** funciona con dual-screen encendido o apagado.

### Twinpad como dispositivo

Twinpad aparece como opción propia en **Ajustes → Controles → Device**. Al
conectar con el servidor se selecciona solo para el puerto 1
(`game.twinpadInputPort`), se suelta el mando SDL/teclado que lo ocupara y los
`input_report` se aplican únicamente a ese puerto. Al desconectar se libera el
pad virtual y se restaura el dispositivo previo. Elegir otro dispositivo en
«Device» devuelve el control al usuario; la selección manual se persiste.

El orden de inyección es funcional: `PADRead` solo fusiona el pad virtual
vigente en el instante de leer, y el táctil limpia `PAD_CHAN0` al empezar
`mDoCPd_c::read()`. Por eso `applyInput()` corre justo antes de
`JUTGamePad::read()`; aplicarlo durante el dibujo (como se hacía antes) dejaba
el mando del teléfono invisible para el juego.

## Tests

```bash
ninja -C build dusk_twinpad_tests && ./build/tests/dusk_twinpad_tests
```

Cubren base64url, NDJSON, el parseo estricto de `input_report` (campo
desconocido/rango/entero), el mapeo del pad y el encoder H.264 (Annex B,
SPS/PPS+IDR, timestamps y límites de tamaño). No requieren ISO, GPU ni
teléfono.

### Qué se verificó (y qué no)

| Comprobación | Estado |
|---|---|
| `Core/`, `Client/`, `Video/`, `Channels/` compilan con `g++ -std=c++20 -Wall -Wextra` | Sí |
| `TwinpadHooks.cpp` compila contra la ABI usada por el juego (`auxwin`, `dolphin/pad.h`), con y sin `DUSK_TWINPAD` | Sí (headers stub con las firmas reales, sin Aurora completo) |
| Ciclo completo contra un servidor falso: anuncio → `game_frame(gfn 0)` → `input_report` → descarte de `seq` repetido → limpieza al caer la conexión | Sí (driver de host, puerto alterno) |
| `dusk_twinpad_tests` con gtest | Sí: 18/18 en `build/linux-default-relwithdebinfo/tests/dusk_twinpad_tests` |
| Build completo de Dusklight con `DUSK_TWINPAD=ON` (Fedora, preset `linux-default-relwithdebinfo`) | Sí: `BUILD_EXIT=0`, binario `dusklight` enlazado |
| Mando real con Dusklight + app en el teléfono | No ejecutado aquí: requiere ISO del juego y un teléfono vinculado |

Trazabilidad: CR-091 (Approved), TEST-0185 (video y wire, verde en host) y
TEST-0186 (dispositivo, pendiente de la prueba en juego con ISO y teléfono).

## Límites conocidos (v1)

- **Mapeo del pad**: A/B/X/Y, START, L/R (digitales + gatillo analógico), d-pad,
  stick y C-stick, Z en los botones EXTRA_1/EXTRA_2. `SELECT` y `L3/R3` no tienen
  equivalente en el pad de GameCube y se ignoran.
- **Táctil del `input_report`**: sin uso. El panel se maneja con el táctil nativo
  del companion (`companion_touch.cpp`); llevarlo al teléfono remoto requiere
  decidir antes quién es dueño del táctil (dos consumidores, un panel).
- **Giroscopio**: se acepta y valida el campo `motion` pero no se consume;
  Dusklight no tiene consumidor de giro para el GameCube.
- **Pad virtual compartido**: los controles táctiles del juego y el adaptador
  usan el mismo punto de inyección (`PADSetVirtualStatus`). El adaptador solo
  escribe cuando Twinpad es el dispositivo elegido y tiene un reporte vivo, y
  suelta el pad una sola vez al perderlo; si el táctil y el teléfono apuntan al
  mismo puerto en el mismo frame, gana el último que escriba.
- **Sin audio y sin cámara/mic**: la integración es video del panel + input.
- No hay QR del lado del juego: el servidor debe estar corriendo.
