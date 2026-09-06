package com.esp32talking.app

/**
 * IMA ADPCM 编解码 (M3)
 *
 * 320 样本(20ms @16kHz)PCM16 <-> 164 字节 ADPCM。
 * 帧头 4 字节:int16 预测样本 + uint8 步长索引 + uint8 保留,
 * 解码不依赖历史帧,丢一帧不影响后续音频。
 * 与 ESP32 固件使用同一套标准 IMA 表,跨端互通。
 */
object Adpcm {
    const val FRAME_SAMPLES = 320
    const val FRAME_BYTES = FRAME_SAMPLES * 2        // 640
    const val ADPCM_BYTES = 4 + FRAME_SAMPLES / 2    // 164

    private val STEP = shortArrayOf(
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767
    )
    private val INDEX = byteArrayOf(
        -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
    )

    /**
     * 编码 320 个 PCM 样本 -> 164 字节。
     * state[0]=跨帧预测样本, state[1]=跨帧步长索引(调用方保存)。
     */
    fun encode(pcm: ShortArray, state: IntArray, out: ByteArray) {
        var pred = state[0]
        var idx = state[1]
        for (i in 0 until FRAME_SAMPLES) {
            var diff = pcm[i] - pred
            val sign = if (diff < 0) 8 else 0
            if (diff < 0) diff = -diff
            val step = STEP[idx].toInt()
            var delta = 0
            if (diff >= step) { delta = delta or 4; diff -= step }
            if (diff >= (step shr 1)) { delta = delta or 2; diff -= step shr 1 }
            if (diff >= (step shr 2)) delta = delta or 1
            val nib = sign or delta
            var diffq = step shr 3
            if (delta and 4 != 0) diffq += step
            if (delta and 2 != 0) diffq += step shr 1
            if (delta and 1 != 0) diffq += step shr 2
            pred += if (sign != 0) -diffq else diffq
            pred = pred.coerceIn(-32768, 32767)
            idx = (idx + INDEX[nib].toInt()).coerceIn(0, 88)
            if (i % 2 == 0) out[4 + i / 2] = (nib shl 4).toByte()
            else out[4 + i / 2] = (out[4 + i / 2].toInt() or nib).toByte()
        }
        state[0] = pred
        state[1] = idx
    }

    /** 解码 164 字节 -> 320 个 PCM 样本(状态取自帧头) */
    fun decode(data: ByteArray, out: ShortArray) {
        var pred = (data[0].toInt() and 0xFF) or (data[1].toInt() shl 8)
        if (pred >= 32768) pred -= 65536
        var idx = data[2].toInt() and 0xFF
        if (idx > 88) idx = 88
        for (i in 0 until FRAME_SAMPLES) {
            val b = data[4 + i / 2].toInt() and 0xFF
            val nib = if (i % 2 == 0) b shr 4 else b and 0x0F
            val step = STEP[idx].toInt()
            var diffq = step shr 3
            if (nib and 4 != 0) diffq += step
            if (nib and 2 != 0) diffq += step shr 1
            if (nib and 1 != 0) diffq += step shr 2
            pred += if (nib and 8 != 0) -diffq else diffq
            pred = pred.coerceIn(-32768, 32767)
            idx = (idx + INDEX[nib].toInt()).coerceIn(0, 88)
            out[i] = pred.toShort()
        }
    }
}
