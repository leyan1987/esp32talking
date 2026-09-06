"""esp32talking 服务器 (M1.1)

M1.1 行为:设备通过 WebSocket 上传的音频帧 —— 回发给发送方(自环验证),
同时转发给其他所有在线客户端(实现两台设备实时互通)。
同一时刻多人按键时语音会重叠,话权控制(先按先得)在 M3 加入。

M2 起将扩展为:设备注册表(MAC/客户端ID)+ 群组转发。
"""

import asyncio
import json
import logging

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("esp32talking")

app = FastAPI(title="esp32talking")


class ConnectionManager:
    """维护在线连接。M2 将扩展为 设备ID -> 连接 的注册表并支持群组。"""

    def __init__(self) -> None:
        self.active: dict[str, WebSocket] = {}
        # 每个连接一把发送锁:多个客户端同时转发给同一目标时避免并发写 ASGI
        self.send_locks: dict[str, asyncio.Lock] = {}

    async def connect(self, ws: WebSocket, client_id: str) -> None:
        await ws.accept()
        self.active[client_id] = ws
        self.send_locks[client_id] = asyncio.Lock()

    def disconnect(self, client_id: str) -> None:
        self.active.pop(client_id, None)
        self.send_locks.pop(client_id, None)

    async def relay(self, sender_id: str, data: bytes) -> None:
        """把音频帧转发给除发送方外的所有在线客户端。"""
        for cid, other in list(self.active.items()):
            if cid == sender_id:
                continue
            lock = self.send_locks.get(cid)
            if lock is None:
                continue
            try:
                async with lock:
                    await other.send_bytes(data)
            except Exception:
                log.exception("转发给 %s 失败", cid)


manager = ConnectionManager()


@app.get("/")
async def index() -> dict:
    return {"service": "esp32talking", "clients_online": len(manager.active)}


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    client_id = f"{ws.client.host}:{ws.client.port}"
    await manager.connect(ws, client_id)
    log.info("设备连接: %s (在线 %d)", client_id, len(manager.active))
    try:
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                break
            if "text" in msg:
                # M1:控制消息仅回 ACK,M2 开始解析 hello/ptt 等协议
                log.info("控制消息 from %s: %s", client_id, msg["text"])
                await ws.send_text(json.dumps({"type": "ack", "echo": msg["text"]}))
            elif "bytes" in msg:
                data = msg["bytes"]
                await ws.send_bytes(data)   # 回环:M1 自测音链路
                await manager.relay(client_id, data)  # 互通:转发给其他设备
    except WebSocketDisconnect:
        pass
    finally:
        manager.disconnect(client_id)
        log.info("设备断开: %s (在线 %d)", client_id, len(manager.active))


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
