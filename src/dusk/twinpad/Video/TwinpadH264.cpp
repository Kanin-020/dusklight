#include "dusk/twinpad/Video/TwinpadH264.h"

#include <wels/codec_api.h>

#include <algorithm>
#include <chrono>

namespace dusk::twinpad {
namespace {

// Límites del panel en SPEC-003 §4: 16..3840 x 16..2160.
constexpr uint32_t kMinDimension = 16;
constexpr uint32_t kMaxWidth = 3840;
constexpr uint32_t kMaxHeight = 2160;

// 8 Mbps bastan para texto y mapas a 30 fps por loopback, y mantienen el frame
// muy por debajo del JPEG de calidad 80 que sustituyen.
constexpr int kBitrateKbps = 8000;
constexpr float kFrameRate = 30.0f;

// Keyframe de respaldo. El servidor pide uno al entrar un cliente
// (`video_force_idr`); esto cubre el caso de que esa línea se pierda.
constexpr int kIntraPeriod = 60;

// BT.601 de rango limitado, la conversión que espera un decodificador H.264
// corriente. Los paneles son opacos, así que el alfa del readback se descarta.
void rgbaToI420(const uint8_t* rgba, uint32_t width, uint32_t height,
    std::vector<uint8_t>& i420) {
    const uint32_t lumaSize = width * height;
    const uint32_t chromaSize = lumaSize / 4;
    i420.resize(lumaSize + chromaSize * 2);

    uint8_t* luma = i420.data();
    uint8_t* cb = luma + lumaSize;
    uint8_t* cr = cb + chromaSize;

    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* src = rgba + static_cast<size_t>(row) * width * 4;
        uint8_t* dst = luma + static_cast<size_t>(row) * width;
        for (uint32_t col = 0; col < width; ++col) {
            const int r = src[col * 4 + 0];
            const int g = src[col * 4 + 1];
            const int b = src[col * 4 + 2];
            dst[col] = static_cast<uint8_t>(std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
        }
    }

    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* src0 = rgba + static_cast<size_t>(row) * width * 4;
        const uint8_t* src1 = src0 + static_cast<size_t>(width) * 4;
        uint8_t* dstCb = cb + static_cast<size_t>(row / 2) * (width / 2);
        uint8_t* dstCr = cr + static_cast<size_t>(row / 2) * (width / 2);
        for (uint32_t col = 0; col < width; col += 2) {
            int r = 0;
            int g = 0;
            int b = 0;
            for (uint32_t dy = 0; dy < 2; ++dy) {
                const uint8_t* src = dy == 0 ? src0 : src1;
                for (uint32_t dx = 0; dx < 2; ++dx) {
                    const uint8_t* pixel = src + (col + dx) * 4;
                    r += pixel[0];
                    g += pixel[1];
                    b += pixel[2];
                }
            }
            // Suma de 4 píxeles sin dividir: el factor 4 se cancela al pasar de
            // 8 bits de precisión a la media del bloque 2x2.
            const int cbValue = ((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128;
            const int crValue = ((112 * r - 94 * g - 18 * b + 512) >> 10) + 128;
            dstCb[col / 2] = static_cast<uint8_t>(std::clamp(cbValue, 0, 255));
            dstCr[col / 2] = static_cast<uint8_t>(std::clamp(crValue, 0, 255));
        }
    }
}

uint8_t nalType(const uint8_t* data, size_t size) {
    size_t index = 0;
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
        index = 4;
    } else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
        index = 3;
    }
    return index < size ? static_cast<uint8_t>(data[index] & 0x1F) : 0;
}

}  // namespace

struct H264Encoder::Impl {
    ISVCEncoder* encoder = nullptr;
};

H264Encoder::~H264Encoder() {
    destroy();
}

void H264Encoder::destroy() {
    if (mImpl == nullptr) {
        return;
    }
    if (mImpl->encoder != nullptr) {
        mImpl->encoder->Uninitialize();
        WelsDestroySVCEncoder(mImpl->encoder);
        mImpl->encoder = nullptr;
    }
    delete mImpl;
    mImpl = nullptr;
    mWidth = 0;
    mHeight = 0;
}

