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
static bool talking = false;
static uint32_t lastWsTry = 0;

// ---------- 播放环形缓冲(容量 64KB ≈ 4 秒) ----------
#define RING_SIZE (64 * 1024)
static uint8_t ringBuf[RING_SIZE];
static size_t ringHead = 0;  // 写入位置
static size_t ringTail = 0;  // 读出位置
static uint32_t droppedFrames = 0;

static size_t ringUsed() {
    return (ringHead + RING_SIZE - ringTail) % RING_SIZE;
}

static void ringPush(const uint8_t *data, size_t len) {
    if (len > RING_SIZE - ringUsed()) {
        droppedFrames++;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        ringBuf[ringHead] = data[i];
        ringHead = (ringHead + 1) % RING_SIZE;
    }
}

static size_t ringPop(uint8_t *out, size_t len) {
    size_t n = min(len, ringUsed());
    for (size_t i = 0; i < n; i++) {
        out[i] = ringBuf[ringTail];
        ringTail = (ringTail + 1) % RING_SIZE;
    }
    return n;
}

static void ringClear() {
    ringTail = ringHead;
    if (droppedFrames > 0) {
        Serial.printf("[音频] 缓冲溢出丢弃 %u 帧\n", droppedFrames);
        droppedFrames = 0;
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
static uint8_t playBuf[FRAME_BYTES];

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
    size_t n = ringPop(playBuf, FRAME_BYTES);
    if (n == 0) return;
    size_t written = 0;
    i2s_write(I2S_NUM_1, playBuf, n, &written, portMAX_DELAY);
}

// ---------- WebSocket ----------
static void onWsEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            wsOk = true;
            Serial.printf("[WS] 已连接: %s\n", (char *)payload);
            String hello = String("{\"type\":\"hello\",\"mac\":\"") +
                           WiFi.macAddress() + "\",\"proto\":1}";
            ws.sendTXT(hello);
            break;
        }
        case WStype_DISCONNECTED:
            if (wsOk) Serial.println("[WS] 连接断开");
            wsOk = false;
            talking = false;
            break;
        case WStype_TEXT:
            Serial.printf("[WS] 收到文本: %.*s\n", (int)len, (const char *)payload);
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
    if (talking) {
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
    WiFi.autoReconnect(true);

    ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    ws.onEvent(onWsEvent);

    Serial.printf("[启动] MAC: %s, 固件: M1\n", WiFi.macAddress().c_str());
}

void loop() {
    ws.loop();
    ledUpdate();

    // WiFi 看门狗
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiOk) {
            wifiOk = true;
            Serial.printf("[WiFi] 已连接, IP: %s\n", WiFi.localIP().toString().c_str());
            lastWsTry = 0;  // 立即尝试连接服务器
        }
    } else {
        wifiOk = false;
    }

    // WebSocket 看门狗:断开后每 5 秒重连一次
    if (wifiOk && !wsOk && millis() - lastWsTry > 5000) {
        lastWsTry = millis();
        ws.disconnect();
        ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    }

    // PTT 按键(30ms 消抖)
    static bool lastPressed = false;
    static uint32_t lastChange = 0;
    bool pressed = digitalRead(PTT_BTN_PIN) == LOW;
    if (pressed != lastPressed && millis() - lastChange > 30) {
        lastPressed = pressed;
        lastChange = millis();
        if (pressed && wsOk) {
            ringClear();
            talking = true;
            Serial.println("[PTT] 开始讲话");
        } else if (!pressed) {
            talking = false;
            Serial.println("[PTT] 停止,开始回放");
        } else {
            Serial.println("[PTT] 服务器未连接");
        }
    }

    if (talking) {
        size_t n = micReadFrame();
        if (wsOk && n > 0) ws.sendBIN((uint8_t *)pcmFrame, n);
    } else {
        speakerFeed();
        delay(2);
    }
}
