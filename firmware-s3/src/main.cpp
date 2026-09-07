/**
 * esp32talking 固件 - LCDWIKI 2.8寸 ESP32-S3 模块适配版 (ES3C28P / ES3N28P)
 *
 * 与经典 ESP32 固件协议完全一致:
 *   按住 BOOT 键(GPIO0)申请话权 -> 授权后采集上传;松开释放。
 *   收到的音频帧(ADPCM 164字节 / PCM 640字节,按帧长自动识别)入环形缓冲播放。
 *   接收文字消息时屏幕显示提示(ES8311 通道对文字消息无操作)。
 *
 * 与经典固件的差异:本板音频走 ES8311 编解码芯片(I2C 初始化 + 单条 I2S),
 * 采用半双工驱动切换:空闲时 I2S 为 TX(播放),拿到话权后切为 RX(采集)。
 *
 * LED 指示(板载 RGB 灯为单线 RGB,此处用屏幕代替指示):
 *   屏幕显示 WiFi / 服务器 / 群组 / 话权 状态。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <WebSocketsClient.h>
#include <LovyanGFX.hpp>

#include "config.h"
#include "es8311.h"
#include "adpcm.h"

// ---------- 屏幕驱动(ILI9341V 240x320 SPI) ----------
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI _bus;

   public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = LCD_PIN_SCLK;
            cfg.pin_mosi = LCD_PIN_MOSI;
            cfg.pin_miso = LCD_PIN_MISO;
            cfg.pin_dc = LCD_PIN_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = LCD_PIN_CS;
            cfg.pin_rst = -1;    // 与芯片复位共用
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.invert = true;   // IPS 屏
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

static LGFX lcd;

// ---------- 状态 ----------
static WebSocketsClient ws;
static bool wifiOk = false;
static bool wsOk = false;
static uint32_t lastWsTry = 0;

// 话权状态机
enum PttState : uint8_t { PTT_IDLE, PTT_REQUESTING, PTT_HOLDING };
static PttState pttState = PTT_IDLE;
static uint32_t pttRequestAt = 0;
#define PTT_REQUEST_TIMEOUT_MS 2500

// 屏幕显示缓存
static String dispWifi = "WiFi connecting...";
static String dispServer = "Server: --";
static String dispGroup = "Group: --";
static String dispTalk = "Talk: idle";

// ---------- 播放环形缓冲(帧感知:2字节长度前缀) ----------
#define RING_SIZE (96 * 1024)
static uint8_t ringBuf[RING_SIZE];
static size_t ringHead = 0;
static size_t ringTail = 0;
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
        Serial.printf("[audio] buffer overflow, dropped %u frames\n", droppedFrames);
        droppedFrames = 0;
    }
}

// ---------- I2S(半双工模式切换) ----------
// 空闲 = TX(播放);持话权 = RX(采集)。ES8311 全程已就绪。
static int16_t playPcm[adpcm::SAMPLES_PER_FRAME];
static int16_t playStereo[adpcm::SAMPLES_PER_FRAME * 2];
static uint8_t playBuf[adpcm::PCM_FRAME_BYTES];
static int micSlot = -1;  // ES8311 采集槽位:0=左 1=右(进入采集时自动探测)
static int16_t pcmFrame[adpcm::SAMPLES_PER_FRAME];
#if USE_ADPCM
static uint8_t adpcmFrame[adpcm::ADPCM_FRAME_BYTES];
static adpcm::Encoder enc;
#endif

static void i2sInstallTx() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = SAMPLE_RATE * 256,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_MCLK_PIN,
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DO_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
    // 保证 ES8311 输入数据线有确定电平
    pinMode(I2S_DI_PIN, OUTPUT);
    digitalWrite(I2S_DI_PIN, LOW);
}

static void i2sInstallRx() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = SAMPLE_RATE * 256,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_16BIT,
    };
    i2s_pin_config_t pins = {
        .mck_io_num = I2S_MCLK_PIN,
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DI_PIN,
    };
    i2s_driver_uninstall(I2S_NUM_0);
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
    micSlot = -1;  // 重新探测有效声道
}

static void i2sStop() {
    i2s_driver_uninstall(I2S_NUM_0);
}

// ES8311 DAC 输入在 RX 期间无数据,静音 DAC 避免噪声;TX 恢复后解除
static void codecDacMute(bool m) {
    int regv = es8311::readReg(es8311::REG_DAC31);
    if (regv < 0) return;
    if (m) {
        es8311::writeReg(es8311::REG_DAC32, 0x00);
        es8311::writeReg(es8311::REG_SYS12, 0x02);
    } else {
        es8311::writeReg(es8311::REG_DAC31, regv & 0x9F);
        es8311::writeReg(es8311::REG_SYS12, 0x00);
        es8311::setVolume(CODEC_DAC_VOLUME);
    }
}

static void speakerFeed() {
    size_t n = ringPop(playBuf, sizeof(playBuf));
    if (n == 0) return;
    if (n == adpcm::ADPCM_FRAME_BYTES) {
        adpcm::decode(playBuf, playPcm);
        memcpy(playBuf, playPcm, adpcm::PCM_FRAME_BYTES);
    } else if (n != adpcm::PCM_FRAME_BYTES) {
        return;
    }
    // 立体声槽位:单声道样本复制到左右两槽,规避声道映射不确定性
    size_t frames = adpcm::SAMPLES_PER_FRAME;
    for (size_t i = frames; i > 0; i--) {
        playStereo[(i - 1) * 2] = playPcm[i - 1];
        playStereo[(i - 1) * 2 + 1] = playPcm[i - 1];
    }
    size_t written = 0;
    i2s_write(I2S_NUM_0, playStereo, frames * 2 * sizeof(int16_t), &written, portMAX_DELAY);
}

// 采集一帧(20ms,阻塞立体声帧) -> pcmFrame(单声道 PCM),返回 PCM 字节数。
// ES8311 ADC 数据可能在左或右槽位:首次进入采集时按能量自动探测。
static size_t micReadFrame() {
    static int16_t stereo[adpcm::SAMPLES_PER_FRAME * 2];
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, stereo, sizeof(stereo), &bytesRead, portMAX_DELAY);
    size_t n = bytesRead / (sizeof(int16_t) * 2);
    if (micSlot < 0) {
        // 前 16 帧统计左右槽能量,选大者为有效声道
        static double sumL = 0, sumR = 0;
        static int probeFrames = 0;
        for (size_t i = 0; i < n; i++) {
            double l = stereo[i * 2], r = stereo[i * 2 + 1];
            sumL += l * l;
            sumR += r * r;
        }
        probeFrames++;
        if (probeFrames >= 16) {
            micSlot = (sumR > sumL) ? 1 : 0;
            Serial.printf("[audio] mic slot = %s\n", micSlot == 0 ? "LEFT" : "RIGHT");
        }
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        pcmFrame[i] = stereo[i * 2 + micSlot];
    }
    return n * sizeof(int16_t);
}

// ---------- WebSocket ----------
static void onWsEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            wsOk = true;
            Serial.printf("[WS] connected: %s\n", (char *)payload);
            // hello:服务器按 MAC 识别;join_code 自动入群;proto 协议版本
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
            if (wsOk) Serial.println("[WS] disconnected");
            wsOk = false;
            pttState = PTT_IDLE;
            break;
        case WStype_TEXT:
            Serial.printf("[WS] text: %.*s\n", (int)len, (const char *)payload);
            if (strstr((const char *)payload, "\"ptt_grant\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    ringClear();
                    pttState = PTT_HOLDING;
                    Serial.println("[PTT] floor granted, talking");
                } else {
                    ws.sendTXT("{\"type\":\"ptt_release\"}");
                }
            } else if (strstr((const char *)payload, "\"ptt_deny\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    pttState = PTT_IDLE;
                    Serial.println("[PTT] floor denied, someone is talking");
                }
            } else if (strstr((const char *)payload, "\"ptt_revoke\"") != nullptr) {
                if (pttState == PTT_HOLDING) {
                    pttState = PTT_IDLE;
                    Serial.println("[PTT] floor preempted");
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

// ---------- 屏幕显示 ----------
static void displayRender() {
    lcd.fillRect(0, 0, 240, 320, TFT_BLACK);
    lcd.setTextDatum(lgfx::textdatum_t::top_left);
    lcd.setTextSize(1);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
    lcd.setCursor(4, 4);
    lcd.print("esp32talking");
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(4, 24);
    lcd.print(dispWifi);
    lcd.setCursor(4, 44);
    lcd.print(dispServer);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(4, 64);
    lcd.print(dispGroup);
    lcd.setTextColor(dispTalk.startsWith("Talk: idle") ? TFT_WHITE : TFT_ORANGE, TFT_BLACK);
    lcd.setCursor(4, 84);
    lcd.print(dispTalk);
}

static void dispSet(String &slot, const String &v) {
    slot = v;
    displayRender();
}

// ---------- 主流程 ----------
void setup() {
    Serial.begin(115200);
    pinMode(BOOT_KEY_PIN, INPUT_PULLUP);
    pinMode(PA_EN_PIN, OUTPUT);
    digitalWrite(PA_EN_PIN, LOW);  // 低电平使能功放

    // 初始化屏幕
    lcd.init();
    lcd.setRotation(0);
    pinMode(LCD_PIN_BL, OUTPUT);
    digitalWrite(LCD_PIN_BL, HIGH);
    displayRender();

    // 初始化 ES8311(I2C 地址 0x18;18dB 采集增益对应寄存器值 0x04)
    Wire.begin(CODEC_I2C_SDA, CODEC_I2C_SCL);
    Wire.setClock(100000);
    if (es8311::init(SAMPLE_RATE, CODEC_MIC_GAIN_REG, CODEC_DAC_VOLUME)) {
        Serial.println("[codec] ES8311 ready");
    } else {
        Serial.println("[codec] ES8311 init FAILED (check I2C wiring)");
    }

    i2sInstallTx();

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    ws.onEvent(onWsEvent);

    Serial.printf("[boot] MAC: %s, firmware: S3-ES3 v1.0\n", WiFi.macAddress().c_str());
}

void loop() {
    ws.loop();

    // WiFi 看门狗
    static uint32_t lastWifiTry = 0;
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiOk) {
            wifiOk = true;
            String ip = "WiFi: " + WiFi.localIP().toString();
            dispSet(dispWifi, ip);
            lastWsTry = 0;
        }
    } else {
        wifiOk = false;
        if (millis() - lastWifiTry > 5000) {
            lastWifiTry = millis();
            WiFi.reconnect();
        }
    }

    // WebSocket 看门狗
    if (wifiOk && !wsOk && millis() - lastWsTry > 5000) {
        lastWsTry = millis();
        ws.disconnect();
        ws.begin(SERVER_HOST, SERVER_PORT, SERVER_PATH);
    }

    // PTT 按键(BOOT 键,30ms 消抖)+ 话权状态机
    static bool lastPressed = false;
    static uint32_t lastChange = 0;
    bool pressed = digitalRead(BOOT_KEY_PIN) == LOW;
    if (pressed != lastPressed && millis() - lastChange > 30) {
        lastPressed = pressed;
        lastChange = millis();
        if (pressed) {
            if (!wsOk) {
                Serial.println("[PTT] server not connected");
            } else if (pttState == PTT_IDLE) {
                pttState = PTT_REQUESTING;
                pttRequestAt = millis();
                ws.sendTXT("{\"type\":\"ptt_request\"}");
                Serial.println("[PTT] requesting floor...");
            }
        } else {
            if (pttState != PTT_IDLE) {
                ws.sendTXT("{\"type\":\"ptt_release\"}");
                Serial.println("[PTT] floor released");
            }
            pttState = PTT_IDLE;
        }
    }

    // 申请超时
    if (pttState == PTT_REQUESTING && millis() - pttRequestAt > PTT_REQUEST_TIMEOUT_MS) {
        pttState = PTT_IDLE;
        Serial.println("[PTT] floor request timeout");
    }

    // 话权状态切换时切换 I2S 方向与屏幕提示
    static PttState lastPttShown = PTT_IDLE;
    static bool audioModeRx = false;  // false=TX(播放) true=RX(采集)
    if (pttState == PTT_HOLDING && !audioModeRx) {
        i2sStop();
        i2sInstallRx();
        codecDacMute(true);
        audioModeRx = true;
        Serial.println("[audio] switch to capture");
    } else if (pttState != PTT_HOLDING && audioModeRx) {
        i2sStop();
        i2sInstallTx();
        codecDacMute(false);
        audioModeRx = false;
        Serial.println("[audio] switch to playback");
    }
    if (lastPttShown != pttState) {
        lastPttShown = pttState;
        if (pttState == PTT_IDLE) dispSet(dispTalk, "Talk: idle");
        else if (pttState == PTT_REQUESTING) dispSet(dispTalk, "Talk: requesting");
        else dispSet(dispTalk, "Talk: TALKING");
    }

    if (pttState == PTT_HOLDING) {
        size_t n = micReadFrame();
        if (wsOk && n > 0) {
#if USE_ADPCM
            size_t m = enc.encode(pcmFrame, adpcmFrame);
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
