/**
 * esp32talking 固件 - LCDWIKI 2.8寸 ESP32-S3 模块适配版 (ES3C28P / ES3N28P)
 *
 * 与经典 ESP32 固件协议完全一致:
 *   申请话权(ptt_request)-> 授权后采集上传;松开释放。
 *   收到的音频帧(ADPCM 164字节 / PCM 640字节,按帧长自动识别)入环形缓冲播放。
 *   接收文字消息时屏幕显示提示(ES8311 通道对文字消息无操作)。
 *
 * 与经典固件的差异:本板音频走 ES8311 编解码芯片(I2C 初始化 + 单条 I2S),
 * 采用半双工驱动切换:空闲时 I2S 为 TX(播放),拿到话权后切为 RX(采集)。
 *
 * v1.1 新增触摸界面:
 *   - 电容触摸 FT5x06,输入 6 位"对讲机组号"加入群组(数字键盘 + JOIN + DEL)
 *   - 大号 "HOLD TO TALK" 触摸按钮发送语音(与板载 BOOT 键等效,二者皆可触发话权)
 *   注: LovyanGFX 自带字体不含中文字形,故标签用 ASCII 以保证必然可显示;
 *       如需中文,把 LBL_* 常量换成中文并挂载 CJK 字体即可(见文末说明)。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <WebSocketsClient.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/touch/Touch_FT5x06.hpp>

#include "config.h"
#include "config_store.h"
#include "provisioning.h"
#include "es8311.h"
#include "adpcm.h"

// 触摸引脚: 兼容两种命名约定,以便直接套用你手上的 config.h。
//   - 你实际用的 config.h:  TP_RST_PIN / TP_INT_PIN / TP_I2C_ADDR / TP_SDA / TP_SCL
//   - 早期示例 config.h.example: TP_RST / TP_INT(此处做向下兼容)
// 触摸屏与 ES8311 编解码共用同一条 I2C 总线(CODEC_I2C_SDA / CODEC_I2C_SCL)。
#ifndef TOUCH_ENABLED
#define TOUCH_ENABLED 1
#endif
#ifndef TP_RST
  #ifdef TP_RST_PIN
    #define TP_RST TP_RST_PIN
  #else
    #define TP_RST 18
  #endif
#endif
#ifndef TP_INT
  #ifdef TP_INT_PIN
    #define TP_INT TP_INT_PIN
  #else
    #define TP_INT 17
  #endif
#endif
#ifndef TP_SDA
  #define TP_SDA CODEC_I2C_SDA   // 复用 ES8311 的 I2C SDA
#endif
#ifndef TP_SCL
  #define TP_SCL CODEC_I2C_SCL   // 复用 ES8311 的 I2C SCL
#endif
#ifndef TP_ADDR
  #ifdef TP_I2C_ADDR
    #define TP_ADDR TP_I2C_ADDR
  #else
    #define TP_ADDR 0x38
  #endif
#endif

// LovyanGFX 颜色类型为 uint32_t (0xRRGGBB),与 TFT_* 常量一致;不要用 color565(返回 uint16_t)
#define UI_RGB(r, g, b) ((uint32_t)(((uint8_t)(r) << 16) | ((uint8_t)(g) << 8) | (uint8_t)(b)))

// ---------- 屏幕驱动(ILI9341V 240x320 SPI) + 触摸 ----------
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9341 _panel;
    lgfx::Bus_SPI _bus;
#if TOUCH_ENABLED
    lgfx::Touch_FT5x06 _touch;
#endif

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
#if TOUCH_ENABLED
        {
            auto cfg = _touch.config();
            cfg.x_min = 0;
            cfg.x_max = 239;
            cfg.y_min = 0;
            cfg.y_max = 319;
            cfg.pin_sda = TP_SDA;
            cfg.pin_scl = TP_SCL;
            cfg.pin_rst = TP_RST;
            cfg.pin_int = TP_INT;
            cfg.bus_shared = true;  // 与 ES8311 共用 I2C(Wire),不重复初始化总线
            cfg.i2c_port = (int8_t)0;
            cfg.i2c_addr = TP_ADDR; // FT5x06 默认地址(0x38)
            cfg.offset_rotation = 0;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
#endif
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
static String curGroupCode = "";   // 当前(优化显示的)组号
static String dispMsg = "";        // 临时提示(如"群号不存在")
static uint32_t msgUntil = 0;      // 提示有效截止时间

// ---------- UI 布局(竖屏 240x320) ----------
#define STATUS_H   52
#define CELL_W     34
#define CELL_H     36
#define CELL_GAP    4
#define CELL_X0     8
#define CELL_Y     72
#define KP_X0      10
#define KP_W       68
#define KP_H       30
#define KP_GAP      6
#define KP_GAPY     6
#define KP_Y0     116
#define PTT_X      10
#define PTT_W     164
#define PTT_Y     262
#define PTT_H      52
#define SETUP_X   180
#define SETUP_W    50
#define SETUP_H    52

// 可编辑标签(ASCII 以保证 LovyanGFX 默认字体可显示)
static const char *LBL_TITLE   = "esp32talking";
static const char *LBL_CAPTION = "GROUP CODE (6 digits)";
static const char *LBL_JOIN    = "JOIN";
static const char *LBL_DEL     = "DEL";
static const char *LBL_SETUP   = "SETUP";
static const char *LBL_PTT_IDLE = "HOLD TO TALK (PTT)";
static const char *LBL_PTT_REQ  = "REQUESTING...";
static const char *LBL_PTT_TALK = "TALKING...";

// 重置确认界面
static bool askSetup = false;

// 键盘布局(4 行 x 3 列)
static const char *KEYS[4][3] = {
    {"1", "2", "3"},
    {"4", "5", "6"},
    {"7", "8", "9"},
    {LBL_JOIN, "0", LBL_DEL},
};

// UI 运行时状态
static char codeBuf[7] = "";     // 已输入的组号(最多 6 位 + \0)
static int codeLen = 0;
static bool uiNeedRedraw = true;
static bool pttTouchHeld = false;  // 触摸 PTT 区域是否正被按住

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

// ---------- UI 辅助 ----------
static bool pointInRect(int x, int y, int w, int h, int px, int py) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void uiDrawBtn(int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg) {
    lcd.fillRoundRect(x, y, w, h, 6, bg);
    lcd.setTextDatum(lgfx::textdatum_t::middle_center);
    lcd.setFont(&fonts::Font2);
    lcd.setTextColor(fg, bg);
    lcd.drawString(label, x + w / 2, y + h / 2);
    lcd.setTextDatum(lgfx::textdatum_t::top_left);
}

static void setMsg(const String &m) {
    dispMsg = m;
    msgUntil = millis() + 3000;
    uiNeedRedraw = true;
}

// 完整重绘界面
static void uiRender() {
    lcd.fillScreen(TFT_BLACK);

    // 重置确认页
    if (askSetup) {
        lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
        lcd.setTextDatum(lgfx::textdatum_t::middle_center);
        lcd.setFont(&fonts::Font4);
        lcd.drawString("RESET CONFIG?", 120, 80);
        lcd.setFont(&fonts::Font2);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.drawString("Clear WiFi / Server / Group", 120, 120);
        lcd.drawString("and enter SETUP MODE?", 120, 140);
        uiDrawBtn(14, 200, 100, 60, "YES", UI_RGB(180, 40, 40), TFT_WHITE);
        uiDrawBtn(126, 200, 100, 60, "NO", UI_RGB(34, 51, 102), TFT_WHITE);
        lcd.setTextDatum(lgfx::textdatum_t::top_left);
        return;
    }

    // 顶部状态栏
    lcd.fillRect(0, 0, 240, STATUS_H, UI_RGB(12, 24, 40));
    lcd.setTextColor(TFT_GREENYELLOW, UI_RGB(12, 24, 40));
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(lgfx::textdatum_t::top_left);
    lcd.setCursor(4, 4);
    lcd.print(LBL_TITLE);
    lcd.setTextColor(TFT_WHITE, UI_RGB(12, 24, 40));
    lcd.setCursor(4, 24);
    lcd.print(dispWifi);

    // 状态行: 连接状态 + 当前组号, 临时提示优先
    String statusLine;
    if (millis() < msgUntil) {
        statusLine = dispMsg;
    } else if (!wifiOk) {
        statusLine = "WiFi connecting...";
    } else if (!wsOk) {
        statusLine = "Server connecting...";
    } else {
        statusLine = "Server: connected";
        if (curGroupCode.length()) statusLine += "  Grp:" + curGroupCode;
    }
    lcd.setCursor(4, 42);
    lcd.print(statusLine);

    // 组号标题
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.setCursor(8, 56);
    lcd.print(LBL_CAPTION);

    // 6 个组号输入框
    for (int i = 0; i < 6; i++) {
        int x = CELL_X0 + i * (CELL_W + CELL_GAP);
        lcd.fillRoundRect(x, CELL_Y, CELL_W, CELL_H, 4, UI_RGB(34, 51, 68));
        lcd.drawRect(x, CELL_Y, CELL_W, CELL_H, TFT_CYAN);
        char c = (i < codeLen) ? codeBuf[i] : 0;
        if (c) {
            lcd.setTextDatum(lgfx::textdatum_t::middle_center);
            lcd.setTextColor(TFT_WHITE, UI_RGB(34, 51, 68));
            lcd.setFont(&fonts::Font4);
            char d[2] = {c, 0};
            lcd.drawString(d, x + CELL_W / 2, CELL_Y + CELL_H / 2);
            lcd.setTextDatum(lgfx::textdatum_t::top_left);
        }
    }

    // 数字键盘
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = KP_X0 + c * (KP_W + KP_GAP);
            int by = KP_Y0 + r * (KP_H + KP_GAPY);
            const char *k = KEYS[r][c];
            uint32_t bg = UI_RGB(34, 51, 102);
            if (strcmp(k, LBL_JOIN) == 0) bg = UI_RGB(30, 122, 30);
            else if (strcmp(k, LBL_DEL) == 0) bg = UI_RGB(122, 30, 30);
            uiDrawBtn(bx, by, KP_W, KP_H, k, bg, TFT_WHITE);
        }
    }

    // PTT 发送语音按钮(颜色随话权状态变化)
    const char *pttLabel = (pttState == PTT_HOLDING) ? LBL_PTT_TALK
                           : (pttState == PTT_REQUESTING) ? LBL_PTT_REQ
                                                          : LBL_PTT_IDLE;
    uint32_t pttBg = (pttState == PTT_HOLDING)     ? UI_RGB(20, 180, 20)
                     : (pttState == PTT_REQUESTING) ? UI_RGB(180, 180, 30)
                                                    : UI_RGB(40, 40, 140);
    uiDrawBtn(PTT_X, PTT_Y, PTT_W, PTT_H, pttLabel, pttBg, TFT_WHITE);

    // SETUP 按钮(重置配置进配网)
    uiDrawBtn(SETUP_X, PTT_Y, SETUP_W, SETUP_H, LBL_SETUP, UI_RGB(120, 60, 10), TFT_WHITE);
}

// 点击(按下沿)处理: 输入数字 / 删除 / 加入组
static void tryJoinGroup();  // 前置声明(onTap 中调用)

static void onTap(int x, int y) {
    // 重置确认页
    if (askSetup) {
        if (pointInRect(14, 200, 100, 60, x, y)) {
            Serial.println("[UI] config reset requested");
            configClear();
            lcd.fillScreen(TFT_BLACK);
            lcd.setTextColor(TFT_WHITE, TFT_BLACK);
            lcd.setTextDatum(lgfx::textdatum_t::middle_center);
            lcd.setFont(&fonts::Font4);
            lcd.drawString("Rebooting...", 120, 160);
            delay(800);
            ESP.restart();
        }
        if (pointInRect(126, 200, 100, 60, x, y)) {
            askSetup = false;
            uiNeedRedraw = true;
        }
        return;
    }
    // SETUP 按钮(仅空闲时可用,避免打断讲话)
    if (pointInRect(SETUP_X, PTT_Y, SETUP_W, SETUP_H, x, y)) {
        if (pttState == PTT_IDLE) {
            askSetup = true;
            uiNeedRedraw = true;
        }
        return;
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 3; c++) {
            int bx = KP_X0 + c * (KP_W + KP_GAP);
            int by = KP_Y0 + r * (KP_H + KP_GAPY);
            if (pointInRect(bx, by, KP_W, KP_H, x, y)) {
                const char *k = KEYS[r][c];
                if (k[0] >= '0' && k[0] <= '9') {
                    if (codeLen < 6) {
                        codeBuf[codeLen++] = k[0];
                        codeBuf[codeLen] = 0;
                        uiNeedRedraw = true;
                    }
                } else if (strcmp(k, LBL_DEL) == 0) {
                    if (codeLen > 0) {
                        codeLen--;
                        codeBuf[codeLen] = 0;
                        uiNeedRedraw = true;
                    }
                } else if (strcmp(k, LBL_JOIN) == 0) {
                    tryJoinGroup();
                }
                return;
            }
        }
    }
}

// 发送加入群组: join_group(凭 6 位群号) + select_group(切为当前群组)
static void tryJoinGroup() {
    if (codeLen < 6) {
        setMsg("Need 6 digits");
        return;
    }
    String code = String(codeBuf);
    configSetGroupCode(code);  // 持久化:重启后 hello 自动 join_code,无需重输
    ws.sendTXT("{\"type\":\"join_group\",\"code\":\"" + code + "\"}");
    ws.sendTXT("{\"type\":\"select_group\",\"code\":\"" + code + "\"}");
    curGroupCode = code;  // 乐观显示
    setMsg("Joining " + code + " ...");
    Serial.println("[UI] join group " + code);
}

#if TOUCH_ENABLED
// 触摸轮询: PTT 区域按住=持续; 其余区域按下沿触发 onTap(确认页由 onTap 内部分支处理)
static void handleTouch() {
    static bool touchPrev = false;
    int16_t tx = 0, ty = 0;
    bool touching = lcd.getTouch(&tx, &ty);
    if (touching) {
        if (!askSetup && pointInRect(PTT_X, PTT_Y, PTT_W, PTT_H, tx, ty)) pttTouchHeld = true;
        if (!touchPrev) onTap(tx, ty);
    } else {
        pttTouchHeld = false;
    }
    touchPrev = touching;
}
#endif

// ---------- WebSocket ----------
static void onWsEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
        case WStype_CONNECTED: {
            wsOk = true;
            uiNeedRedraw = true;
            Serial.printf("[WS] connected: %s\n", (char *)payload);
            // hello:服务器按 MAC 识别;join_code 自动入群;proto 协议版本
            String hello = String("{\"type\":\"hello\",\"mac\":\"") +
                           WiFi.macAddress() + "\",\"proto\":1";
            if (cfg.groupCode.length() == 6) {
                hello += String(",\"join_code\":\"") + cfg.groupCode + "\"";
            }
            hello += "}";
            ws.sendTXT(hello);
            break;
        }
        case WStype_DISCONNECTED:
            if (wsOk) Serial.println("[WS] disconnected");
            wsOk = false;
            uiNeedRedraw = true;
            pttState = PTT_IDLE;
            break;
        case WStype_TEXT:
            Serial.printf("[WS] text: %.*s\n", (int)len, (const char *)payload);
            if (strstr((const char *)payload, "\"ptt_grant\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    ringClear();
                    pttState = PTT_HOLDING;
                    uiNeedRedraw = true;
                    Serial.println("[PTT] floor granted, talking");
                } else {
                    ws.sendTXT("{\"type\":\"ptt_release\"}");
                }
            } else if (strstr((const char *)payload, "\"ptt_deny\"") != nullptr) {
                if (pttState == PTT_REQUESTING) {
                    pttState = PTT_IDLE;
                    uiNeedRedraw = true;
                    Serial.println("[PTT] floor denied, someone is talking");
                }
            } else if (strstr((const char *)payload, "\"ptt_revoke\"") != nullptr) {
                if (pttState == PTT_HOLDING) {
                    pttState = PTT_IDLE;
                    uiNeedRedraw = true;
                    Serial.println("[PTT] floor preempted");
                }
            } else if (strstr((const char *)payload, "\"error\"") != nullptr) {
                // 提取 message 字段给用户提示(如"群号不存在")
                const char *m = strstr((const char *)payload, "\"message\"");
                String msg = "error";
                if (m) {
                    const char *q = strchr(m, ':');
                    if (q) {
                        const char *s = strchr(q, '"');
                        if (s) {
                            s++;
                            const char *e = strchr(s, '"');
                            if (e) msg = String(s).substring(0, (int)(e - s));
                        }
                    }
                }
                setMsg(msg);
            }
            break;
        case WStype_BIN:
            ringPush(payload, len);
            break;
        default:
            break;
    }
}

// ---------- 主流程 ----------
void setup() {
    Serial.begin(115200);
    pinMode(BOOT_KEY_PIN, INPUT_PULLUP);
    pinMode(PA_EN_PIN, OUTPUT);
    digitalWrite(PA_EN_PIN, HIGH);  // 先关闭功放(低电平使能),连接就绪后再打开

    // 先初始化 I2C(ES8311 与触摸 FT5x06 共用 Wire),再初始化屏幕/触摸
    Wire.begin(CODEC_I2C_SDA, CODEC_I2C_SCL);
    Wire.setClock(100000);

    // 初始化屏幕(含触摸)
    lcd.init();
    lcd.setRotation(0);
    pinMode(LCD_PIN_BL, OUTPUT);
    digitalWrite(LCD_PIN_BL, HIGH);
    uiRender();

    // 加载配置(NVS 优先,回退编译期默认)
    configLoad();
    Serial.printf("[boot] MAC: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[配置] SSID=%s 服务器=%s:%u 群号=%s (来源: %s)\n",
                  cfg.ssid.c_str(), cfg.host.c_str(), cfg.port,
                  cfg.groupCode.length() ? cfg.groupCode.c_str() : "(无)",
                  cfg.provisioned() ? "配网页面" : "编译默认");

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());

    Serial.println("[WiFi] connecting (max 15s)...");
    dispWifi = "WiFi connecting...";
    uiNeedRedraw = true;
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(50);
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] connect failed, entering provisioning");
        dispWifi = "WiFi FAILED -> setup";
        uiNeedRedraw = true;
        uiRender();
        provisioningStart();
        return;  // loop() 将只运行配网
    }

    wifiOk = true;
    dispWifi = "WiFi: " + WiFi.localIP().toString();
    uiNeedRedraw = true;

    // 初始化 ES8311(I2C 地址 0x18;18dB 采集增益对应寄存器值 0x04)
    if (es8311::init(SAMPLE_RATE, CODEC_MIC_GAIN_REG, CODEC_DAC_VOLUME)) {
        Serial.println("[codec] ES8311 ready");
    } else {
        Serial.println("[codec] ES8311 init FAILED (check I2C wiring)");
    }

    i2sInstallTx();
    digitalWrite(PA_EN_PIN, LOW);  // 使能功放

    ws.begin(cfg.host.c_str(), cfg.port, SERVER_PATH);
    ws.onEvent(onWsEvent);

    Serial.printf("[服务器] %s:%u%s\n", cfg.host.c_str(), cfg.port, SERVER_PATH);
    Serial.printf("[boot] firmware: S3-ES3 v1.2 (portal + touch UI)\n");
}

// 配网模式下的屏幕呈现(由 provisioning.h 每秒调用一次)
void provisioningUiTick() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    lcd.fillScreen(TFT_BLACK);
    lcd.setTextColor(UI_RGB(255, 160, 30), TFT_BLACK);
    lcd.setTextDatum(lgfx::textdatum_t::middle_center);
    lcd.setFont(&fonts::Font4);
    lcd.drawString("SETUP MODE", 120, 48);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(lgfx::textdatum_t::top_left);
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(8, 100);
    lcd.print("1. Connect phone WiFi:");
    lcd.setCursor(8, 122);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.print(provisioningApSsid());
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(8, 150);
    lcd.print("2. Open in browser:");
    lcd.setCursor(8, 172);
    lcd.setTextColor(TFT_CYAN, TFT_BLACK);
    lcd.print("http://" + WiFi.softAPIP().toString());
    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setCursor(8, 200);
    lcd.print("3. Set WiFi/Server/Group");
    lcd.print("  phones:");
    lcd.print(provisioningStations());
}

void loop() {
    if (provisioningActive()) {
        provisioningLoop();
        return;
    }
    ws.loop();

    // WiFi 看门狗
    static uint32_t lastWifiTry = 0;
    if (WiFi.status() == WL_CONNECTED) {
        if (!wifiOk) {
            wifiOk = true;
            dispWifi = "WiFi: " + WiFi.localIP().toString();
            uiNeedRedraw = true;
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
        ws.begin(cfg.host.c_str(), cfg.port, SERVER_PATH);
    }

#if TOUCH_ENABLED
    handleTouch();
#endif

    // 临时提示到期清除
    if (msgUntil && millis() > msgUntil) {
        msgUntil = 0;
        uiNeedRedraw = true;
    }

    // PTT 触发源: 板载 BOOT 键 或 触摸 PTT 按钮(二者等效)
    static bool lastPressed = false;
    static uint32_t lastChange = 0;
    bool bootLow = digitalRead(BOOT_KEY_PIN) == LOW;
    bool pressed = bootLow || pttTouchHeld;
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
                uiNeedRedraw = true;
                Serial.println("[PTT] requesting floor...");
            }
        } else {
            if (pttState != PTT_IDLE) {
                ws.sendTXT("{\"type\":\"ptt_release\"}");
                Serial.println("[PTT] floor released");
            }
            pttState = PTT_IDLE;
            uiNeedRedraw = true;
        }
    }

    // 申请超时
    if (pttState == PTT_REQUESTING && millis() - pttRequestAt > PTT_REQUEST_TIMEOUT_MS) {
        pttState = PTT_IDLE;
        uiNeedRedraw = true;
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
        uiNeedRedraw = true;
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

    // 界面有变化时整屏重绘(状态/组号/话权/按键)
    if (uiNeedRedraw) {
        uiRender();
        uiNeedRedraw = false;
    }
}

/*
 * 本地化说明(中文标签):
 *   LovyanGFX 自带字体(Font0~Font8 / efont)不含中文字形,直接 drawString 中文会显示空白或乱码。
 *   若要显示中文:
 *     1) 准备一个包含所需汉字的字体(如 u8g2 / LVGL 导出的 .bin,或 Arduino GFX 字体 .h);
 *     2) 用 lgfx::Font 加载(例如 lcd.loadFont("/fonts/zh.bin") 或 lcd.setFont(&myCjkFont));
 *     3) 把上方 LBL_* 常量换成中文串即可(如 "加入组"、"删除"、"按住说话")。
 *   触摸坐标若发现 X/Y 反了或镜像,调整 LGFX 构造函数中 _touch.config() 的 cfg.offset_rotation(0/1/2/3)。
 */
