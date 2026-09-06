package com.esp32talking.app

import android.Manifest
import android.app.Activity
import android.content.pm.PackageManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.MotionEvent
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * esp32talking 安卓客户端 (M1)
 *
 * 与 ESP32 固件同协议:连接 ws://<服务器>:8000/ws,
 * 按住 PTT 采集 16kHz/16bit/单声道 PCM,20ms 一帧(640 字节)二进制上行;
 * 收到的二进制帧进入播放队列,松开 PTT 后播放(半双工,避免回声啸叫)。
 */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "esp32talking"
        private const val SAMPLE_RATE = 16000
        private const val FRAME_SAMPLES = 320          // 20ms
        private const val FRAME_BYTES = FRAME_SAMPLES * 2
        private const val MAX_QUEUE_FRAMES = 150       // 约 3 秒播放缓冲
        private const val REQ_PERM_RECORD = 1
        private const val PREFS = "cfg"
        private const val KEY_SERVER = "server"
        private const val KEY_DEVICE_ID = "device_id"
        private const val DEFAULT_SERVER = "ws://192.168.1.100:8000/ws"
    }

    private lateinit var tvStatus: TextView
    private lateinit var etServer: EditText
    private lateinit var btnConnect: Button
    private lateinit var btnPtt: Button

    private val ui = Handler(Looper.getMainLooper())
    private val http = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)   // WebSocket 保活
        .connectTimeout(5, TimeUnit.SECONDS)
        .build()

    private var ws: WebSocket? = null
    private var userClosed = false
    private var reconnectScheduled = false

    // ---- 采集(按住 PTT 期间) ----
    private val talking = AtomicBoolean(false)
    private var record: AudioRecord? = null
    private var recordThread: Thread? = null

    // ---- 播放(常驻) ----
    private var track: AudioTrack? = null
    private var playerThread: Thread? = null
    private val playQueue = ArrayDeque<ByteArray>()
    private val playLock = Object()

    /** 设备唯一 ID(安卓拿不到稳定 MAC,用持久化 UUID 代替,M2 注册用) */
    private fun deviceId(): String {
        val p = getSharedPreferences(PREFS, MODE_PRIVATE)
        p.getString(KEY_DEVICE_ID, null)?.let { return it }
        val id = UUID.randomUUID().toString()
        p.edit().putString(KEY_DEVICE_ID, id).apply()
        return id
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        tvStatus = findViewById(R.id.tvStatus)
        etServer = findViewById(R.id.etServer)
        btnConnect = findViewById(R.id.btnConnect)
        btnPtt = findViewById(R.id.btnPtt)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        etServer.setText(prefs.getString(KEY_SERVER, DEFAULT_SERVER))

        btnConnect.setOnClickListener {
            if (ws == null) connect() else disconnect()
        }

        btnPtt.setOnTouchListener { _, e ->
            when (e.action) {
                MotionEvent.ACTION_DOWN -> {
                    startTalking()
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    stopTalking()
                    true
                }
                else -> false
            }
        }

        startPlayer()
    }

    // ---------------- WebSocket ----------------

    private fun connect() {
        val url = etServer.text.toString().trim()
        if (!url.startsWith("ws://") && !url.startsWith("wss://")) {
            toast("地址需以 ws:// 或 wss:// 开头")
            return
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_SERVER, url).apply()

        userClosed = false
        setStatus("连接中…")
        btnConnect.text = "断开"

        val request = Request.Builder().url(url).build()
        ws = http.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: okhttp3.Response) {
                val hello =
                    "{\"type\":\"hello\",\"id\":\"${deviceId()}\",\"kind\":\"android\",\"proto\":1}"
                webSocket.send(hello)
                setStatus("已连接: $url")
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                enqueuePlay(bytes.toByteArray())
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                Log.d(TAG, "文本: $text")
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: okhttp3.Response?) {
                Log.w(TAG, "连接失败", t)
                onLinkDown("连接失败: ${t.message}")
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                onLinkDown("已断开")
            }
        })
    }

    private fun disconnect() {
        userClosed = true
        stopTalking()
        ws?.close(1000, "bye")
        ws = null
        btnConnect.text = "连接服务器"
        setStatus("未连接")
    }

    private fun onLinkDown(msg: String) {
        ws = null
        stopTalking()
        ui.post {
            btnConnect.text = "连接服务器"
            if (!userClosed) {
                setStatus("$msg,5 秒后重连")
                if (!reconnectScheduled) {
                    reconnectScheduled = true
                    ui.postDelayed({ reconnectScheduled = false; connect() }, 5000)
                }
            } else {
                setStatus("未连接")
            }
        }
    }

    private fun setStatus(s: String) = ui.post { tvStatus.text = s }

    private fun toast(s: String) =
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

    // ---------------- PTT 采集 ----------------

    private fun startTalking() {
        if (talking.get()) return
        if (ws == null) {
            toast("请先连接服务器")
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_PERM_RECORD)
            return
        }

        // 半双工:开始讲话时丢弃待播放内容(与固件行为一致)
        synchronized(playLock) { playQueue.clear() }
        talking.set(true)
        btnPtt.text = "正在讲话…"

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        val rec = AudioRecord(
            MediaRecorder.AudioSource.VOICE_COMMUNICATION,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            maxOf(minBuf, FRAME_BYTES * 4)
        )
        record = rec
        rec.startRecording()

        recordThread = Thread {
            val shortBuf = ShortArray(FRAME_SAMPLES)
            val byteBuf = ByteArray(FRAME_BYTES)
            while (talking.get()) {
                val n = rec.read(shortBuf, 0, FRAME_SAMPLES)
                if (n <= 0) continue
                var j = 0
                for (i in 0 until n) {
                    val v = shortBuf[i]
                    byteBuf[j++] = (v.toInt() and 0xFF).toByte()
                    byteBuf[j++] = ((v.toInt() shr 8) and 0xFF).toByte()
                }
                ws?.send(byteBuf.toByteString(0, n * 2))
            }
            try {
                rec.stop()
                rec.release()
            } catch (_: IllegalStateException) {
            }
        }.also { it.start() }
    }

    private fun stopTalking() {
        if (!talking.getAndSet(false)) return
        btnPtt.text = "按住 说话"
        recordThread?.join(500)
        recordThread = null
        record = null
    }

    // ---------------- 播放 ----------------

    private fun startPlayer() {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build()
            )
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setBufferSizeInBytes(maxOf(minBuf, FRAME_BYTES * 4))
            .build()
        track = t
        t.play()

        playerThread = Thread {
            while (!Thread.currentThread().isInterrupted) {
                val chunk: ByteArray? = synchronized(playLock) {
                    if (talking.get() || playQueue.isEmpty()) null else playQueue.removeFirst()
                }
                if (chunk == null) {
                    // 讲话中或无数据:缓冲欠载时 AudioTrack 自动补静音
                    Thread.sleep(10)
                } else {
                    t.write(chunk, 0, chunk.size)
                }
            }
        }.also { it.start() }
    }

    private fun enqueuePlay(data: ByteArray) {
        synchronized(playLock) {
            if (playQueue.size >= MAX_QUEUE_FRAMES) {
                playQueue.removeFirst()  // 满则丢最旧
            }
            playQueue.addLast(data)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        userClosed = true
        stopTalking()
        ws?.close(1000, "bye")
        ws = null
        playerThread?.interrupt()
        track?.release()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_PERM_RECORD) {
            if (grantResults.firstOrNull() == PackageManager.PERMISSION_GRANTED) {
                startTalking()
            } else {
                toast("需要麦克风权限才能讲话")
            }
        }
    }
}
