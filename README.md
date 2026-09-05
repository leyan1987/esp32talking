# esp32talking

小型可自部署的 ESP32 对讲机项目(C/S 架构)。

## 组成

- `server/` — Python (FastAPI) 服务器:WebSocket 音频转发、设备管理(按 MAC 新增/禁用/删除)、群组管理、Web 管理页
- `firmware/` — ESP32 固件(Arduino/PlatformIO):WiFi 接入、I2S 采集(INMP441)与播放(MAX98357A)、PTT 按键、WebSocket 长连接

## 里程碑

1. **M1** 音频链路打通:ESP32 采集 → 服务器回发 → 播放(裸 PCM)← 当前阶段
2. **M2** 设备接入鉴权:MAC 白名单,REST 管理接口
3. **M3** 双机互通 + PTT:群组通话、话权控制、ADPCM 压缩
4. **M4** 完善期:多群组、管理页面、离线检测

## M1 快速开始

### 硬件接线(每台设备)

| INMP441 麦克风 | ESP32 | | MAX98357A 功放 | ESP32 |
|---|---|---|---|---|
| VDD | 3.3V | | VIN | 5V 或 3.3V |
| GND | GND | | GND | GND |
| SCK | GPIO 14 | | BCLK | GPIO 26 |
| WS | GPIO 15 | | LRC | GPIO 25 |
| SD | GPIO 32 | | DIN | GPIO 22 |
| L/R | GND | | GAIN / SD | 悬空 |

按键:GPIO 4 ↔ GND(一端一端)。LED 用板载 GPIO 2。

### 服务器

```bash
cd server
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt   # Windows
.venv\Scripts\python server.py                  # 监听 0.0.0.0:8000
```

首次运行如弹出 Windows 防火墙提示,请允许。

### 固件

1. 编辑 `firmware/src/config.h`,填入 WiFi 名称/密码和运行服务器的电脑 IP
2. VS Code 安装 PlatformIO 插件打开 `firmware/` 目录(或命令行 `pip install platformio` 后在 `firmware/` 下执行):
   ```bash
   pio run -t upload && pio device monitor
   ```
3. 行为验证:LED 常亮表示已连上服务器 → **按住**板载按键(GPIO4)说话 → **松开**,喇叭应回放刚才的录音

> 提示:`config.h` 中含 WiFi 密码,推送到公开仓库前请自行处理。

## 硬件清单(每台设备)

- ESP32 开发板(推荐 ESP32-S3,M1 用经典 ESP32 即可)
- INMP441 I2S 麦克风
- MAX98357A I2S 功放 + 喇叭
- PTT 按键 + 状态 LED
