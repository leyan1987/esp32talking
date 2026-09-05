# esp32talking

小型可自部署的 ESP32 对讲机项目(C/S 架构)。

## 组成

- `esp32talking-server/` — Python (FastAPI) 服务器:WebSocket 音频转发、设备管理(按 MAC 新增/禁用/删除)、群组管理、Web 管理页
- `esp32talking-firmware/` — ESP32 固件(Arduino/PlatformIO):WiFi 接入、I2S 采集(INMP441)与播放(MAX98357A)、PTT 按键、WebSocket 长连接、ADPCM 音频

## 里程碑

1. **M1** 音频链路打通:ESP32 采集 → 服务器回发 → 播放(裸 PCM)
2. **M2** 设备接入鉴权:MAC 白名单,REST 管理接口
3. **M3** 双机互通 + PTT:群组通话、话权控制、ADPCM 压缩
4. **M4** 完善期:多群组、管理页面、离线检测

## 硬件清单(每台设备)

- ESP32 开发板(推荐 ESP32-S3)
- INMP441 I2S 麦克风
- MAX98357A I2S 功放 + 喇叭
- PTT 按键 + 状态 LED
