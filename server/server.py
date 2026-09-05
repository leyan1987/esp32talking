"""esp32talking 服务器 (M1)

M1 目标:验证音频链路 —— 设备通过 WebSocket 上传的音频帧原样回发,
设备端按下 PTT 说话、松开后应能听到自己的回放。

M2 起将扩展为:MAC 设备注册表 + 群组转发。
"""

import json
import logging

from fastapi import FastAPI, WebSocket, WebSocketDisconnect

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("esp32talking")

app = FastAPI(title="esp32talking")


class ConnectionManager:
    """M1:仅维护在线连接数;M2 扩展为 mac -> 连接的注册表。"""

    def __init__(self) -> None:
        self.active: dict[str, WebSocket] = {}

    async def connect(self, ws: WebSocket, client_id: str) -> None:
        await ws.accept()
        self.active[client_id] = ws

    def disconnect(self, client_id: str) -> None:
        self.active.pop(client_id, None)


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
                # M1 回环:音频帧原样发回
                await ws.send_bytes(data)
    except WebSocketDisconnect:
        pass
    finally:
        manager.disconnect(client_id)
        log.info("设备断开: %s (在线 %d)", client_id, len(manager.active))


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
