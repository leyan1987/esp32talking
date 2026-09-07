# esp32talking

自建服务器的 ESP32 对讲机系统(C/S 架构):ESP32 硬件对讲机 + 安卓 App + Python 服务器 + Web 管理台,全部跑在你自己的局域网里,不依赖任何云服务。

A self-hosted ESP32 walkie-talkie system: ESP32 intercom hardware, an Android app, a Python (FastAPI) server and a web admin console — everything runs on your own LAN.

[English](#features) | [中文文档](#功能特性)

## 功能特性 / Features

- 🎙 **语音对讲**:16kHz/16bit PCM、20ms 一帧;**IMA ADPCM 压缩**(带宽 32KB/s → 8KB/s),语音 SNR ≈ 30dB
- 📡 **多设备多群组**:设备可加入多个群组(6 位数字群号),语音按"当前群组"路由,同组实时互通、跨组隔离
- 🔐 **话权控制**:同组同时只有一人说话;按 PTT 先申请话权(先到先得),支持**设备优先级(0~9)抢占**、15 秒无音频自动释放
- 📱 **安卓客户端**:按住说话/松开收听、群组切换/新建/凭群号加入/改名、群成员在线状态、文字消息(离线补发)、自动响度均衡、前台服务保活(灭屏不掉线)
- 🖥 **Web 管理台**:登录鉴权,设备自动注册(MAC/UUID)、改名/禁用/删除、建群拉人、切换群组、设置优先级
- 🛡 **设备恢复码**:每台设备 10 位恢复码,重装 App / 换手机后凭码找回全部历史群组
- 💬 **文字消息**:按当前群组群发,服务器保留最近 50 条,离线设备上线自动补发;ESP32 端自动忽略

## 系统架构 / Architecture

```
                    ┌──────────────────────────────┐
                    │      服务器 (Python/FastAPI)   │
                    │  ┌────────┐  ┌─────────────┐ │
                    │  │ SQLite │  │  Web 管理台  │ │
                    │  └────────┘  └─────────────┘ │
                    │   设备注册表 / 群组 / 话权 / 消息 │
                    └──────────┬───────────────────┘
                               │ WebSocket (一条长连接)
              ┌────────────────┼────────────────┐
              │                │                │
     ┌────────┴───────┐ ┌──────┴───────┐ ┌──────┴───────┐
     │  ESP32 对讲机   │ │  ESP32 对讲机 │ │   安卓 App    │
     │ INMP441 麦克风  │ │     ...      │ │ 按住说话/文字  │
     │ MAX98357A 功放  │ │              │ │ 前台服务保活   │
     └────────────────┘ └──────────────┘ └──────────────┘
```

- 二进制音频帧:ADPCM 164 字节 / 兼容旧版 PCM 640 字节,服务器原样转发
- 控制消息:JSON 文本帧(注册、群组、话权、文字消息),协议详见 [README 协议速查](#协议速查websocket)

## 目录结构

```
esp32talking/
├── server/                 # Python 服务器(FastAPI + SQLite + Web 管理台)
│   ├── server.py           # 主服务:WebSocket / REST / 话权 / 群组
│   ├── static/admin.html   # Web 管理台(单文件)
│   ├── test_server.py      # 自动化功能测试(21 项)
│   └── start_server.bat    # Windows 一键启动
├── firmware/               # ESP32 固件(经典 ESP32 + INMP441/MAX98357A 外设方案)
│   ├── src/main.cpp        # I2S 采集播放 / ADPCM / 话权状态机
│   └── src/config.h.example# 配置模板(复制为 config.h 使用)
├── firmware-s3/            # ESP32-S3 一体板适配(LCDWIKI 2.8寸 ES3C28P/ES3N28P)
│   ├── platformio.ini      # 16MB Flash + 8MB OPI PSRAM 配置
│   ├── src/main.cpp        # ES8311 编解码(I2C)+ 半双工 I2S 切换 + ILI9341 屏显
│   ├── src/es8311.h        # 自包含 ES8311 驱动(寄存器序列源自乐鑫 ESP-BSP)
│   └── src/config.h.example# 配置模板(复制为 config.h 使用)
└── android/                # 安卓客户端(Kotlin,前台服务 + OkHttp)
    └── app/src/main/.../   # TalkService(连接与音频)/ MainActivity(界面)
```

### 已适配硬件

| 板型 | 固件目录 | 音频方案 | 显示 |
|---|---|---|---|
| 经典 ESP32-WROOM 开发板 | `firmware/` | INMP441 麦克风 + MAX98357A 功放(外接) | 无(串口日志) |
| LCDWIKI 2.8寸 ESP32-S3 模块(ES3C28P/ES3N28P,板载 ES8311 编解码 + 功放 + 硅麦 + ILI9341 触摸屏) | `firmware-s3/` | ES8311(I2C 0x18 初始化,I2S MCLK=4/BCLK=5/WS=7/DO=8/DI=6),BOOT 键作 PTT | ILI9341 屏显连接/群组/话权状态 |

## 快速开始 / Quick Start

### 1. 硬件清单(每台 ESP32 设备,约 30 元)

- ESP32 开发板(推荐 ESP32-S3,经典 ESP32 亦可)
- INMP441 I2S 数字麦克风
- MAX98357A I2S 功放 + 3W 小喇叭
- PTT 按键(GPIO4↔GND)+ 板载 LED(GPIO2)

接线表见下文[硬件接线](#硬件接线每台-esp32-设备)。

### 2. 启动服务器

```bash
cd server
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt   # Windows
.venv\Scripts\python server.py                  # 监听 0.0.0.0:8000
```

- Web 管理台:`http://<服务器IP>:8000/`,默认密码 `admin123`,**登录后请立即修改**
- 也可以双击 `server\start_server.bat`(Windows)

### 3. 烧录 ESP32 固件

1. 复制 `firmware/src/config.h.example` 为 `firmware/src/config.h`,填入 WiFi 与服务器 IP
2. 用 VS Code PlatformIO 插件打开 `firmware/`,或命令行:
   ```bash
   pip install platformio
   cd firmware && pio run -t upload && pio device monitor
   ```

### 4. 安卓客户端

- 直接从 [Releases](https://github.com/leyan1987/esp32talking/releases/latest) 下载 APK 传到手机安装(需允许"安装未知应用");或用 Android Studio 打开 `android/` 自行构建
- 填服务器地址 `ws://<服务器IP>:8000/ws` → 连接 → 允许麦克风/通知权限
- 群组操作都在 App 内:新建群组(自动分配 6 位群号)/ 群号加入 / 切换当前群组 / 改名

### 验收:LED 常亮 = 就绪;两台设备加入同一群组,一边按住说话,另一边实时出声。

<details>
<summary><b>详细使用说明(数据存储 / 恢复码 / 调优 / 调试)</b></summary>

- **数据存储**:所有数据(设备、群组、成员关系、昵称、恢复码、文字消息)都在 `server/esp32talking.db`(SQLite 单文件),备份该文件即可;删除等于全部重置
- **恢复码**:每台设备 10 位恢复码,连接后自动下发;App【设备码】按钮可查看/复制(管理台设备列表也可查),恢复码同时写入手机 `Download/esp32talking_device_code.txt`。升级安装自动找回身份;卸载重装后自动读取该文件;极端情况点【设备码】手动输入旧码
- **禁用/删除设备**:管理台操作,禁用立即踢下线且重连被拒
- **话权优先级**:管理台设备表"优先级"列(0~9),数值高的申请话权时直接抢断低者
- **音量调节**:固件 `config.h` 的 `MIC_GAIN_SHIFT`(16=0dB、14=+12dB、12=+24dB 默认,越小越响,爆音调大);App 内"音量"按钮软件增益 x1~x4;硬件可把 MAX98357A GAIN 接 GND 再 +3dB
- **单机自环调试**(听到自己的回放,验证音频链路):
  PowerShell: `$env:ESP32TALKING_ECHO="1"; .venv\Scripts\python server.py`
- **自动化测试**:`.venv\Scripts\python test_server.py`(21 项,需全新数据库启动服务器)
- **防火墙**:首次启动 Windows 弹窗请允许;手机/ESP32 与服务器需同一局域网

</details>

## 硬件接线(每台 ESP32 设备)

| INMP441 麦克风 | ESP32 | | MAX98357A 功放 | ESP32 |
|---|---|---|---|---|
| VDD | 3.3V | | VIN | 5V 或 3.3V |
| GND | GND | | GND | GND |
| SCK | GPIO 14 | | BCLK | GPIO 26 |
| WS | GPIO 15 | | LRC | GPIO 25 |
| SD | GPIO 32 | | DIN | GPIO 22 |
| L/R | GND | | GAIN / SD | 悬空(音量小可把 GAIN 接 GND,+3dB) |

## 技术要点

- **音频链路**:I2S(INMP441 32bit 采集 / MAX98357A 16bit 播放)→ 20ms 帧 → IMA ADPCM(帧头自带解码状态,丢帧不扩散)→ WebSocket 二进制帧 → 对端按帧长自动识别解码
- **性能**:ESP32 RAM 占用 35%,Flash 70%;局域网端到端延迟约 100~200ms
- **长连接保活**:WebSocket ping/pong 心跳 + 断线自动重连(指数规避)+ 安卓前台服务 + CPU/WiFi 锁 + 进程被杀自愈重连
- **音质调优**:采集端禁用 AGC(避免"越说越小"),播放端自动响度均衡(RMS 目标)叠加 1x~4x 手动增益,兼容不同 ROM 外放差异

## 协议速查(WebSocket)

- 文本帧 JSON:`hello{id,kind,join_code?,recovery?}` → `welcome{device:{id,name,recovery_code},groups,active_group_id}` + `chat_history{...}`;`create_group{name}` → `created{...}`;`join_group{code}` → `groups{...}`;`select_group` / `list_groups` → `groups{...}`;`set_name` → `device_info`;`rename_group` / `list_members` → `members{...}`;`ptt_request` → `ptt_grant` / `ptt_deny{holder_name}` / `ptt_revoke`;`ptt_release` / 群内广播 `ptt_status{held,holder_name}`;`chat{text}` → `chat{group_id,from_name,text,ts}`;被禁用收 `rejected{reason}` 后断开
- 二进制帧:ADPCM 164 字节(4 字节状态头 + 160 字节数据)或兼容裸 PCM 640 字节,按"当前群组"转发,不回发发送方

## 开发状态

- ✅ M1 音频链路 / M2 设备管理+群组+管理台 / M2.1 群号 / M2.2 保活+在线状态 / M2.3 昵称 / M2.4 恢复码 / M2.5 文字消息+文件持久化 / M3 话权控制+ADPCM
- 🔜 计划中:端到端加密(WSS/TLS)、离线语音留言、多群组并行监听

## 许可证

[MIT](LICENSE)
