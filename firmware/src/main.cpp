/**
 * esp32talking 固件 (M1 音频链路验证)
 *
 * 行为:按住 PTT 按键 -> 麦克风采集 PCM 并通过 WebSocket 上传;
 *      服务器回环把帧原样发回,松开按键后回放收到的音频。
 *      (同时播放会引起啸叫,所以只在松开后播放)
 *
 * LED 指示:常亮=就绪(服务器已连);慢闪=WiFi 已连但服务器未连;
 *           快闪=WiFi 未连;讲话时急闪。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_err.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <WebSocketsClient.h>

#include "config.h"

static WebSocketsClient ws;
static bool wifiOk = false;
static bool wsOk = false;
static uint32_t lastWsTry = 0;

// ---------- PTT 话权状态机 ----------
// M3:按键先向服务器申请话权,拿到授权后才能发音频(服务器会丢弃无权音频)
enum PttState : uint8_t { PTT_IDLE, PTT_REQUESTING, PTT_HOLDING };
static PttState pttState = PTT_IDLE;
static uint32_t pttRequestAt = 0;
#define PTT_REQUEST_TIMEOUT_MS 2500

// ---------- 播放环形缓冲 ----------
// 裸 PCM 32KB/s 下 64KB ≈ 2 秒;96KB 会使经典 ESP32 的 DRAM 溢出,
// M3 接入 ADPCM(8KB/s)后等效时长 ×4,届时可再加大
#define RING_SIZE (64 * 1024)

// 帧感知环形缓冲:每帧前有 2 字节小端长度前缀,支持 PCM(640)/ADPCM(164) 混存
static uint8_t ringBuf[RING_SIZE];
static size_t ringHead = 0;  // 写入位置
static size_t ringTail = 0;  // 读出位置
static uint32_t droppedFrames = 0;

static size_t ringUsed() {
    return (ringHead + RING_SIZE - ringTail) % RING_SIZE;
}

static void ringPush(const uint8_t *data, size_t len) {
    if (len == 0 || len > 1023) return;
    if (len + 2 > RING_SIZE - ringUsed()) {
        droppedFrames++;
        return;
    }
    ringBuf[ringHead] = (uint8_t)(len & 0xFF);
    ringHead = (ringHead + 1) % RING_SIZE;
    ringBuf[ringHead] = (uint8_t)((len >> 8) & 0xFF);
    ringHead = (ringHead + 1) % RING_SIZE;
    for (size_t i = 0; i < len; i++) {
        ringBuf[ringHead] = data[i];
        ringHead = (ringHead + 1) % RING_SIZE;
    }
}

// 取出一帧到 out(容量 cap),返回帧长;帧未收全返回 0
static size_t ringPop(uint8_t *out, size_t cap) {
    size_t used = ringUsed();
    if (used < 2) return 0;
    size_t len = ringBuf[ringTail] | ((size_t)ringBuf[(ringTail + 1) % RING_SIZE] << 8);
    if (len > cap || 2 + len > used) return 0;
    ringTail = (ringTail + 2) % RING_SIZE;
    for (size_t i = 0; i < len; i++) {
        out[i] = ringBuf[ringTail];
        ringTail = (ringTail + 1) % RING_SIZE;
    }
    return len;
}

static void ringClear() {
    ringTail = ringHead;
    if (droppedFrames > 0) {
        Serial.printf("[音频] 缓冲溢出丢弃 %u 帧\n", droppedFrames);
        droppedFrames = 0;
    }
}

// ---------- IMA ADPCM 编解码 (M3) ----------
// 每帧自带 4 字节状态头(int16 预测样本 + uint8 步长索引 + uint8 保留),
// 解码不依赖历史帧,丢一帧不影响后续音频
#define ADPCM_FRAME_BYTES (4 + FRAME_SAMPLES / 2)  // 320 样本 -> 164 字节

static const uint16_t stepTab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t indexTab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static int32_t adpcmPredictor = 0;  // 编码器跨帧状态
static int32_t adpcmIndex = 0;

#if USE_ADPCM
// 编码 320 样本 -> 164 字节(偶数样本在高 4 位)
static size_t adpcmEncode(const int16_t *pcm, uint8_t *out) {
    int32_t pred = adpcmPredictor;
    int32_t idx = adpcmIndex;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
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
    adpcmPredictor = pred;
    adpcmIndex = idx;
    return ADPCM_FRAME_BYTES;
}
#endif

// 解码 164 字节 -> 320 样本(状态取自帧头)
static void adpcmDecode(const uint8_t *in, int16_t *pcm) {
    int32_t pred = (int16_t)(in[0] | ((uint16_t)in[1] << 8));
    int32_t idx = in[2];
    if (idx > 88) idx = 88;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
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

// ---------- I2S 初始化 ----------
static void micInit() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = MIC_BCLK_PIN,
        .ws_io_num = MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = MIC_DATA_PIN,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
}

static void spkInit() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        // 缓冲欠载时自动补零,避免重复播放最后一帧产生杂音
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = SPK_BCLK_PIN,
        .ws_io_num = SPK_WS_PIN,
        .data_out_num = SPK_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_1, &pins));
}

// ---------- 采集 / 播放 ----------
static int32_t micRaw[FRAME_SAMPLES];
static int16_t pcmFrame[FRAME_SAMPLES];
static int16_t playPcm[FRAME_SAMPLES];
static uint8_t playBuf[FRAME_BYTES];
#if USE_ADPCM
static uint8_t adpcmFrame[ADPCM_FRAME_BYTES];
#endif

static inline int16_t sat16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// 阻塞读取一帧(20ms),返回 PCM 字节数
static size_t micReadFrame() {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, micRaw, sizeof(micRaw), &bytesRead, portMAX_DELAY);
    size_t n = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        // INMP441 输出 24 位数据左对齐在 32 位槽中,右移取高 16 位并施加增益
        pcmFrame[i] = sat16((int32_t)micRaw[i] >> MIC_GAIN_SHIFT);
    }
    return n * sizeof(int16_t);
}

static void speakerFeed() {
    size_t n = ringPop(playBuf, sizeof(playBuf));
    if (n == 0) return;
    if (n == ADPCM_FRAME_BYTES) {
        // ADPCM 帧 -> 解码为 PCM
        adpcmDecode(playBuf, playPcm);
        memcpy(playBuf, playPcm, FRAME_BYTES);
    } else if (n != FRAME_BYTES) {
        return;  // 未知帧长,丢弃
    }
    // n == FRAME_BYTES:裸 PCM(兼容旧客户端),直接写
    size_t written = 0;
    i2s_write(I2S_NUM_1, playBuf, FRAME_BYTES, &written, portMAX_DELAY);
}

// ---------- WebSocket ----------
static void onWsEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            wsOk = true;
            Serial.printf("[WS] 已连接: %s\n", (char *)payload);
            // hello:服务器按 MAC 识别设备;join_code 让服务器自动把本机拉进群组
            String hello = String("{\"type\":\"hello\",\"mac\":\"") +
                           WiFi.macAddress() + "\",\"proto\":1";
            if (strlen(JOIN_GROUP_CODE) > 0) {
                hello += String(",\"join_code\":\"") + JOIN_GROUP_CODE + "\"";
            }
            hello += "}";
            ws.sendTXT(hello);
            break;
        }
        case WStype_DISCONNECTED:
            if (wsOk) Serial.println("[WS] 连接断开");
            wsOk = false;
            pttState = PTT_IDLE;
            break;
        case WStype_TEXT:
            Serial.printf("[WS] 收到文本: %.*s\n", (int)len, (const char *)payload);
            // 话权消息(轻量匹配,避免引入 JSON 库)
            if (strstr((const char *)payload, "\"ptt_grant\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    ringClear();
                    pttState = PTT_HOLDING;
                    Serial.println("[PTT] 获得话权,开始讲话");
                } else {
                    // 迟到的授权:退还话权
                    ws.sendTXT("{\"type\":\"ptt_release\"}");
                }
            } else if (strstr((const char *)payload, "\"ptt_deny\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    pttState = PTT_IDLE;
                    Serial.println("[PTT] 他人正在讲话,未获话权");
                }
            } else if (strstr((const char *)payload, "\"ptt_revoke\"") != nullptr) {
                if (pttState == PTT_HOLDING) {
                    pttState = PTT_IDLE;
                    Serial.println("[PTT] 话权被更高优先级抢占");
                }
            }
            break;
        case WStype_BIN:
            ringPush(payload, len);
            break;
        default:
            break;
    }
}

// ---------- LED ----------
static void ledUpdate() {
    if (pttState == PTT_HOLDING) {
        digitalWrite(STATUS_LED_PIN, (millis() / 60) % 2);
        return;
    }
    if (wsOk) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        return;
    }
    uint32_t period = wifiOk ? 500 : 120;
    digitalWrite(STATUS_LED_PIN, (millis() % period) < period / 2);
}

// ---------- 主流程 ----------
void setup() {
    Serial.begin(115200);
    pinMode(PTT_BTN_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    micInit();
    spkInit();

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // 关闭省电模式,降低音频延迟抖动
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    ws.onEvent(onWsEvent);

    Serial.printf("[启动] MAC: %s, 固件: M1\n", WiFi.macAddress().c_str());
}

void loop() {
    ws.loop();
    ledUpdate();

    // WiFi 看门狗:断开后每 5 秒重连一次
    static uint32_t lastWifiTry = 0;
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiOk) {
            wifiOk = true;
            Serial.printf("[WiFi] 已连接, IP: %s\n", WiFi.localIP().toString().c_str());
            lastWsTry = 0;  // 立即尝试连接服务器
        }
    } else {
        wifiOk = false;
        if (millis() - lastWifiTry > 5000) {
            lastWifiTry = millis();
            WiFi.reconnect();
        }
    }

    // WebSocket 看门狗:断开后每 5 秒重连一次
    if (wifiOk && !wsOk && millis() - lastWsTry > 5000) {
        lastWsTry = millis();
        ws.disconnect();
        ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    }

    // PTT 按键(30ms 消抖)+ 话权状态机
    static bool lastPressed = false;
    static uint32_t lastChange = 0;
    bool pressed = digitalRead(PTT_BTN_PIN) == LOW;
    if (pressed != lastPressed && millis() - lastChange > 30) {
        lastPressed = pressed;
        lastChange = millis();
        if (pressed) {
            if (!wsOk) {
                Serial.println("[PTT] 服务器未连接");
            } else if (pttState == PTT_IDLE) {
                pttState = PTT_REQUESTING;
                pttRequestAt = millis();
                ws.sendTXT("{\"type\":\"ptt_request\"}");
                Serial.println("[PTT] 申请话权…");
            }
        } else {
            if (pttState != PTT_IDLE) {
                ws.sendTXT("{\"type\":\"ptt_release\"}");
                Serial.println("[PTT] 释放话权");
            }
            pttState = PTT_IDLE;
        }
    }

    // 申请超时:松开重按即可重试
    if (pttState == PTT_REQUESTING && millis() - pttRequestAt > PTT_REQUEST_TIMEOUT_MS) {
        pttState = PTT_IDLE;
        Serial.println("[PTT] 话权申请超时");
    }

    if (pttState == PTT_HOLDING) {
        size_t n = micReadFrame();  // -> pcmFrame,n = FRAME_BYTES
        if (wsOk && n > 0) {
#if USE_ADPCM
            size_t m = adpcmEncode(pcmFrame, adpcmFrame);
            ws.sendBIN(adpcmFrame, m);
#else
            ws.sendBIN((uint8_t *)pcmFrame, n);
#endif
        }
    } else {
        speakerFeed();
        delay(2);
    }
}
