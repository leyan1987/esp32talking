"""M3 服务器功能测试(管理台鉴权 / 话权 / 群号 / 群组 / 路由 / 管理)

前置:全新数据库启动服务器。
覆盖:
0. 管理台鉴权:未登录 401、错误密码 401、默认密码登录
1. 未知设备自动注册 + welcome(空群组)
2. REST 建群(6 位群号)、手工新增设备、拉成员、改名
3. welcome 返回群组列表与默认活动群组
4. 话权控制:申请→授权→发送;未授权音频丢弃;释放;优先级抢占;低优先级被拒
4b. WS list_members 成员在线状态
4c. WS rename_group(安卓方式)与越权改名报错
5. WS create_group:创建者自动加入并切换
6. WS join_group:错误群号报 error,正确群号加入
7. REST 群组改名推送
8. hello 携带 join_code 自动入群(ESP32 方式)
9. 设备列表(含优先级)
9b. set_name 客户端自改昵称
10. 禁用设备 -> 踢下线 + 重连被拒;重新启用恢复
11. 删除群组后成员群组列表更新
12. 管理页可访问
"""

import asyncio
import json
import os
import sys
import urllib.error
import urllib.request

import websockets

URI = "ws://127.0.0.1:8000/ws"
API = "http://127.0.0.1:8000/api"
FRAME = 640

TOKEN = ""


def api(method: str, path: str, body: dict | None = None) -> dict:
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if TOKEN:
        headers["X-Auth-Token"] = TOKEN
    req = urllib.request.Request(API + path, data=data, method=method, headers=headers)
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read().decode())


async def recv_json(ws, timeout=5):
    return json.loads(await asyncio.wait_for(ws.recv(), timeout))


async def recv_of_type(ws, mtype: str, timeout=5):
    """读取直到指定类型的控制消息(忽略路上其他的)。"""
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if msg.get("type") == mtype:
            return msg


async def recv_bytes(ws, timeout=5):
    """读取直到收到二进制音频帧(忽略控制消息)。"""
    for _ in range(20):
        got = await asyncio.wait_for(ws.recv(), timeout)
        if isinstance(got, bytes):
            return got
    raise AssertionError("未收到音频帧")


async def drain(ws, timeout=0.4):
    """清空待读消息,避免影响后续断言。"""
    while True:
        try:
            await asyncio.wait_for(ws.recv(), timeout)
        except asyncio.TimeoutError:
            return
        except websockets.ConnectionClosed:
            return


async def expect_silence(ws, timeout=0.7):
    """期待超时内没有二进制音频帧(控制消息允许出现)。"""
    try:
        while True:
            got = await asyncio.wait_for(ws.recv(), timeout)
            if isinstance(got, bytes):
                raise AssertionError(f"期待静默,却收到音频帧")
    except asyncio.TimeoutError:
        pass


async def hello_device(ws, dev_id: str, kind: str = "esp32", join_code: str | None = None) -> dict:
    msg = {"type": "hello", "id": dev_id, "kind": kind}
    if join_code:
        msg["join_code"] = join_code
    await ws.send(json.dumps(msg))
    return await recv_json(ws)


def ensure_login() -> None:
    global TOKEN
    # 未登录访问受保护接口 -> 401
    try:
        urllib.request.urlopen(API + "/devices")
        raise AssertionError("未登录却访问成功")
    except urllib.error.HTTPError as e:
        assert e.code == 401
    # 错误密码 -> 401
    req = urllib.request.Request(
        API + "/login", data=json.dumps({"password": "wrong-pass"}).encode(),
        method="POST", headers={"Content-Type": "application/json"},
    )
    try:
        urllib.request.urlopen(req)
        raise AssertionError("错误密码却登录成功")
    except urllib.error.HTTPError as e:
        assert e.code == 401
    # 默认密码登录
    data = api("POST", "/login", {"password": "admin123"})
    assert data["default_password"] is True
    TOKEN = data["token"]
    print("0. 管理台鉴权 OK(未登录 401 / 错误密码 401 / 默认密码登录)")


