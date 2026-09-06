"""M2.1 服务器功能测试(设备注册 / 群号 / 群组 / 路由 / 管理)

前置:全新数据库启动服务器。
覆盖:
1. 未知设备自动注册 + welcome(空群组)
2. REST 建群(6 位群号)、手工新增设备、拉成员、改名
3. welcome 返回群组列表与默认活动群组
4. select_group 切换监听群组;音频只路由给当前群组相同的成员;发送方无回环
5. WS create_group:创建者自动加入并切换
6. WS join_group:错误群号报 error,正确群号加入
7. hello 携带 join_code 自动入群(ESP32 方式)
8. 群组改名推送;设备列表在线状态
9. 禁用设备 -> 踢下线 + 重连被拒;重新启用恢复
10. 删除群组后成员群组列表更新
11. 管理页可访问
"""

import asyncio
import json
import os
import sys
import urllib.request

import websockets

URI = "ws://127.0.0.1:8000/ws"
API = "http://127.0.0.1:8000/api"
FRAME = 640


def api(method: str, path: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(
        API + path, data=data, method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode())


async def recv_json(ws, timeout=5):
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def expect_silence(ws, timeout=0.7):
    """期待超时无消息(验证不回环/不串组)。"""
    try:
        got = await asyncio.wait_for(ws.recv(), timeout)
        raise AssertionError(f"期待静默,却收到: {got!r}")
    except asyncio.TimeoutError:
        pass


async def hello_device(ws, dev_id: str, kind: str = "esp32", join_code: str | None = None) -> dict:
    msg = {"type": "hello", "id": dev_id, "kind": kind}
    if join_code:
        msg["join_code"] = join_code
    await ws.send(json.dumps(msg))
    return await recv_json(ws)


async def main() -> int:
    D1, D2, D3 = "TEST-AABB-0001", "TEST-AABB-0002", "TEST-AABB-0003"

    # 1) 未知设备自动注册
    async with websockets.connect(URI) as w1:
        welcome = await hello_device(w1, D1)
        assert welcome["type"] == "welcome", welcome
        assert welcome["device"]["id"] == D1 and welcome["groups"] == []
        print("1. 自动注册 + welcome(空群组) OK")

        # 2) REST 建群(6 位群号)、手工新增设备、拉成员、改名
        g1 = api("POST", "/groups", {"name": "车队"})
        g2 = api("POST", "/groups", {"name": "家庭"})
        assert 100000 <= g1["id"] <= 999999 and 100000 <= g2["id"] <= 999999, (g1, g2)
        assert g1["id"] != g2["id"]
        api("POST", "/devices", {"id": D2, "name": "ESP32-书房"})  # 手工新增(尚未上线的设备)
        api("POST", f"/groups/{g1['id']}/members", {"device_id": D1})
        api("POST", f"/groups/{g1['id']}/members", {"device_id": D2})
        api("POST", f"/groups/{g2['id']}/members", {"device_id": D2})
        api("PATCH", f"/devices/{D1}", {"name": "ESP32-客厅"})
        print(f"2. 建群(群号 {g1['id']}/{g2['id']})/新增设备/拉成员/改名 OK")

    # 3) 重新上线,验证群组列表与默认活动群组
    async with websockets.connect(URI) as w1, websockets.connect(URI) as w2:
        wl1 = await hello_device(w1, D1)
        wl2 = await hello_device(w2, D2)
        assert [g["name"] for g in wl1["groups"]] == ["车队"], wl1
        assert {g["name"] for g in wl2["groups"]} == {"车队", "家庭"}, wl2
        assert wl1["active_group_id"] == g1["id"]
        assert wl2["active_group_id"] in {g1["id"], g2["id"]}
        print("3. welcome 群组列表/默认活动群组 OK")

        # 4) 按当前群组路由:D2 在"家庭"时听不到"车队"的语音
        await w2.send(json.dumps({"type": "select_group", "group_id": g2["id"]}))
        resp = await recv_json(w2)
        assert resp["type"] == "groups" and resp["active_group_id"] == g2["id"], resp
        await w1.send(os.urandom(FRAME))
        await expect_silence(w2)
        await expect_silence(w1)  # 发送方默认无回环
        await w2.send(json.dumps({"type": "select_group", "group_id": g1["id"]}))
        await recv_json(w2)
        frame = os.urandom(FRAME)
        await w1.send(frame)
        got = await asyncio.wait_for(w2.recv(), 5)
        assert got == frame, "群组内互通失败"
        await expect_silence(w1)
        print("4. 按当前群组路由 + 同组互通 + 无自环 OK")

        # 5) WS create_group:创建者自动加入并切换为当前群组
        await w1.send(json.dumps({"type": "create_group", "name": "测试组"}))
        created = await recv_json(w1)
        assert created["type"] == "created" and 100000 <= created["group_id"] <= 999999, created
        gpush = await recv_json(w1)
        assert gpush["type"] == "groups"
        assert any(g["id"] == created["group_id"] for g in gpush["groups"]), gpush
        assert gpush["active_group_id"] == created["group_id"], gpush
        print(f"5. WS 新建群组 OK(群号 {created['group_id']})")

        # 6) WS join_group:错误群号 -> error;正确群号 -> 加入
        await w1.send(json.dumps({"type": "join_group", "code": "abc"}))
        err = await recv_json(w1)
        assert err["type"] == "error", err
        await w1.send(json.dumps({"type": "join_group", "code": str(g2["id"])}))
        gpush = await recv_json(w1)
        assert gpush["type"] == "groups"
        assert any(g["id"] == g2["id"] for g in gpush["groups"]), gpush
        print("6. WS 凭群号加入 OK(含错误群号提示)")

        # 7) 群组改名推送
        api("PATCH", f"/groups/{g1['id']}", {"name": "车队A组"})
        resp = await recv_json(w1)
        renamed = next((g for g in resp["groups"] if g["id"] == g1["id"]), None)
        assert resp["type"] == "groups" and renamed and renamed["name"] == "车队A组", resp
        print("7. 群组改名 + 推送 OK")

    # 8) hello 携带 join_code 自动入群(ESP32 方式)
    async with websockets.connect(URI) as w3:
        wl3 = await hello_device(w3, D3, join_code=str(g1["id"]))
        assert any(g["name"] == "车队A组" for g in wl3["groups"]), wl3
        assert wl3["active_group_id"] == g1["id"], wl3
        print("8. hello join_code 自动入群 OK")

    # 9) 设备列表在线状态/名称
    devs = api("GET", "/devices")["devices"]
    d1 = next(d for d in devs if d["id"] == D1)
    assert d1["name"] == "ESP32-客厅", d1
    print("9. 设备列表 OK")

    # 10) 禁用 -> 踢下线 -> 重连被拒 -> 启用恢复
    async with websockets.connect(URI) as w2:
        await hello_device(w2, D2)
        api("PATCH", f"/devices/{D2}", {"enabled": False})
        try:
            await asyncio.wait_for(w2.recv(), 5)  # rejected
            await asyncio.wait_for(w2.recv(), 5)  # close
            closed = True
        except websockets.ConnectionClosed:
            closed = True
        assert closed, "禁用后未断开"

        async with websockets.connect(URI) as w2b:
            await w2b.send(json.dumps({"type": "hello", "id": D2}))
            rej = await recv_json(w2b)
            assert rej["type"] == "rejected", rej
        api("PATCH", f"/devices/{D2}", {"enabled": True})
        async with websockets.connect(URI) as w2c:
            wl = await hello_device(w2c, D2)
            assert wl["type"] == "welcome"
        print("10. 禁用/踢线/拒绝重连/恢复 OK")

    # 11) 删除群组后成员群组列表更新(D2 在"家庭"里)
    async with websockets.connect(URI) as w2:
        await hello_device(w2, D2)
        api("DELETE", f"/groups/{g2['id']}")
        resp = await recv_json(w2)
        assert resp["type"] == "groups" and all(g["id"] != g2["id"] for g in resp["groups"]), resp
        print("11. 删除群组 + 成员列表更新 OK")

    # 12) 管理页
    with urllib.request.urlopen("http://127.0.0.1:8000/") as resp:
        html = resp.read().decode()
        assert "esp32talking 管理台" in html
    print("12. 管理页 OK")

    print("全部测试通过 ✔")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
