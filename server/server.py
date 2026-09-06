"""esp32talking 服务器 (M2)

功能:
- 设备注册表:ESP32 按 MAC、安卓按 UUID 自动注册(也可 REST 手工新增),
  支持改名、禁用(在线即踢下线,重连被拒)、删除
- 群组:创建/改名/删除,设备可加入多个群组
- 语音路由:每台设备有一个"当前群组"(客户端可用 select_group 切换,
  管理员也可在网页上指定),音频帧只转发给"当前群组相同"的在线成员
- Web 管理台:浏览器打开 http://<服务器IP>:8000/

协议(WebSocket 文本帧为 JSON,二进制帧为 640 字节 PCM):
  C->S  {"type":"hello","id":"..","kind":"..","join_code":"123456"?}
                                                 首条消息,id=MAC或UUID;
                                                 join_code 可选,自动加入该群号
  C->S  {"type":"select_group","group_id":N}     切换当前群组(监听/通讯目标)
  C->S  {"type":"create_group","name":".."}      新建群组(创建者自动加入并切换)
  C->S  {"type":"join_group","code":"123456"}    凭 6 位群号加入群组
  C->S  {"type":"list_groups"}                   请求刷新自己的群组列表
  S->C  {"type":"welcome","device":{...},"groups":[..],"active_group_id":N}
  S->C  {"type":"groups","groups":[..],"active_group_id":N}  群组变更推送
  S->C  {"type":"created","group_id":N,"name":".."}
  S->C  {"type":"error","message":".."}
  S->C  {"type":"rejected","reason":".."}        随后断开(设备被禁用)
  S->C  {"type":"ack","echo":".."}               M1 兼容应答

自环开关:默认不再把音频回发给发送方(真实对讲行为)。
单机自测时启动:  ESP32TALKING_ECHO=1 python server.py (Linux/macOS)
或 PowerShell:    $env:ESP32TALKING_ECHO="1"; python server.py
"""

import asyncio
import json
import logging
import os
import random
import sqlite3
from dataclasses import dataclass, field
from pathlib import Path

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("esp32talking")

DB_PATH = Path(__file__).parent / "esp32talking.db"
ADMIN_HTML = Path(__file__).parent / "static" / "admin.html"
ECHO_TO_SENDER = os.environ.get("ESP32TALKING_ECHO", "0") == "1"

app = FastAPI(title="esp32talking")

# ---------------- 数据库 ----------------

db = sqlite3.connect(DB_PATH)
db.row_factory = sqlite3.Row
db.executescript(
    """
    CREATE TABLE IF NOT EXISTS devices(
        id TEXT PRIMARY KEY,          -- ESP32 的 MAC 或安卓的 UUID
        name TEXT NOT NULL,
        enabled INTEGER NOT NULL DEFAULT 1,
        active_group_id INTEGER,
        created_at TEXT DEFAULT (datetime('now','localtime')),
        last_seen TEXT
    );
    CREATE TABLE IF NOT EXISTS groups(
        id INTEGER PRIMARY KEY,       -- 群号:6 位数字(100000~999999),客户端凭此加入
        name TEXT NOT NULL,
        created_at TEXT DEFAULT (datetime('now','localtime'))
    );
    CREATE TABLE IF NOT EXISTS group_members(
        group_id INTEGER NOT NULL REFERENCES groups(id) ON DELETE CASCADE,
        device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
        PRIMARY KEY(group_id, device_id)
    );
    """
)
db.execute("PRAGMA foreign_keys=ON")


def now() -> str:
    import datetime

    return datetime.datetime.now().isoformat(timespec="seconds")


def allocate_group_id() -> int:
    """分配一个未占用的 6 位群号。"""
    while True:
        gid = random.randint(100000, 999999)
        if not db.execute("SELECT 1 FROM groups WHERE id = ?", (gid,)).fetchone():
            return gid


def find_group_by_code(code: str):
    """按群号字符串查找群组,兼容前导零。"""
    if not code.isdigit():
        return None
    return db.execute("SELECT * FROM groups WHERE id = ?", (int(code),)).fetchone()


# ---------------- 在线连接 ----------------


