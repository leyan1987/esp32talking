#pragma once
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config_store.h"

// 配网门户(WiFiManager 方案):
//   设备开热点 esp32talking-XXXX(无密码)→ 手机连接后自动弹出配置页
//   (或手动访问 http://192.168.4.1)→ 填 WiFi/服务器/群号 → 写入 NVS → 重启生效。
//   DNS 泛解析 + 302 重定向实现 captive portal。
// 两个固件工程各有一份拷贝,内容保持一致。
// main.cpp 需提供 void provisioningUiTick()(每秒一次,绘制 LED/屏幕)。

static DNSServer dnsServer;
static WebServer webServer(80);
static String apSsid;
static bool provisioningOn = false;

static String provisioningApSsid() { return apSsid; }
static bool provisioningActive() { return provisioningOn; }
static uint8_t provisioningStations() { return WiFi.softAPgetStationNum(); }

// 各工程实现:provisioning 模式下每秒一次的状态呈现(LED/屏幕)
extern void provisioningUiTick();

static String htmlEscape(const String &s) {
    String r = s;
    r.replace("&", "&amp;");
    r.replace("\"", "&quot;");
    r.replace("<", "&lt;");
    r.replace(">", "&gt;");
    return r;
}

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32talking 配置</title><style>
body{font-family:system-ui,"Microsoft YaHei",sans-serif;margin:14px;max-width:460px;color:#222}
h3{margin:8px 0}label{display:block;font-size:13px;color:#666;margin-top:10px}
input,select,button{width:100%;box-sizing:border-box;padding:10px;font-size:16px;margin:4px 0;border:1px solid #ccc;border-radius:6px}
button{background:#2563eb;color:#fff;border:0;margin-top:16px}
small{color:#999;display:block;margin-top:12px}</style></head><body>
<h3>esp32talking 设备配置</h3>
<form action="/save" method="POST">
<label>WiFi 名称</label><input name="ssid" list="nets" value="%SSID%" maxlength="32" placeholder="选择或输入">
<datalist id="nets">%NETWORKS%</datalist>
<label>WiFi 密码</label><input name="pass" value="%PASS%" maxlength="64">
<label>服务器地址(电脑局域网 IP)</label><input name="host" value="%HOST%" placeholder="如 192.168.1.100">
<label>端口</label><input name="port" value="%PORT%" inputmode="numeric">
<label>群号(6 位数字,可留空由管理台分配)</label><input name="gcode" value="%GCODE%" inputmode="numeric" maxlength="6">
<button type="submit">保存并重启设备</button></form>
<small>保存后设备重启并连接所配 WiFi。手机提示"该热点无法上网"属正常现象,请保持连接直到保存完成。</small>
</body></html>)rawliteral";

static const char SAVED_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>已保存</title></head>
<body style="font-family:system-ui;margin:20px"><h3>&#10004; 配置已保存</h3>
<p>设备正在重启,稍后会自动连接所配置的 WiFi 并上线。本页可关闭。</p></body></html>)rawliteral";

static String buildPortalPage() {
    String nets;
    int n = WiFi.scanComplete();
    if (n > 0) {
        int count = 0;
        for (int i = 0; i < n && count < 15; i++) {
            String s = WiFi.SSID(i);
            if (s.length() == 0) continue;  // 隐藏网络无法在列表中选择
            bool dup = false;
            for (int j = 0; j < i; j++)
                if (WiFi.SSID(j) == s) { dup = true; break; }
            if (dup) continue;
            nets += "<option>" + htmlEscape(s) + "</option>";
            count++;
        }
        WiFi.scanDelete();
    }
    String page = PORTAL_HTML;
    page.replace("%SSID%", htmlEscape(cfg.ssid));
    page.replace("%PASS%", htmlEscape(cfg.pass));
    page.replace("%HOST%", htmlEscape(cfg.host));
    page.replace("%PORT%", String(cfg.port));
    page.replace("%GCODE%", htmlEscape(cfg.groupCode));
    page.replace("%NETWORKS%", nets);
    return page;
}

static void handleRoot() {
    webServer.send(200, "text/html; charset=utf-8", buildPortalPage());
}

static void handleSave() {
    String ssid = webServer.arg("ssid");
    ssid.trim();
    String pass = webServer.arg("pass");
    String host = webServer.arg("host");
    host.trim();
    String portS = webServer.arg("port");
    portS.trim();
    String gcode = webServer.arg("gcode");
    gcode.trim();

    if (ssid.length() == 0 || host.length() == 0) {
        webServer.send(400, "text/html; charset=utf-8",
                       "<meta charset=\"utf-8\">WiFi 名称与服务器地址不能为空。<a href=\"/\">返回</a>");
        return;
    }
    uint16_t port = (uint16_t)portS.toInt();
    if (port == 0) port = 8000;
    bool digits = gcode.length() == 6;
    for (unsigned i = 0; digits && i < gcode.length(); i++)
        if (!isDigit(gcode[i])) digits = false;
    if (!digits) gcode = "";

    configSave(ssid, pass, host, port, gcode);
    Serial.printf("[配网] 已保存: SSID=%s 服务器=%s:%u 群号=%s,即将重启\n",
                  ssid.c_str(), host.c_str(), port, gcode.c_str());
    webServer.send(200, "text/html; charset=utf-8", SAVED_HTML);
    delay(1500);  // 让响应送达手机
    ESP.restart();
}

static void handleNotFound() {
    webServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    webServer.send(302, "text/plain", "");
}

// 进入配网模式(不返回):AP + DNS 泛解析 + 配置页;之后 loop() 调 provisioningLoop()
static void provisioningStart() {
    WiFi.mode(WIFI_AP_STA);
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    apSsid = String("esp32talking-") + mac.substring(mac.length() - 4);
    WiFi.softAP(apSsid.c_str());
    delay(150);
    WiFi.scanNetworks();  // 同步扫描,供配置页网络列表(约 2~3 秒)
    dnsServer.start(53, "*", WiFi.softAPIP());
    webServer.on("/", handleRoot);
    webServer.on("/save", handleSave);
    webServer.onNotFound(handleNotFound);
    webServer.begin();
    provisioningOn = true;
    Serial.println("[配网] ===== 配网模式 =====");
    Serial.printf("[配网] 热点: %s (无密码)\n", apSsid.c_str());
    Serial.printf("[配网] 配置页: http://%s/\n", WiFi.softAPIP().toString().c_str());
    provisioningUiTick();
}

static void provisioningLoop() {
    dnsServer.processNextRequest();
    webServer.handleClient();
    static uint32_t lastUi = 0;
    if (millis() - lastUi >= 1000) {
        lastUi = millis();
        provisioningUiTick();
    }
}
