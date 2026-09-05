#pragma once

// ==== WiFi ====
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASSWORD "你的WiFi密码"

// ==== 服务器 ====
// 运行 server/server.py 的电脑局域网 IP (ipconfig 查看)
#define SERVER_HOST   "192.168.1.100"
#define SERVER_PORT   8000
#define SERVER_PATH   "/ws"

// ==== 按键 / LED ====
#define PTT_BTN_PIN     4    // 按键一端接 GPIO4,另一端接 GND
#define STATUS_LED_PIN  2    // 多数 DevKit 板载 LED

// ==== 音频参数 ====
#define SAMPLE_RATE    16000
#define FRAME_MS       20
#define FRAME_SAMPLES  (SAMPLE_RATE * FRAME_MS / 1000)  // 320 采样/帧
#define FRAME_BYTES    (FRAME_SAMPLES * 2)              // 640 字节 PCM16/帧

// 麦克风增益:右移位数。16=原增益,数值越小越响(12=约+24dB),
// 嫌声音小就调小,爆音就调大。
#define MIC_GAIN_SHIFT 14

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