@dataclass
class Conn:
    conn_id: str
    ws: WebSocket
    device_id: str
    active_group_id: int | None = None
    member_groups: set[int] = field(default_factory=set)
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)


conns: dict[str, Conn] = {}          # conn_id -> Conn
conn_by_device: dict[str, str] = {}  # device_id -> conn_id (保留最新)


async def ws_send_json(conn: Conn, obj: dict) -> None:
    try:
        async with conn.send_lock:
            await conn.ws.send_text(json.dumps(obj, ensure_ascii=False))
    except Exception:
        log.exception("发送给 %s 失败", conn.device_id)


async def kick(conn: Conn, reason: str) -> None:
    """通知并断开某连接(禁用/删除设备时调用)。"""
    try:
        async with conn.send_lock:
            await conn.ws.send_text(json.dumps({"type": "rejected", "reason": reason}))
            await conn.ws.close(code=1008, reason=reason)
    except Exception:
        pass


def group_rows(device_id: str) -> list[dict]:
    rows = db.execute(
        """SELECT g.id, g.name FROM groups g
           JOIN group_members m ON m.group_id = g.id
           WHERE m.device_id = ? ORDER BY g.id""",
        (device_id,),
    ).fetchall()
    return [{"id": r["id"], "name": r["name"]} for r in rows]


async def push_groups(conn: Conn) -> None:
    await ws_send_json(
        conn,
        {
            "type": "groups",
            "groups": group_rows(conn.device_id),
            "active_group_id": conn.active_group_id,
        },
    )


def first_group_of(device_id: str) -> int | None:
    r = db.execute(
        "SELECT MIN(group_id) AS g FROM group_members WHERE device_id = ?", (device_id,)
    ).fetchone()
    return r["g"]


# ---------------- WebSocket 端点 ----------------


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    conn_id = f"{ws.client.host}:{ws.client.port}"
    conn: Conn | None = None
    try:
        # ---- 第一条消息必须是 hello ----
        msg = await ws.receive()
        if msg.get("text") is None:
            await ws.close(code=1002)
            return
        try:
            hello = json.loads(msg["text"])
        except json.JSONDecodeError:
            await ws.close(code=1002)
            return
        device_id = str(hello.get("id") or hello.get("mac") or "").strip().upper()
        if not device_id:
            await ws.close(code=1002)
            return

        # ---- 设备注册校验(未知设备自动注册) ----
        dev = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
        if dev is None:
            db.execute(
                "INSERT INTO devices(id, name, last_seen) VALUES(?, ?, ?)",
                (device_id, f"设备-{device_id[-4:]}", now()),
            )
            db.commit()
            dev = db.execute("SELECT * FROM devices WHERE id = ?", (device_id,)).fetchone()
            log.info("新设备自动注册: %s (%s)", device_id, hello.get("kind", "?"))
        if not dev["enabled"]:
            await ws.send_text(
                json.dumps({"type": "rejected", "reason": "设备已被禁用"}, ensure_ascii=False)
            )
            log.info("拒绝被禁用设备接入: %s", device_id)
            await ws.close(code=1008)
            return

        # ---- hello 携带 join_code 时自动加入该群组(ESP32 免输入加入方式) ----
        join_code = str(hello.get("join_code") or "").strip()
        if join_code:
            g = find_group_by_code(join_code)
            if g is not None:
                db.execute(
                    "INSERT OR IGNORE INTO group_members(group_id, device_id) VALUES(?, ?)",
                    (g["id"], device_id),
                )
                db.commit()
                log.info("设备 %s 凭群号 %s 加入群组", device_id, join_code)
            else:
                log.warning("设备 %s 的 join_code %s 无效,忽略", device_id, join_code)

        # ---- 同设备重复连接:踢掉旧连接 ----
        old = conn_by_device.get(device_id)
        if old and old in conns:
            log.info("设备 %s 重复连接,踢掉旧连接 %s", device_id, old)
            await kick(conns[old], "设备在其他地方连接")
            conns.pop(old, None)

        conn = Conn(conn_id=conn_id, ws=ws, device_id=device_id)
        conn.member_groups = {g["id"] for g in group_rows(device_id)}
        active = dev["active_group_id"]
        conn.active_group_id = active if active in conn.member_groups else (
            next(iter(sorted(conn.member_groups)), None)
        )
        conns[conn_id] = conn
        conn_by_device[device_id] = conn_id
        db.execute("UPDATE devices SET last_seen = ? WHERE id = ?", (now(), device_id))
        db.commit()

        await ws_send_json(
            conn,
            {
                "type": "welcome",
                "device": {"id": device_id, "name": dev["name"]},
                "groups": group_rows(device_id),
                "active_group_id": conn.active_group_id,
            },
        )
        log.info(
            "设备上线: %s (%s) 当前群组 %s, 在线 %d",
            device_id, dev["name"], conn.active_group_id, len(conns),
        )

        # ---- 消息循环 ----
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if "text" in msg and msg["text"] is not None:
                await handle_text(conn, msg["text"])
            elif "bytes" in msg and msg["bytes"] is not None:
                await relay_audio(conn, msg["bytes"])
    except WebSocketDisconnect:
        pass
    finally:
        if conn is not None:
            conns.pop(conn.conn_id, None)
            if conn_by_device.get(conn.device_id) == conn.conn_id:
                conn_by_device.pop(conn.device_id, None)
            log.info("设备下线: %s (在线 %d)", conn.device_id, len(conns))


