#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Contrato de wire del adaptador Twinpad (SPEC-006 §4/§7, SPEC-004 §4,
// SPEC-003 §4 del repo Twinpad). Este archivo NO conoce Dusklight ni Aurora:
// solo std. Es la única pieza que sabe cómo se llaman los campos, para que el
// transporte, el cliente y los tests no los repitan por su cuenta.
//
// El servidor valida con `deny_unknown_fields` y rangos estrictos: cualquier
// desviación cierra el canal, así que el parseo de aquí es igual de estricto
// (campo desconocido, tipo equivocado o valor fuera de rango = línea inválida).

namespace dusk::twinpad {

// Puerto del canal primario del Servidor Twinpad en loopback (ADR-008). Lleva
// anuncio, frames y `input_report`; no hay QR ni selección de lado del juego.
// Sobreescribible al compilar para pruebas de host que no deben chocar con un
// servidor real en la máquina (misma idea que el hook E2E de Cemu): el default
// es siempre el del protocolo.
#ifndef DUSK_TWINPAD_IPC_PORT
#define DUSK_TWINPAD_IPC_PORT 8421
#endif
inline constexpr uint16_t kGameIpcPort = DUSK_TWINPAD_IPC_PORT;

// Identidad anunciada. `game_id` debe cumplir [a-z0-9_.-] y 1..64 chars.
inline constexpr std::string_view kGameId = "dusklight";
inline constexpr std::string_view kGameName = "Dusklight";

// D-pad neutro de SPEC-004 §4 (0..7 = dirección, 8 = neutro).
inline constexpr uint8_t kDpadNeutral = 8;

// Bits canónicos de `buttons` (SPEC-004 §2: bit 0 = button_a).
enum ButtonBit : uint16_t {
    kButtonA = 1u << 0,
    kButtonB = 1u << 1,
    kButtonX = 1u << 2,
    kButtonY = 1u << 3,
    kButtonL1 = 1u << 4,
    kButtonR1 = 1u << 5,
    kButtonL3 = 1u << 6,
    kButtonR3 = 1u << 7,
    kButtonStart = 1u << 8,
    kButtonSelect = 1u << 9,
    kButtonExtra1 = 1u << 10,
    kButtonExtra2 = 1u << 11,
};

// Último `input_report` aceptado. `motion` no se consume en v1 (el giroscopio
// del teléfono no tiene consumidor en este port todavía): se valida su forma y
// se marca `hasMotion`, sin retener muestras.
struct InputReport {
    uint32_t seq = 0;
    int8_t stickLx = 0;
    int8_t stickLy = 0;
    int8_t stickRx = 0;
    int8_t stickRy = 0;
    uint8_t dpad = kDpadNeutral;
    uint16_t buttons = 0;
    uint8_t triggerL2 = 0;
    uint8_t triggerR2 = 0;
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    bool touchPressed = false;
    bool hasMotion = false;
};

// Configuración de emisión vigente de la sesión (SPEC-003 §4, CR-027/CR-030/
// CR-074). `width`/`height` en 0/0 significan «dimensión nativa del panel»; el
// encoder escala a lo pedido antes de codificar. `bitrateKbps` ya viene
// clampeado al rango del contrato (1.000–30.000).
struct VideoConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    int bitrateKbps = 8000;
};

// Línea de anuncio (§7 paso 1). `second_screen` porque este adaptador publica
// el panel; `input_consumption` porque aplica el gamepad del teléfono.
std::string BuildAnnounceLine();

// Línea `game_frame` con una access unit H.264 Annex B completa. `annexBBase64Url`
// ya viene codificado. `ts_capture_us` es monotónico y `idr` marca las unidades
// que llevan SPS/PPS (SPEC-003 §4): el servidor solo relaya H.264.
std::string BuildFrameLine(uint64_t gfn, uint16_t width, uint16_t height,
    uint64_t timestampUs, bool idr, std::string_view annexBBase64Url);

// El servidor pide un keyframe al entrar un cliente nuevo (DES-004 §7.4). El
// adaptador solo reconoce esta línea además de `input_report`.
bool IsForceIdrLine(std::string_view line);

// base64url sin padding: el mismo alfabeto y semántica que el resto del wire
// (SPEC-006 §7), para que el servidor no distinga quién lo emite.
std::string Base64UrlEncode(const uint8_t* data, size_t size);
// Decodificación estricta: rechaza la cadena entera ante un carácter inválido
// o un tamaño 4n+1 (misma semántica que el transporte de Cemu).
bool DecodeBase64Url(std::string_view text, std::vector<uint8_t>& out);

// Acumulador NDJSON: TCP no conserva límites de mensaje. `next` entrega una
// línea por llamada, sin el `\n` (y sin el `\r` de un peer que lo añada).
class LineBuffer {
public:
    void append(const char* data, size_t size);
    bool next(std::string& line);
    void clear();

private:
    std::string m_buffer;
};

// Parseo estricto de una línea `input_report` (sin `\n`). Devuelve false ante
// cualquier desviación: `type` distinto, campo desconocido o faltante, valor no
// entero o fuera de rango.
bool ParseInputReport(std::string_view line, InputReport& out);

// Parseo de una línea `video_config` (sin `\n`), la que el Servidor reenvía del
// drawer de la app (SPEC-003 §4). `type` es obligatorio; `w`/`h` van juntos (o
// no van) y `0/0` es el centinela de dimensión nativa. Los campos desconocidos
// se saltan a propósito: el Servidor puede añadir metadata (`fps` de apps
// antiguas, sin efecto desde CR-029). Devuelve false si la línea no configura
// nada o si un campo conocido tiene forma inválida.
bool ParseVideoConfig(std::string_view line, VideoConfig& out);

}  // namespace dusk::twinpad
