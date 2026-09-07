#pragma once
#include <Arduino.h>
#include <Wire.h>

// ES8311 音频编解码芯片驱动(自包含,基于 Wire/I2C)
// 寄存器序列提取自乐鑫 ESP-BSP 的 ES8311 驱动(MIT License),
// Copyright (c) 2019 Espressif Systems (Shanghai) CO., LTD
//
// 时钟:ES8311 作为 I2S 从机,ESP32 提供主时钟 MCLK = 256 * 采样率

namespace es8311 {

// 寄存器地址
constexpr uint8_t REG_RESET = 0x00;      // 00 复位/主从模式
constexpr uint8_t REG_CLK01 = 0x01;      // 01 时钟管理(反相/时钟源)
constexpr uint8_t REG_CLK02 = 0x02;      // 02 时钟管理(预分频/倍频)
constexpr uint8_t REG_CLK03 = 0x03;      // 03 ADC 采样率比/OSR
constexpr uint8_t REG_CLK04 = 0x04;      // 04 DAC 采样率比/OSR
constexpr uint8_t REG_CLK05 = 0x05;      // 05 ADC/DAC 时钟分频
constexpr uint8_t REG_CLK06 = 0x06;      // 06 BCLK 分频/反相
constexpr uint8_t REG_CLK07 = 0x07;      // 07 LRCK 高位
constexpr uint8_t REG_CLK08 = 0x08;      // 08 LRCK 低位
constexpr uint8_t REG_SDPIN = 0x09;      // 09 DAC 串口数据端口格式
constexpr uint8_t REG_SDPOUT = 0x0A;     // 0A ADC 串口数据端口格式
constexpr uint8_t REG_SYS0B = 0x0B;
constexpr uint8_t REG_SYS0C = 0x0C;
constexpr uint8_t REG_SYS0D = 0x0D;
constexpr uint8_t REG_SYS0E = 0x0E;
constexpr uint8_t REG_SYS10 = 0x10;
constexpr uint8_t REG_SYS11 = 0x11;
constexpr uint8_t REG_SYS12 = 0x12;
constexpr uint8_t REG_SYS13 = 0x13;
constexpr uint8_t REG_SYS14 = 0x14;      // 14 DMIC/模拟 MIC 等
constexpr uint8_t REG_ADC15 = 0x15;
constexpr uint8_t REG_ADC16 = 0x16;      // 16 MIC 增益
constexpr uint8_t REG_ADC17 = 0x17;
constexpr uint8_t REG_ADC1B = 0x1B;
constexpr uint8_t REG_ADC1C = 0x1C;
constexpr uint8_t REG_DAC31 = 0x31;      // 31 静音控制位
constexpr uint8_t REG_DAC32 = 0x32;      // 32 DAC 音量(0~255)
constexpr uint8_t REG_DAC37 = 0x37;
constexpr uint8_t REG_GP45 = 0x45;

constexpr uint8_t ADDR = 0x18;           // CE 引脚接地时的 I2C 地址

static void setVolume(int volume);  // 前置声明(定义在后)

static bool writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static int readReg(uint8_t reg) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((int)ADDR, 1) != 1) return -1;
    return Wire.read();
}

// 时钟系数表(节选自乐鑫驱动,仅保留 16kHz 可用组合;MCLK/FS=256)
struct Coeff {
    uint32_t mclk;
    uint32_t rate;
    uint8_t pre_div;
    uint8_t pre_multi;  // 1/2/4/8
    uint8_t adc_div;
    uint8_t dac_div;
    uint8_t fs_mode;    // 0=单速 1=倍速
    uint8_t lrck_h;
    uint8_t lrck_l;
    uint8_t bclk_div;
    uint8_t adc_osr;
    uint8_t dac_osr;
};

