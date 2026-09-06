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
- ✅ **话权控制(M3)**:同组同时只允许一人说话;按键先申请话权(先到先得),松开释放;**设备优先级 0~9,高者可抢占**;持话权 15 秒无音频自动释放;未授权音频被服务器丢弃
- ✅ Web 管理台:浏览器打开 `http://<服务器IP>:8000/`,**登录后使用**(默认密码 `admin123`,请立即修改)
- ✅ 安卓端:新建群组 / 群号加入 / 切换当前群组 / 群组改名 / 群成员在线状态 / 自改昵称 / 话权等待与抢占提示
- ✅ **ADPCM 压缩(M3)**:IMA ADPCM 4:1,上行/下行带宽 32KB/s → **8KB/s**;帧头自带解码状态,丢帧不扩散;接收端按帧长自动识别新(ADPCM 164 字节)/旧(PCM 640 字节)格式;ESP32 播放缓冲等效时长 ×4(约 8 秒);语音 SNR 实测约 30dB
- ✅ 安卓保活:前台服务 + CPU/WiFi 锁,灭屏、划掉 App 后仍保持在线
- ✅ 设备身份与恢复码:每台设备有 **10 位恢复码**(连接后自动下发,管理台可查);安卓重装 App 后点【设备码】输入旧码即可找回原设备和全部历史群组;ANDROID_ID 稳定的手机重装后自动恢复
- ⏳ M4(可选):多群组提示音、管理台美化、设备离线告警、WSS 加密

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

- 管理台:`http://<服务器IP>:8000/`,**需登录**(默认密码 `admin123`,登录后右上角"修改密码"立即更换;也可用环境变量 `ESP32TALKING_ADMIN` 设初始密码);管理台可新增设备、建群、拉成员、切当前群组、设置话权优先级、禁用/删除
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
4. `config.h` 的 `USE_ADPCM 1` 开启压缩(默认);排查问题时可改 0 回退裸 PCM。**注意:压缩需固件与安卓两端同时更新,旧版混用时音频无法正常播放**

### 安卓客户端

1. 安装 `android/app/build/outputs/apk/debug/app-debug.apk`,或用 Android Studio 打开 `android/` 自行构建
2. 填服务器地址 `ws://<电脑IP>:8000/ws` → 连接 → 允许麦克风权限(通知权限建议允许)
3. 群组操作:**新建群组**(创建后自动加入并切换,系统会报出 6 位群号)、**群号加入**(输入别人分享的 6 位号码)、**改名**;下拉框选择"当前群组"进行监听/通讯
4. **成员**按钮可查看当前群组内谁的在线/离线(3 秒自动刷新)
5. **改昵称**:修改自己在群组里的显示名(成员列表/管理台都会显示);昵称保存在服务器,重装不丢
6. 连接后通知栏会出现常驻通知,这是保活机制——灭屏、划掉 App 后连接仍然保持;请勿强制停止应用,部分国产 ROM 需在设置里允许该应用"后台运行/自启动"

### 数据存在哪里

- **所有数据(设备、群组、成员关系、昵称、恢复码)都在服务器的 `server/esp32talking.db`**(SQLite 单文件),备份这个文件即可;删除它等于全部重置
- 手机端不保存群组;设备身份由服务器分配 **10 位恢复码**并在连接时下发保存

### 重装 App 后恢复群组(恢复码)

1. 平时:App 里点【设备码】可见当前恢复码,截图/抄写保存(管理台设备列表也可查)
2. 重装后:连上服务器 → 点【设备码】→ 输入旧恢复码 → 自动重连,服务器找回原设备,welcome 自动下发全部历史群组
3. 若手机 ANDROID_ID 稳定(多数原生系统),重装后身份自动一致,无需手动输入

> 注:安卓 6+ 拿不到稳定 WiFi MAC,客户端用持久化 UUID 作为设备 ID;管理台里可为其改名区分。

## 协议速查(WebSocket)

- 文本帧 JSON:`hello{id,kind,join_code?,recovery?}` → `welcome{device:{id,name,recovery_code},groups,active_group_id}`(recovery 能对上已有设备时找回身份;join_code 存在时自动入群);`create_group{name}` → `created{group_id,name}` + `groups{...}`;`join_group{code}` → `groups{...}`(群号错误回 `error`);`select_group{group_id}`、`list_groups` → `groups{...}`;`set_name{name}` → `device_info`;`rename_group{group_id,name}`(仅限自己所在群组)、`list_members{group_id}` → `members{...}`;被禁用收 `rejected{reason}` 后被断开
- **话权(M3)**:`ptt_request` → `ptt_grant` 或 `ptt_deny{holder_name}`;`ptt_release` 释放;被抢占收 `ptt_revoke`;群内广播 `ptt_status{held,holder_name}`;未持话权的音频帧被服务器丢弃
- 二进制帧:**ADPCM 164 字节**(M3,4 字节状态头 + 160 字节数据)或裸 PCM 640 字节(旧客户端兼容),接收端按帧长自动识别;服务器按发送方"当前群组"原样转发给同组其他在线成员,不回发给发送方

## 硬件清单(每台 ESP32 设备)

- ESP32 开发板(推荐 ESP32-S3,M1 用经典 ESP32 即可)
- INMP441 I2S 麦克风
- MAX98357A I2S 功放 + 喇叭
- PTT 按键 + 状态 LED
