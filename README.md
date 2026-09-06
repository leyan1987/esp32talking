# esp32talking

小型可自部署的 ESP32 对讲机项目(C/S 架构),含安卓客户端与 Web 管理台。

## 组成

- `server/` — Python (FastAPI) 服务器:设备注册表(MAC/UUID)、群组、按当前群组路由语音、REST 管理接口、Web 管理台
- `firmware/` — ESP32 固件(Arduino/PlatformIO):WiFi 接入、I2S 采集(INMP441)与播放(MAX98357A)、PTT 按键、WebSocket 长连接
- `android/` — 安卓客户端(Kotlin/OkHttp):群组选择、改群组名、按住说话/松开收听

## 功能现状

- ✅ 音频链路:16kHz/16bit PCM、20ms 一帧,局域网延迟约 100~200ms
- ✅ 设备管理:首次连接自动注册(ESP32 按 MAC、安卓按 UUID),可改名/禁用/删除;禁用立即踢下线且无法重连
- ✅ 群组:每个群组有 **6 位数字群号**,安卓可新建群组、凭群号加入;ESP32 在 `config.h` 里填群号自动加入(或由管理台拉人)
- ✅ 多群组:设备可加入多个群组,每台设备有"当前群组",语音只路由给当前群组相同的在线成员
- ✅ Web 管理台:浏览器打开 `http://<服务器IP>:8000/`
- ✅ 安卓端:新建群组 / 群号加入 / 切换当前群组 / 群组改名
- ⏳ M3 待做:话权控制(先按先得)、ADPCM 压缩

## 快速开始

### 硬件接线(每台 ESP32 设备)

| INMP441 麦克风 | ESP32 | | MAX98357A 功放 | ESP32 |
|---|---|---|---|---|
| VDD | 3.3V | | VIN | 5V 或 3.3V |
| GND | GND | | GND | GND |
| SCK | GPIO 14 | | BCLK | GPIO 26 |
| WS | GPIO 15 | | LRC | GPIO 25 |
| SD | GPIO 32 | | DIN | GPIO 22 |
| L/R | GND | | GAIN / SD | 悬空(音量小可把 GAIN 接 GND,+3dB) |

按键:GPIO 4 ↔ GND。LED 用板载 GPIO 2。

### 服务器

```bash
cd server
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt   # Windows
.venv\Scripts\python server.py                  # 监听 0.0.0.0:8000
.venv\Scripts\python test_server.py             # (可选)自动化功能测试
```

也可以双击 `server\start_server.bat` 启动。

- 管理台:`http://<服务器IP>:8000/`(新增设备、建群、拉成员、切当前群组、禁用/删除)
- 数据库:`server/esp32talking.db`(SQLite,可随时备份/删除重置)
- 单机自环测试(听到自己的回放,仅调试用):
  PowerShell: `$env:ESP32TALKING_ECHO="1"; .venv\Scripts\python server.py`

### ESP32 固件

1. 编辑 `firmware/src/config.h`,填入 WiFi 名称/密码、服务器 IP,以及要加入的 **6 位群号**(`JOIN_GROUP_CODE`,留空则由管理台手动拉入群组)
2. VS Code 安装 PlatformIO 插件打开 `firmware/` 目录(或命令行 `pip install platformio` 后在 `firmware/` 下执行):
   ```bash
   pio run -t upload && pio device monitor
   ```
3. 音量调节(`config.h` 的 `MIC_GAIN_SHIFT`):16=0dB、14=+12dB、12=+24dB(当前默认),数值越小越响;爆音则调大

### 安卓客户端

1. 安装 `android/app/build/outputs/apk/debug/app-debug.apk`,或用 Android Studio 打开 `android/` 自行构建
2. 填服务器地址 `ws://<电脑IP>:8000/ws` → 连接 → 允许麦克风权限
3. 群组操作:**新建群组**(创建后自动加入并切换,系统会报出 6 位群号)、**群号加入**(输入别人分享的 6 位号码)、**改名**;下拉框选择"当前群组"进行监听/通讯

> 注:安卓 6+ 拿不到稳定 WiFi MAC,客户端用持久化 UUID 作为设备 ID;管理台里可为其改名区分。

## 协议速查(WebSocket)

- 文本帧 JSON:`hello{id,kind,join_code?}` → `welcome{device,groups,active_group_id}`(join_code 存在时自动入群);`create_group{name}` → `created{group_id,name}` + `groups{...}`;`join_group{code}` → `groups{...}`(群号错误回 `error`);`select_group{group_id}`、`list_groups` → `groups{...}`;被禁用收 `rejected{reason}` 后被断开
- 二进制帧:640 字节 PCM,服务器按发送方"当前群组"转发给同组其他在线成员,不回发给发送方

## 硬件清单(每台 ESP32 设备)

- ESP32 开发板(推荐 ESP32-S3,M1 用经典 ESP32 即可)
- INMP441 I2S 麦克风
- MAX98357A I2S 功放 + 喇叭
- PTT 按键 + 状态 LED