async def handle_text(conn: Conn, text: str) -> None:
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        return
    t = obj.get("type")

    if t == "select_group":
        gid = obj.get("group_id")
        if isinstance(gid, int) and gid in conn.member_groups:
            conn.active_group_id = gid
            db.execute(
                "UPDATE devices SET active_group_id = ? WHERE id = ?", (gid, conn.device_id)
            )
            db.commit()
            await push_groups(conn)
            log.info("设备 %s 切换群组 -> %d", conn.device_id, gid)

    elif t == "create_group":
        # 安卓端"新建群组":创建后创建者自动加入并切换为当前群组
        name = str(obj.get("name", "")).strip()
        if not name:
            await ws_send_json(conn, {"type": "error", "message": "群组名不能为空"})
            return
        gid = allocate_group_id()
        db.execute("INSERT INTO groups(id, name) VALUES(?, ?)", (gid, name))
        db.execute(
            "INSERT INTO group_members(group_id, device_id) VALUES(?, ?)",
            (gid, conn.device_id),
        )
        conn.member_groups.add(gid)
        conn.active_group_id = gid
        db.execute(
            "UPDATE devices SET active_group_id = ? WHERE id = ?", (gid, conn.device_id)
        )
        db.commit()
        await ws_send_json(conn, {"type": "created", "group_id": gid, "name": name})
        await push_groups(conn)
        log.info("设备 %s 创建群组 %d (%s)", conn.device_id, gid, name)

    elif t == "join_group":
        # 凭 6 位群号加入群组
        code = str(obj.get("code", "")).strip()
        g = find_group_by_code(code)
        if g is None:
            await ws_send_json(conn, {"type": "error", "message": f"群号 {code} 不存在"})
            return
        db.execute(
            "INSERT OR IGNORE INTO group_members(group_id, device_id) VALUES(?, ?)",
            (g["id"], conn.device_id),
        )
        db.commit()
        conn.member_groups.add(g["id"])
        if conn.active_group_id is None:
            conn.active_group_id = g["id"]
            db.execute(
                "UPDATE devices SET active_group_id = ? WHERE id = ?",
                (g["id"], conn.device_id),
            )
            db.commit()
        await push_groups(conn)
        log.info("设备 %s 凭群号 %s 加入群组 %d", conn.device_id, code, g["id"])

    elif t == "list_groups":
        await push_groups(conn)

    else:
        await ws_send_json(conn, {"type": "ack", "echo": text})


async def relay_audio(sender: Conn, data: bytes) -> None:
    gid = sender.active_group_id
    if gid is None:
        return
    if ECHO_TO_SENDER:
        try:
            async with sender.send_lock:
                await sender.ws.send_bytes(data)
        except Exception:
            pass
    for conn in list(conns.values()):
        if conn.device_id == sender.device_id:
            continue
        if conn.active_group_id != gid or gid not in conn.member_groups:
            continue
        try:
            async with conn.send_lock:
                await conn.ws.send_bytes(data)
        except Exception:
            log.exception("转发给 %s 失败", conn.device_id)


