#pragma once

#include "dusk/twinpad/Core/TwinpadProtocol.h"

#include <cstdint>
#include <vector>

// Codificador H.264 del panel. Aísla OpenH264 del resto del adaptador: este es
// el único archivo que incluye `wels/codec_api.h`, igual que el de Cemu, para
// que el cliente, el protocolo y los tests de host sigan siendo std-only.
//
// Contrato (SPEC-003 §4 del repo Twinpad): Annex B, perfil Baseline, nivel 4.2
// como máximo, sin B-frames, y cada IDR lleva SPS y PPS delante.
namespace dusk::twinpad {

// La configuración de emisión es `VideoConfig` (Core/TwinpadProtocol.h): la
// misma estructura que parsea `video_config`, para que no haya dos tipos con
// los mismos campos que se puedan desincronizar.
//
// Escalado nearest-neighbor de un frame RGBA. Es el mismo criterio que aplica
// el adaptador de Cemu (SPEC-003 §3/CR-074): sin filtro ni mezcla, y admite
// tanto reducir como ampliar. `out` queda con dstWidth*dstHeight*4 bytes.
void scaleRgbaNearest(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight,
    uint32_t dstWidth, uint32_t dstHeight, std::vector<uint8_t>& out);

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // RGBA del readback del panel → access unit Annex B. `forceIdr` pide un
    // keyframe en este frame (primer frame, reconexión, `video_force_idr` o
    // cambio de configuración). `timestampUs` es monotónico desde la creación
    // del codificador y `idr` indica si la unidad es IDR con SPS/PPS. Devuelve
    // false si no hay frame codificable (tamaño fuera de rango, encoder sin
    // inicializar o frame saltado); en ese caso `annexB` queda vacío.
    bool encode(const uint8_t* rgba, uint32_t width, uint32_t height,
        const VideoConfig& config, bool forceIdr, std::vector<uint8_t>& annexB,
        uint64_t& timestampUs, bool& idr);

    // Dimensiones efectivas del último encoder inicializado (siempre pares).
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }

private:
    struct Impl;

    bool ensureEncoder(uint32_t width, uint32_t height, int bitrateKbps);
    void destroy();

    Impl* mImpl = nullptr;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    int mBitrateKbps = 0;
    uint64_t mEpochUs = 0;
    std::vector<uint8_t> mScaled;
    std::vector<uint8_t> mI420;
};

}  // namespace dusk::twinpad