static const Coeff coeffs16k[] = {
    // mclk      rate   pre_div multi adc dac fs  lrch lrcl bck osr
    {12288000, 16000, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {18432000, 16000, 0x03, 0x02, 0x03, 0x03, 0x00, 0x02, 0xff, 0x0c, 0x10, 0x10},
    {16384000, 16000, 0x04, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {8192000, 16000, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {6144000, 16000, 0x03, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {4096000, 16000, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {3072000, 16000, 0x03, 0x04, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
    {2048000, 16000, 0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

// 初始化:sampleRate 采样率(16000),mclkFre = 256*sampleRate,
// micGainReg 写入 REG16(参考 esp-bsp:0x01=0dB,每档+6dB,0x08=42dB),
// dacVolume 0~100(内部按对数映射到 REG32)
static bool init(uint32_t sampleRate, uint8_t micGainReg, uint8_t dacVolume) {
    uint32_t mclkFre = sampleRate * 256;

    // 基础初始化序列(与乐鑫驱动 init() 一致)
    writeReg(REG_CLK01, 0x30);
    writeReg(REG_CLK02, 0x00);
    writeReg(REG_CLK03, 0x10);
    writeReg(REG_ADC16, 0x24);
    writeReg(REG_CLK04, 0x10);
    writeReg(REG_CLK05, 0x00);
    writeReg(REG_SYS0B, 0x00);
    writeReg(REG_SYS0C, 0x00);
    writeReg(REG_SYS10, 0x1F);
    writeReg(REG_SYS11, 0x7F);
    writeReg(REG_RESET, 0x80);

    // 从机模式(ESP32 I2S 为主机)
    int regv = readReg(REG_RESET);
    regv &= 0xBF;
    writeReg(REG_RESET, regv);
    writeReg(REG_CLK01, 0x3F);
    // 内部 MCLK 时钟源来自 MCLK 引脚
    regv = readReg(REG_CLK01);
    regv &= 0x7F;
    writeReg(REG_CLK01, regv);

    // 查时钟系数表
    const Coeff *coeff = nullptr;
    for (auto &c : coeffs16k) {
        if (c.rate == sampleRate && c.mclk == mclkFre) { coeff = &c; break; }
    }
    if (coeff == nullptr) return false;

    regv = readReg(REG_CLK02) & 0x07;
    regv |= (coeff->pre_div - 1) << 5;
    uint8_t multiCode = 0;  // 1/2/4/8 -> 0/1/2/3
    if (coeff->pre_multi == 2) multiCode = 1;
    else if (coeff->pre_multi == 4) multiCode = 2;
    else if (coeff->pre_multi == 8) multiCode = 3;
    regv |= multiCode << 3;
    writeReg(REG_CLK02, regv);

    regv = ((coeff->adc_div - 1) << 4) | (coeff->dac_div - 1);
    writeReg(REG_CLK05, regv);

    regv = (readReg(REG_CLK03) & 0x80) | (coeff->fs_mode << 6) | coeff->adc_osr;
    writeReg(REG_CLK03, regv);
    regv = (readReg(REG_CLK04) & 0x80) | coeff->dac_osr;
    writeReg(REG_CLK04, regv);
    regv = (readReg(REG_CLK07) & 0xC0) | coeff->lrck_h;
    writeReg(REG_CLK07, regv);
    regv = coeff->lrck_l;
    writeReg(REG_CLK08, regv);
    regv = (readReg(REG_CLK06) & 0xE0) | (coeff->bclk_div < 19 ? coeff->bclk_div - 1 : coeff->bclk_div);
    writeReg(REG_CLK06, regv);

    // MCLK/BCLK 不反相
    regv = readReg(REG_CLK01) & ~(0x40);
    writeReg(REG_CLK01, regv);
    regv = readReg(REG_CLK06) & ~(0x20);
    writeReg(REG_CLK06, regv);

    writeReg(REG_SYS13, 0x10);
    writeReg(REG_ADC1B, 0x0A);
    writeReg(REG_ADC1C, 0x6A);

    // I2S 标准格式 + 16bit(DAC 输入端口与 ADC 输出端口)
    uint8_t dacIface = readReg(REG_SDPIN) & 0xFC;
    uint8_t adcIface = readReg(REG_SDPOUT) & 0xFC;
    dacIface |= 0x0C;
    adcIface |= 0x0C;
    writeReg(REG_SDPIN, dacIface);
    writeReg(REG_SDPOUT, adcIface);

    // 启动 ADC+DAC(CODEC_MODE_BOTH)
    dacIface = readReg(REG_SDPIN) & 0xBF;
    adcIface = readReg(REG_SDPOUT) & 0xBF;
    writeReg(REG_SDPIN, dacIface);
    writeReg(REG_SDPOUT, adcIface);

    writeReg(REG_ADC17, 0xBF);
    writeReg(REG_SYS0E, 0x02);
    writeReg(REG_SYS12, 0x00);
    writeReg(REG_SYS14, 0x1A);
    regv = readReg(REG_SYS14) & ~(0x40);  // 模拟 MIC(非 DMIC)
    writeReg(REG_SYS14, regv);
    writeReg(REG_SYS0D, 0x01);
    writeReg(REG_ADC15, 0x40);
    writeReg(REG_DAC37, 0x48);
    writeReg(REG_GP45, 0x00);

    // MIC 增益与 DAC 音量
    writeReg(REG_ADC16, micGainReg);
    setVolume(dacVolume);

    // 解除静音
    regv = readReg(REG_DAC31) & 0x9F;
    writeReg(REG_DAC31, regv);
    writeReg(REG_SYS12, 0x00);

    return true;
}

// 音量 0~100(对数映射,与乐鑫驱动一致)
static void setVolume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    int vol = 0;
    if (volume > 0) {
        vol = (int)(255.0 * log10(9.0 * volume / 100.0 + 1.0) / log10(10.0));
    }
    writeReg(REG_DAC32, vol);
}

}  // namespace es8311
