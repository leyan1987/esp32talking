#pragma once
#include <Arduino.h>
#include <Preferences.h>

// 运行配置存储(NVS/Preferences)。
// 优先级:配网页面/屏幕写入的 NVS 值 > config.h 编译期默认值。
// 两个固件工程(firmware/ 与 firmware-s3/)各有一份拷贝,内容保持一致。
//
// NVS 命名空间 "ett-cfg":ssid/pass/host/port/gcode

struct DeviceConfig {
    String ssid;
    String pass;
    String host;
    uint16_t port = 8000;
    String groupCode;      // 6 位数字群号,可为空
    bool fromNvs = false;  // NVS 中是否有已保存配置(决定开机是否进配网)

    bool provisioned() const { return fromNvs; }
};

static DeviceConfig cfg;

static void configLoad() {
    Preferences p;
    p.begin("ett-cfg", true);
    String ssid = p.getString("ssid", "");
    String pass = p.getString("pass", "");
    String host = p.getString("host", "");
    uint16_t port = p.getUShort("port", 0);
    String gcode = p.getString("gcode", "");
    p.end();

    cfg = DeviceConfig();
    if (ssid.length() > 0) {
        cfg.fromNvs = true;
        cfg.ssid = ssid;
        cfg.pass = pass;
        cfg.host = host;
        if (port > 0) cfg.port = port;
        cfg.groupCode = gcode;
    } else {
        // 未配网过:回退到编译期默认(含占位 SSID 时连接必然失败,进而进配网)
        cfg.ssid = WIFI_SSID;
        cfg.pass = WIFI_PASSWORD;
        cfg.host = SERVER_HOST;
        cfg.port = SERVER_PORT;
        cfg.groupCode = JOIN_GROUP_CODE;
    }
}

static void configSave(const String &ssid, const String &pass, const String &host,
                       uint16_t port, const String &gcode) {
    Preferences p;
    p.begin("ett-cfg", false);
    p.putString("ssid", ssid);
    p.putString("pass", pass);
    p.putString("host", host);
    p.putUShort("port", port);
    p.putString("gcode", gcode);
    p.end();
}

// 屏幕键盘/门户修改群号:更新 NVS 与内存
static void configSetGroupCode(const String &gcode) {
    Preferences p;
    p.begin("ett-cfg", false);
    p.putString("gcode", gcode);
    p.end();
    cfg.groupCode = gcode;
}

static void configClear() {
    Preferences p;
    p.begin("ett-cfg", false);
    p.clear();
    p.end();
}