async def main() -> int:
    D1, D2, D3 = "TEST-AABB-0001", "TEST-AABB-0002", "TEST-AABB-0003"

    ensure_login()

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
        api("POST", "/devices", {"id": D2, "name": "ESP32-书房"})
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

        # 4) 话权控制全流程
        await w2.send(json.dumps({"type": "select_group", "group_id": g2["id"]}))
        await recv_of_type(w2, "groups")

        await w1.send(json.dumps({"type": "ptt_request"}))
        grant = await recv_of_type(w1, "ptt_grant")
        assert grant["group_id"] == g1["id"]

        await w1.send(os.urandom(FRAME))
        await expect_silence(w2)  # D2 在"家庭",听不到
        await expect_silence(w1)  # 无回环

        await w1.send(json.dumps({"type": "ptt_release"}))
        await w1.send(os.urandom(FRAME))  # 释放后未再申请,应被丢弃
        await expect_silence(w1)
        await expect_silence(w2)

        await w2.send(json.dumps({"type": "select_group", "group_id": g1["id"]}))
        await recv_of_type(w2, "groups")
        await w2.send(json.dumps({"type": "ptt_request"}))
        await recv_of_type(w2, "ptt_grant")
        frame = os.urandom(FRAME)
        await w2.send(frame)
        got = await recv_bytes(w1)
        assert got == frame, "群组内互通失败"

        # 优先级抢占:A 调到 9,A 申请 -> B 被夺权
        api("PATCH", f"/devices/{D1}", {"priority": 9})
        await w1.send(json.dumps({"type": "ptt_request"}))
        await recv_of_type(w1, "ptt_grant")
        await recv_of_type(w2, "ptt_revoke")
        # B 重新申请 -> 低优先级被拒
        await w2.send(json.dumps({"type": "ptt_request"}))
        deny = await recv_of_type(w2, "ptt_deny")
        assert deny["holder_name"] == "ESP32-客厅", deny
        # A 释放 -> 状态广播
        await w1.send(json.dumps({"type": "ptt_release"}))
        status = await recv_of_type(w2, "ptt_status")
        assert status["held"] is False, status
        await drain(w1)
        await drain(w2)
        print("4. 话权控制(授权/门控丢弃/互通/抢占/拒绝/释放) OK")

        # 4b) WS list_members 成员在线状态
        await w1.send(json.dumps({"type": "list_members", "group_id": g1["id"]}))
        mem = await recv_of_type(w1, "members")
        assert {m["id"] for m in mem["members"]} == {D1, D2}, mem
        assert all(m["online"] for m in mem["members"])
        print("4b. WS 成员列表 + 在线状态 OK")

        # 4c) WS rename_group(安卓方式)
        await w1.send(json.dumps({"type": "rename_group", "group_id": g1["id"], "name": "车队B组"}))
        pushed = await recv_of_type(w1, "groups")
        renamed = next(g for g in pushed["groups"] if g["id"] == g1["id"])
        assert renamed["name"] == "车队B组", pushed
        await w1.send(json.dumps({"type": "rename_group", "group_id": g2["id"], "name": "x"}))
        await recv_of_type(w1, "error")  # 非成员改名被拒
        await drain(w1)
        await drain(w2)
        print("4c. WS 改群名 + 越权拒绝 OK")

        # 5) WS create_group:创建者自动加入并切换为当前群组
        await w1.send(json.dumps({"type": "create_group", "name": "测试组"}))
        created = await recv_of_type(w1, "created")
        assert 100000 <= created["group_id"] <= 999999, created
        gpush = await recv_of_type(w1, "groups")
        assert any(g["id"] == created["group_id"] for g in gpush["groups"]), gpush
        assert gpush["active_group_id"] == created["group_id"], gpush
        print(f"5. WS 新建群组 OK(群号 {created['group_id']})")

        # 6) WS join_group:错误群号 -> error;正确群号 -> 加入
        await w1.send(json.dumps({"type": "join_group", "code": "abc"}))
        await recv_of_type(w1, "error")
        await w1.send(json.dumps({"type": "join_group", "code": str(g2["id"])}))
        gpush = await recv_of_type(w1, "groups")
        assert any(g["id"] == g2["id"] for g in gpush["groups"]), gpush
        print("6. WS 凭群号加入 OK(含错误群号提示)")

        # 7) REST 群组改名推送(管理台方式)
        api("PATCH", f"/groups/{g1['id']}", {"name": "车队A组"})
        pushed = await recv_of_type(w1, "groups")
        renamed = next(g for g in pushed["groups"] if g["id"] == g1["id"])
        assert renamed["name"] == "车队A组", pushed
        print("7. REST 群组改名 + 推送 OK")

        await drain(w1)
        await drain(w2)

    # 8) hello 携带 join_code 自动入群(ESP32 方式)
    async with websockets.connect(URI) as w3:
        wl3 = await hello_device(w3, D3, join_code=str(g1["id"]))
        assert any(g["name"] == "车队A组" for g in wl3["groups"]), wl3
        assert wl3["active_group_id"] == g1["id"], wl3
        print("8. hello join_code 自动入群 OK")

    # 9) 设备列表(含优先级)
    devs = api("GET", "/devices")["devices"]
    d1 = next(d for d in devs if d["id"] == D1)
    assert d1["name"] == "ESP32-客厅" and d1["priority"] == 9, d1
    print("9. 设备列表(名称/优先级) OK")

    # 9b) 客户端自改昵称(set_name)
    async with websockets.connect(URI) as w1:
        await hello_device(w1, D1)
        await w1.send(json.dumps({"type": "set_name", "name": "  老王的手机  "}))
        resp = await recv_of_type(w1, "device_info")
        assert resp["name"] == "老王的手机", resp
        await w1.send(json.dumps({"type": "set_name", "name": "   "}))
        await recv_of_type(w1, "error")
        devs = api("GET", "/devices")["devices"]
        assert next(d for d in devs if d["id"] == D1)["name"] == "老王的手机"
        print("9b. 客户端自改昵称 OK(空昵称报 error)")

    # 10) 禁用 -> 踢下线 -> 重连被拒 -> 启用恢复
    async with websockets.connect(URI) as w2:
        await hello_device(w2, D2)
        api("PATCH", f"/devices/{D2}", {"enabled": False})
        try:
            await asyncio.wait_for(w2.recv(), 5)  # rejected
            await asyncio.wait_for(w2.recv(), 5)  # close
        except websockets.ConnectionClosed:
            pass

        async with websockets.connect(URI) as w2b:
            await w2b.send(json.dumps({"type": "hello", "id": D2}))
            rej = await recv_of_type(w2b, "rejected")
            assert rej["reason"], rej
        api("PATCH", f"/devices/{D2}", {"enabled": True})
        async with websockets.connect(URI) as w2c:
            wl = await hello_device(w2c, D2)
            assert wl["type"] == "welcome"
        print("10. 禁用/踢线/拒绝重连/恢复 OK")

    # 11) 删除群组后成员群组列表更新(D2 在"家庭"里)
    async with websockets.connect(URI) as w2:
        await hello_device(w2, D2)
        api("DELETE", f"/groups/{g2['id']}")
        pushed = await recv_of_type(w2, "groups")
        assert all(g["id"] != g2["id"] for g in pushed["groups"]), pushed
        print("11. 删除群组 + 成员列表更新 OK")

    # 12) 管理页(登录视图)
    with urllib.request.urlopen("http://127.0.0.1:8000/") as resp:
        html = resp.read().decode()
        assert "esp32talking 管理台" in html and "loginView" in html
    print("12. 管理页(含登录) OK")

    print("全部测试通过 ✔")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
