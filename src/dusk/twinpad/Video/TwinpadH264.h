#pragma once

#include <cstdint>
#include <vector>

// Codificador H.264 del panel. Aísla OpenH264 del resto del adaptador: este es
// el único archivo que incluye `wels/codec_api.h`, igual que el de Cemu, para
// que el cliente, el protocolo y los tests de host sigan siendo std-only.
//
// Contrato (SPEC-003 §4 del repo Twinpad): Annex B, perfil Baseline, nivel 4.2
// como máximo, sin B-frames, y cada IDR lleva SPS y PPS delante.
namespace dusk::twinpad {

class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // RGBA del readback del panel → access unit Annex B. `forceIdr` pide un
    // keyframe en este frame (primer frame, reconexión o `video_force_idr`).
    // `timestampUs` es monotónico desde la creación del codificador y `idr`
    // indica si la unidad es IDR con SPS/PPS. Devuelve false si no hay frame
    // codificable (tamaño fuera de rango, encoder sin inicializar o frame
    // saltado); en ese caso `annexB` queda vacío.
    bool encode(const uint8_t* rgba, uint32_t width, uint32_t height, bool forceIdr,
        std::vector<uint8_t>& annexB, uint64_t& timestampUs, bool& idr);

    // Dimensiones efectivas del último encoder inicializado (siempre pares).
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }

private:
    struct Impl;

    bool ensureEncoder(uint32_t width, uint32_t height);
    void destroy();

    Impl* mImpl = nullptr;
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint64_t mEpochUs = 0;
    std::vector<uint8_t> mI420;
};

}  // namespace dusk::twinpad