bool H264Encoder::ensureEncoder(uint32_t width, uint32_t height) {
    if (mImpl != nullptr && mImpl->encoder != nullptr && width == mWidth && height == mHeight) {
        return true;
    }
    destroy();

    ISVCEncoder* encoder = nullptr;
    if (WelsCreateSVCEncoder(&encoder) != 0 || encoder == nullptr) {
        return false;
    }

    SEncParamExt params{};
    encoder->GetDefaultParams(&params);
    params.iUsageType = SCREEN_CONTENT_REAL_TIME;
    params.iPicWidth = static_cast<int>(width);
    params.iPicHeight = static_cast<int>(height);
    params.iTargetBitrate = kBitrateKbps * 1000;
    params.iRCMode = RC_BITRATE_MODE;
    params.fMaxFrameRate = kFrameRate;
    params.iTemporalLayerNum = 1;
    params.iSpatialLayerNum = 1;
    params.uiIntraPeriod = kIntraPeriod;
    params.eSpsPpsIdStrategy = CONSTANT_ID;
    params.bPrefixNalAddingCtrl = false;
    params.bEnableDenoise = false;
    // Igual que el adaptador de Cemu: el IDR se controla con kIntraPeriod y
    // `video_force_idr`, no con detección de cambio de escena (los paneles de UI
    // cambian a menudo y cada IDR cuesta varios frames de bitrate).
    params.bEnableSceneChangeDetect = false;
    params.bEnableFrameSkip = false;
    params.bEnableAdaptiveQuant = true;
    params.bEnableLongTermReference = false;
    params.iNumRefFrame = 1;
    // CAVLC es obligatorio en Baseline: CABAC solo existe en Main y superiores.
    params.iEntropyCodingModeFlag = 0;
    params.iComplexityMode = LOW_COMPLEXITY;

    SSpatialLayerConfig& layer = params.sSpatialLayers[0];
    layer.uiProfileIdc = PRO_BASELINE;
    layer.uiLevelIdc = LEVEL_4_2;
    layer.iVideoWidth = static_cast<int>(width);
    layer.iVideoHeight = static_cast<int>(height);
    layer.fFrameRate = kFrameRate;
    layer.iSpatialBitrate = kBitrateKbps * 1000;
    layer.iMaxSpatialBitrate = kBitrateKbps * 2000;
    layer.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
    layer.sSliceArgument.uiSliceNum = 1;

    if (encoder->InitializeExt(&params) != cmResultSuccess) {
        WelsDestroySVCEncoder(encoder);
        return false;
    }

    mImpl = new Impl{encoder};
    mWidth = width;
    mHeight = height;
    mEpochUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return true;
}

bool H264Encoder::encode(const uint8_t* rgba, uint32_t width, uint32_t height, bool forceIdr,
    std::vector<uint8_t>& annexB, uint64_t& timestampUs, bool& idr) {
    annexB.clear();
    timestampUs = 0;
    idr = false;

    if (rgba == nullptr) {
        return false;
    }
    // I420 necesita dimensiones pares; el panel puede llegar impar por un
    // recorte de la ventana, así que se descarta la última fila/columna.
    width &= ~1u;
    height &= ~1u;
    if (width < kMinDimension || height < kMinDimension || width > kMaxWidth ||
        height > kMaxHeight) {
        return false;
    }
    if (!ensureEncoder(width, height)) {
        return false;
    }

    if (forceIdr) {
        mImpl->encoder->ForceIntraFrame(true);
    }

    rgbaToI420(rgba, mWidth, mHeight, mI420);

    const uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    timestampUs = nowUs >= mEpochUs ? nowUs - mEpochUs : 0;

    SSourcePicture picture{};
    picture.iColorFormat = videoFormatI420;
    picture.iPicWidth = static_cast<int>(mWidth);
    picture.iPicHeight = static_cast<int>(mHeight);
    picture.iStride[0] = static_cast<int>(mWidth);
    picture.iStride[1] = static_cast<int>(mWidth / 2);
    picture.iStride[2] = static_cast<int>(mWidth / 2);
    picture.pData[0] = mI420.data();
    picture.pData[1] = mI420.data() + mWidth * mHeight;
    picture.pData[2] = mI420.data() + mWidth * mHeight + mWidth * mHeight / 4;
    picture.uiTimeStamp = timestampUs / 1000;

    SFrameBSInfo info{};
    if (mImpl->encoder->EncodeFrame(&picture, &info) != cmResultSuccess) {
        return false;
    }
    if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid) {
        return false;
    }

    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
    annexB.reserve(static_cast<size_t>(info.iFrameSizeInBytes));
    for (int layerIndex = 0; layerIndex < info.iLayerNum; ++layerIndex) {
        const SLayerBSInfo& layer = info.sLayerInfo[layerIndex];
        size_t offset = 0;
        for (int nalIndex = 0; nalIndex < layer.iNalCount; ++nalIndex) {
            const int nalSize = layer.pNalLengthInByte[nalIndex];
            if (nalSize <= 0) {
                continue;
            }
            const uint8_t* nal = layer.pBsBuf + offset;
            annexB.insert(annexB.end(), nal, nal + nalSize);
            switch (nalType(nal, static_cast<size_t>(nalSize))) {
            case 7: hasSps = true; break;
            case 8: hasPps = true; break;
            case 5: hasIdr = true; break;
            default: break;
            }
            offset += static_cast<size_t>(nalSize);
        }
    }

    if (annexB.empty()) {
        return false;
    }
    // Un IDR sin SPS/PPS delante no es decodificable por un cliente que acaba
    // de entrar, así que no se anuncia como tal.
    idr = hasIdr && hasSps && hasPps;
    return true;
}

}  // namespace dusk::twinpad
