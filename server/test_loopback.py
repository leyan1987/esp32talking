"""M1.1 服务器测试(模拟两个 ESP32/安卓客户端)

1. 单客户端:文本 ACK + 音频帧回环
2. 双客户端互通:A 发的帧,A 自己收到回环,B 收到转发
"""

import asyncio
import os
import sys

import websockets

URI = "ws://127.0.0.1:8000/ws"
FRAME = 640  # FRAME_BYTES, 与固件一致


async def main() -> int:
    # 1) 单客户端
    async with websockets.connect(URI, max_size=1 << 20) as ws:
        await ws.send('{"type":"hello","mac":"TEST:00:00","proto":1}')
        ack = await asyncio.wait_for(ws.recv(), timeout=5)
        print("文本 ACK:", ack)

        frame = os.urandom(FRAME)
        await ws.send(frame)
        echo = await asyncio.wait_for(ws.recv(), timeout=5)
        assert isinstance(echo, bytes) and echo == frame, "回环失败"
        print(f"单帧回环 OK: {len(echo)} 字节,内容一致")

        for _ in range(50):
            await ws.send(os.urandom(FRAME))
        ok = 0
        for _ in range(50):
            back = await asyncio.wait_for(ws.recv(), timeout=5)
            if isinstance(back, bytes):
                ok += 1
        assert ok == 50, f"流式回环只收到 {ok}/50"
        print("流式回环 OK: 50/50 帧")

    # 2) 双客户端互通
    async with websockets.connect(URI, max_size=1 << 20) as a:
        async with websockets.connect(URI, max_size=1 << 20) as b:
            frame = os.urandom(FRAME)
            await a.send(frame)

            got_a = await asyncio.wait_for(a.recv(), timeout=5)
            got_b = await asyncio.wait_for(b.recv(), timeout=5)
            assert isinstance(got_a, bytes) and got_a == frame, "A 自环失败"
            assert isinstance(got_b, bytes) and got_b == frame, "B 未收到转发"

            # B 回一发,A 应同样收到自环+转发
            frame2 = os.urandom(FRAME)
            await b.send(frame2)
            got_b2 = await asyncio.wait_for(b.recv(), timeout=5)
            got_a2 = await asyncio.wait_for(a.recv(), timeout=5)
            assert got_b2 == frame2 and got_a2 == frame2, "B->A 方向互通失败"

    print("双向互通 OK")
    print("测试通过 ✔")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