# ---------------- REST 管理接口 ----------------


def device_payload(r: sqlite3.Row) -> dict:
    return {
        "id": r["id"],
        "name": r["name"],
        "enabled": bool(r["enabled"]),
        "online": r["id"] in conn_by_device,
        "active_group_id": r["active_group_id"],
        "last_seen": r["last_seen"],
        "groups": group_rows(r["id"]),
    }


@app.get("/")
async def admin_page():
    return FileResponse(ADMIN_HTML, media_type="text/html")


@app.get("/api/devices")
async def list_devices():
    rows = db.execute("SELECT * FROM devices ORDER BY created_at").fetchall()
    return {"devices": [device_payload(r) for r in rows]}


@app.post("/api/devices")
async def add_device(body: dict):
    dev_id = str(body.get("id", "")).strip().upper()
    if not dev_id:
        raise HTTPException(400, "缺少设备 id(MAC 或 UUID)")
    name = str(body.get("name") or f"设备-{dev_id[-4:]}")
    if db.execute("SELECT 1 FROM devices WHERE id = ?", (dev_id,)).fetchone():
        raise HTTPException(409, "设备已存在")
    db.execute("INSERT INTO devices(id, name, last_seen) VALUES(?, ?, ?)", (dev_id, name, now()))
    db.commit()
    r = db.execute("SELECT * FROM devices WHERE id = ?", (dev_id,)).fetchone()
    return device_payload(r)


@app.patch("/api/devices/{device_id}")
async def patch_device(device_id: str, body: dict):
    r = db.execute("SELECT * FROM devices WHERE id = ?", (device_id.upper(),)).fetchone()
    if r is None:
        raise HTTPException(404, "设备不存在")
    updates, args = [], []

    if "name" in body:
        updates.append("name = ?")
        args.append(str(body["name"]))
    if "enabled" in body:
        updates.append("enabled = ?")
        args.append(1 if body["enabled"] else 0)
    if "active_group_id" in body:
        gid = body["active_group_id"]
        if gid is not None:
            member = db.execute(
                "SELECT 1 FROM group_members WHERE group_id = ? AND device_id = ?",
                (gid, device_id.upper()),
            ).fetchone()
            if not member:
                raise HTTPException(400, "设备不在该群组中")
        updates.append("active_group_id = ?")
        args.append(gid)

    if updates:
        db.execute(f"UPDATE devices SET {', '.join(updates)} WHERE id = ?", (*args, device_id.upper()))
        db.commit()

    r = db.execute("SELECT * FROM devices WHERE id = ?", (device_id.upper(),)).fetchone()
    payload = device_payload(r)

    # 同步在线设备
    cid = conn_by_device.get(device_id.upper())
    if cid and cid in conns:
        conn = conns[cid]
        if "active_group_id" in body:
            conn.active_group_id = body["active_group_id"]
        if body.get("enabled") is False:
            await kick(conn, "设备已被管理员禁用")
        elif body.get("enabled") is True:
            await push_groups(conn)
        else:
            await push_groups(conn)
    return payload


@app.delete("/api/devices/{device_id}")
async def delete_device(device_id: str):
    dev_id = device_id.upper()
    if db.execute("SELECT 1 FROM devices WHERE id = ?", (dev_id,)).fetchone() is None:
        raise HTTPException(404, "设备不存在")
    cid = conn_by_device.get(dev_id)
    if cid and cid in conns:
        await kick(conns[cid], "设备已被管理员删除")
    db.execute("DELETE FROM devices WHERE id = ?", (dev_id,))
    db.commit()
    return {"ok": True}


@app.get("/api/groups")
async def list_groups():
    rows = db.execute("SELECT * FROM groups ORDER BY id").fetchall()
    out = []
    for g in rows:
        members = db.execute(
            """SELECT d.id, d.name FROM devices d
               JOIN group_members m ON m.device_id = d.id
               WHERE m.group_id = ? ORDER BY d.id""",
            (g["id"],),
        ).fetchall()
        out.append(
            {
                "id": g["id"],
                "name": g["name"],
                "members": [
                    {"id": m["id"], "name": m["name"], "online": m["id"] in conn_by_device}
                    for m in members
                ],
            }
        )
    return {"groups": out}


