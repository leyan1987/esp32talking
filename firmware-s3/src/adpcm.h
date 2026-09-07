#pragma once
#include <Arduino.h>

// IMA ADPCM 编解码(与安卓端 Adpcm.kt、经典 ESP32 固件位格式完全一致)
// 帧格式:4 字节状态头(int16 预测样本 + uint8 步长索引 + uint8 保留)
//        + 每样本半字节(偶数样本在高 4 位) -> 320 样本 = 164 字节
// 状态随帧头携带,解码不依赖历史帧,丢一帧不影响后续音频。

namespace adpcm {

constexpr size_t SAMPLES_PER_FRAME = 320;              // 20ms @16kHz
constexpr size_t PCM_FRAME_BYTES = SAMPLES_PER_FRAME * 2;    // 640
constexpr size_t ADPCM_FRAME_BYTES = 4 + SAMPLES_PER_FRAME / 2;  // 164

static const uint16_t stepTab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t indexTab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

// 编码器(跨帧状态)
struct Encoder {
    int32_t pred = 0;
    int32_t idx = 0;

    size_t encode(const int16_t *pcm, uint8_t *out) {
        for (int i = 0; i < (int)SAMPLES_PER_FRAME; i++) {
            int32_t diff = pcm[i] - pred;
            int32_t sign = diff < 0 ? 8 : 0;
            uint32_t mag = diff < 0 ? -(int32_t)diff : diff;
            int32_t step = stepTab[idx];
            uint8_t delta = 0;
            if (mag >= (uint32_t)step) { delta |= 4; mag -= step; }
            if (mag >= (uint32_t)(step >> 1)) { delta |= 2; mag -= step >> 1; }
            if (mag >= (uint32_t)(step >> 2)) delta |= 1;
            uint8_t nib = (uint8_t)(sign | delta);
            int32_t diffq = step >> 3;
            if (delta & 4) diffq += step;
            if (delta & 2) diffq += step >> 1;
            if (delta & 1) diffq += step >> 2;
            pred += (sign ? -diffq : diffq);
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += indexTab[nib];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            if (i % 2 == 0) out[4 + i / 2] = (uint8_t)(nib << 4);
            else out[4 + i / 2] |= nib;
        }
        return ADPCM_FRAME_BYTES;
    }

    void reset() {
        pred = 0;
        idx = 0;
    }
};

// 解码(状态取自帧头,无跨帧依赖)
inline void decode(const uint8_t *in, int16_t *pcm) {
    int32_t pred = (int16_t)(in[0] | ((uint16_t)in[1] << 8));
    int32_t idx = in[2];
    if (idx > 88) idx = 88;
    for (int i = 0; i < (int)SAMPLES_PER_FRAME; i++) {
        uint8_t b = in[4 + i / 2];
        uint8_t nib = (i % 2 == 0) ? (b >> 4) : (b & 0x0F);
        int32_t step = stepTab[idx];
        int32_t diffq = step >> 3;
        if (nib & 4) diffq += step;
        if (nib & 2) diffq += step >> 1;
        if (nib & 1) diffq += step >> 2;
        pred += (nib & 8) ? -diffq : diffq;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        idx += indexTab[nib];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        pcm[i] = (int16_t)pred;
    }
}

}  // namespace adpcm
