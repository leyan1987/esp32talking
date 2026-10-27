#pragma once
#include <Arduino.h>
#include <Wire.h>

// FT6336G 电容触摸最小驱动(I2C 地址 0x38,与 ES8311 共用总线 GPIO15/16)。
// main.cpp 负责先 Wire.begin();本模块只做复位与读取。
// 坐标方向不确定性由 config.h 的 TOUCH_SWAP_XY / TOUCH_MIRROR_X / TOUCH_MIRROR_Y 翻转。

namespace ft6336 {

constexpr uint8_t ADDR = 0x38;

static bool init(int rstPin) {
    if (rstPin >= 0) {
        pinMode(rstPin, OUTPUT);
        digitalWrite(rstPin, LOW);
        delay(20);
        digitalWrite(rstPin, HIGH);
        delay(60);
    }
    Wire.beginTransmission(ADDR);
    return Wire.endTransmission() == 0;
}

// 读取第一个触点(轮询);无触摸返回 false。
// 寄存器:0x02 触点数,0x03 XH,0x04 XL,0x05 YH,0x06 YL(各 12 位)
static bool read(int16_t &x, int16_t &y) {
    Wire.beginTransmission(ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)ADDR, 5) != 5) return false;
    uint8_t status = Wire.read();
    uint8_t xh = Wire.read();
    uint8_t xl = Wire.read();
    uint8_t yh = Wire.read();
    uint8_t yl = Wire.read();
    int n = status & 0x0F;
    if (n < 1 || n > 2) return false;  // 0=无触摸;>2 非法
    int16_t rx = ((xh & 0x0F) << 8) | xl;
    int16_t ry = ((yh & 0x0F) << 8) | yl;
#if TOUCH_SWAP_XY
    int16_t t = rx;
    rx = ry;
    ry = t;
#endif
#if TOUCH_MIRROR_X
    rx = 239 - rx;
#endif
#if TOUCH_MIRROR_Y
    ry = 319 - ry;
#endif
    if (rx < 0) rx = 0;
    if (rx > 239) rx = 239;
    if (ry < 0) ry = 0;
    if (ry > 319) ry = 319;
    x = rx;
    y = ry;
    return true;
}

}  // namespace ft6336