@app.post("/api/groups")
async def create_group(body: dict):
    name = str(body.get("name", "")).strip()
    if not name:
        raise HTTPException(400, "群组名不能为空")
    gid = allocate_group_id()
    db.execute("INSERT INTO groups(id, name) VALUES(?, ?)", (gid, name))
    db.commit()
    return {"id": gid, "name": name, "members": []}


@app.patch("/api/groups/{group_id}")
async def rename_group(group_id: int, body: dict):
    name = str(body.get("name", "")).strip()
    if not name:
        raise HTTPException(400, "群组名不能为空")
    if db.execute("SELECT 1 FROM groups WHERE id = ?", (group_id,)).fetchone() is None:
        raise HTTPException(404, "群组不存在")
    db.execute("UPDATE groups SET name = ? WHERE id = ?", (name, group_id))
    db.commit()
    # 推送给所有在该群组里的在线成员
    for conn in list(conns.values()):
        if group_id in conn.member_groups:
            await push_groups(conn)
    return {"ok": True}


@app.delete("/api/groups/{group_id}")
async def delete_group(group_id: int):
    if db.execute("SELECT 1 FROM groups WHERE id = ?", (group_id,)).fetchone() is None:
        raise HTTPException(404, "群组不存在")
    affected = [
        c for c in conns.values() if group_id in c.member_groups
    ]
    db.execute("DELETE FROM groups WHERE id = ?", (group_id,))
    db.commit()
    for conn in affected:
        conn.member_groups.discard(group_id)
        if conn.active_group_id == group_id:
            conn.active_group_id = first_group_of(conn.device_id)
            db.execute(
                "UPDATE devices SET active_group_id = ? WHERE id = ?",
                (conn.active_group_id, conn.device_id),
            )
            db.commit()
        await push_groups(conn)
    return {"ok": True}


@app.post("/api/groups/{group_id}/members")
async def add_member(group_id: int, body: dict):
    dev_id = str(body.get("device_id", "")).strip().upper()
    if db.execute("SELECT 1 FROM groups WHERE id = ?", (group_id,)).fetchone() is None:
        raise HTTPException(404, "群组不存在")
    if db.execute("SELECT 1 FROM devices WHERE id = ?", (dev_id,)).fetchone() is None:
        raise HTTPException(404, "设备不存在")
    db.execute(
        "INSERT OR IGNORE INTO group_members(group_id, device_id) VALUES(?, ?)",
        (group_id, dev_id),
    )
    db.commit()
    # 若设备还没有活动群组,默认设为该群组
    cid = conn_by_device.get(dev_id)
    if cid and cid in conns:
        conn = conns[cid]
        conn.member_groups.add(group_id)
        if conn.active_group_id is None:
            conn.active_group_id = group_id
            db.execute("UPDATE devices SET active_group_id = ? WHERE id = ?", (group_id, dev_id))
            db.commit()
        await push_groups(conn)
    return {"ok": True}


@app.delete("/api/groups/{group_id}/members/{device_id}")
async def remove_member(group_id: int, device_id: str):
    dev_id = device_id.upper()
    db.execute(
        "DELETE FROM group_members WHERE group_id = ? AND device_id = ?", (group_id, dev_id)
    )
    db.commit()
    cid = conn_by_device.get(dev_id)
    if cid and cid in conns:
        conn = conns[cid]
        conn.member_groups.discard(group_id)
        if conn.active_group_id == group_id:
            conn.active_group_id = first_group_of(dev_id)
            db.execute(
                "UPDATE devices SET active_group_id = ? WHERE id = ?",
                (conn.active_group_id, dev_id),
            )
            db.commit()
        await push_groups(conn)
    return {"ok": True}


@app.exception_handler(HTTPException)
async def http_exc_handler(request, exc: HTTPException):
    return JSONResponse(status_code=exc.status_code, content={"detail": exc.detail})


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
