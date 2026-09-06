#pragma once

// ==== WiFi ====
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"

// ==== 服务器 ====
// 运行 server/server.py 的电脑局域网 IP (ipconfig 查看)
#define SERVER_HOST   "192.168.1.100"
#define SERVER_PORT   8000
#define SERVER_PATH   "/ws"

// ==== 群组 ====
// 填 6 位群号(如 "382746"),设备连接时自动加入该群组;留空 "" 则由
// 管理台(http://服务器IP:8000/)手动把本机拉进群组
#define JOIN_GROUP_CODE ""

// ==== 按键 / LED ====
#define PTT_BTN_PIN     4    // 按键一端接 GPIO4,另一端接 GND
#define STATUS_LED_PIN  2    // 多数 DevKit 板载 LED

// ==== 音频参数 ====
#define SAMPLE_RATE    16000
#define FRAME_MS       20
#define FRAME_SAMPLES  (SAMPLE_RATE * FRAME_MS / 1000)  // 320 采样/帧
#define FRAME_BYTES    (FRAME_SAMPLES * 2)              // 640 字节 PCM16/帧

// 麦克风增益:右移位数。16=0dB,14=+12dB,12=+24dB,数值越小越响。
// 默认 12(上一版 14 偏小)。若爆音改回 13/14;若仍偏小可试 11。
// 硬件上也可把 MAX98357A 的 GAIN 引脚接 GND(输出再 +3dB)。
#define MIC_GAIN_SHIFT 12

// ==== INMP441 麦克风 (I2S0 接收) ====
// VDD->3.3V  GND->GND  L/R->GND
#define MIC_BCLK_PIN 14    // INMP441 SCK
#define MIC_WS_PIN   15    // INMP441 WS
#define MIC_DATA_PIN 32    // INMP441 SD

// ==== MAX98357A 功放 (I2S1 发送) ====
// VIN->5V/3.3V  GND->GND  GAIN/SD 悬空
#define SPK_BCLK_PIN 26
#define SPK_WS_PIN   25
#define SPK_DATA_PIN 22
