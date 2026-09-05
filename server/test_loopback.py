"""M1 服务器回环测试(模拟 ESP32 客户端)

1. 发送文本控制消息 -> 期待 ACK
2. 发送 640 字节"音频帧"(一帧 20ms@16kHz PCM16) -> 期待原样回传
3. 连发 50 帧验证流式回环
"""

import asyncio
import os
import sys

import websockets

URI = "ws://127.0.0.1:8000/ws"
FRAME = 640  # FRAME_BYTES, 与固件一致


async def main() -> int:
    async with websockets.connect(URI, max_size=1 << 20) as ws:
        # 1) 文本 ACK
        await ws.send('{"type":"hello","mac":"TEST:00:00","proto":1}')
        ack = await asyncio.wait_for(ws.recv(), timeout=5)
        print("文本 ACK:", ack)

        # 2) 单帧回环
        frame = os.urandom(FRAME)
        await ws.send(frame)
        echo = await asyncio.wait_for(ws.recv(), timeout=5)
        assert isinstance(echo, bytes), f"期待二进制回传,得到 {type(echo)}"
        assert echo == frame, "回传内容与发送不一致"
        print(f"单帧回环 OK: {len(echo)} 字节,内容一致")

        # 3) 连发 50 帧(约 1 秒音频)
        for _ in range(50):
            await ws.send(os.urandom(FRAME))
        ok = 0
        for _ in range(50):
            back = await asyncio.wait_for(ws.recv(), timeout=5)
            assert isinstance(back, bytes) and len(back) == FRAME
            ok += 1
        print(f"流式回环 OK: 50/50 帧")

    print("测试通过 ✔")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
