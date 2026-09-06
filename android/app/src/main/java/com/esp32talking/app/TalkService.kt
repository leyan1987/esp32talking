package com.esp32talking.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.net.wifi.WifiManager
import android.os.Binder
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import android.widget.Toast
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONObject
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 对讲机前台服务:持有 WebSocket 长连接与音频线程。
 *
 * 灭屏后安卓会休眠 CPU/WiFi 并冻结后台应用导致掉线,因此:
 * - 以前台服务(常驻通知)运行,划掉 App 也不被杀
 * - 连接期间持有 PARTIAL_WAKE_LOCK + WifiLock,阻止 CPU/WiFi 休眠
 * - START_STICKY:进程被杀后系统会尝试拉起
 */
class TalkService : Service() {

    companion object {
        private const val TAG = "esp32talking"
        private const val CHANNEL_ID = "esp32talking"
        private const val NOTIF_ID = 1
        private const val PREFS = "cfg"
        private const val KEY_SERVER = "server"
        private const val SAMPLE_RATE = 16000
        private const val FRAME_SAMPLES = 320          // 20ms
        private const val FRAME_BYTES = FRAME_SAMPLES * 2
        private const val MAX_QUEUE_FRAMES = 150       // 约 3 秒播放缓冲
        const val ACTION_CONNECT = "com.esp32talking.app.CONNECT"
        const val EXTRA_URL = "url"
    }

    interface Listener {
        fun onStatus(status: String)
        fun onGroups(groups: List<GroupInfo>, activeGroupId: Int?)
        fun onCreated(groupId: Int, name: String)
        fun onError(message: String)
        fun onRejected(reason: String)
        fun onTalkingChanged(talking: Boolean)
    }

    inner class LocalBinder : Binder() {
        fun service(): TalkService = this@TalkService
    }

    private val binder = LocalBinder()
    private val ui = Handler(Looper.getMainLooper())
    private val listeners = CopyOnWriteArrayList<Listener>()

    private val http = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)
        .connectTimeout(5, TimeUnit.SECONDS)
        .build()

    private var ws: WebSocket? = null
    private var serverUrl: String? = null
    private var userClosed = false
    private var reconnectScheduled = false
    private var statusText = "未连接"
    private var foreground = false

    private val groups = mutableListOf<GroupInfo>()
    private var activeGroupId: Int? = null

    // ---- 采集 ----
    private val talking = AtomicBoolean(false)
    private var record: AudioRecord? = null
    private var recordThread: Thread? = null

    // ---- 播放 ----
    private var track: AudioTrack? = null
    private var playerThread: Thread? = null
    private val playQueue = ArrayDeque<ByteArray>()
    private val playLock = Object()

    // ---- 保活锁 ----
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    // ---------------- 生命周期 ----------------

    override fun onCreate() {
        super.onCreate()
        val chan = NotificationChannel(CHANNEL_ID, "对讲机连接", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(chan)
        startPlayer()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val url = intent?.getStringExtra(EXTRA_URL)
        if (intent?.action == ACTION_CONNECT && url != null) {
            connect(url)
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onDestroy() {
        super.onDestroy()
        userClosed = true
        stopTalkingInternal()
        ws?.close(1000, "bye")
        ws = null
        playerThread?.interrupt()
        track?.release()
        releaseLocks()
    }

    // ---------------- 对 UI 的接口 ----------------

    fun addListener(l: Listener) {
        listeners.add(l)
        // 绑定后立即同步当前状态(例如从划掉的界面重新打开)
        ui.post {
            l.onStatus(statusText)
            l.onGroups(groups.toList(), activeGroupId)
            l.onTalkingChanged(talking.get())
        }
    }

    fun removeListener(l: Listener) {
        listeners.remove(l)
    }

    fun isConnected(): Boolean = ws != null

    fun currentUrl(): String? = serverUrl

    fun disconnect() {
        userClosed = true
        stopTalkingInternal()
        ws?.close(1000, "bye")
        ws = null
        releaseLocks()
        stopForegroundAndNotification()
        updateGroups(emptyList(), null)
        setStatus("未连接")
    }

    fun selectGroup(groupId: Int) {
        ws?.send("{\"type\":\"select_group\",\"group_id\":$groupId}")
    }

    fun requestGroups() {
        ws?.send("{\"type\":\"list_groups\"}")
    }

    fun createGroup(name: String) {
        if (ws == null) {
            notifyError("请先连接服务器")
            return
        }
        ws?.send(JSONObject().put("type", "create_group").put("name", name).toString())
    }

    fun joinGroup(code: String) {
        if (ws == null) {
            notifyError("请先连接服务器")
            return
        }
        ws?.send(JSONObject().put("type", "join_group").put("code", code).toString())
    }

    /** 通过 REST 改群组名,成功后刷新群组列表 */
    fun renameGroup(groupId: Int, newName: String) {
        val base = (serverUrl ?: return)
            .replaceFirst("ws://", "http://")
            .replaceFirst("wss://", "https://")
            .removeSuffix("/ws")
        Thread {
            try {
                val body = JSONObject().put("name", newName).toString()
                    .toRequestBody("application/json".toMediaType())
                val req = Request.Builder()
                    .url("$base/api/groups/$groupId")
                    .patch(body)
                    .build()
                http.newCall(req).execute().use { resp ->
                    if (resp.isSuccessful) {
                        requestGroups()
                        ui.post { toast("已重命名") }
                    } else {
                        ui.post { toast("重命名失败: HTTP ${resp.code}") }
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "rename failed", e)
                ui.post { toast("重命名失败: ${e.message}") }
            }
        }.start()
    }

    // ---------------- 连接 ----------------

    private fun connect(url: String) {
        if (ws != null) return
        serverUrl = url
        userClosed = false
        startForegroundNow()
        acquireLocks()
        setStatus("连接中…")

        val request = Request.Builder().url(url).build()
        ws = http.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: okhttp3.Response) {
                val id = deviceId()
                val hello = JSONObject()
                    .put("type", "hello")
                    .put("id", id)
                    .put("kind", "android")
                    .put("proto", 1)
                webSocket.send(hello.toString())
                setStatus("已连接: $url")
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                enqueuePlay(bytes.toByteArray())
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                handleServerText(text)
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

    private fun onLinkDown(msg: String) {
        ws = null
        stopTalkingInternal()
        releaseLocks()
        stopForegroundAndNotification()
        ui.post {
            updateGroups(emptyList(), null)
            if (!userClosed) {
                setStatus("$msg,5 秒后重连")
                if (!reconnectScheduled) {
                    reconnectScheduled = true
                    ui.postDelayed({
                        reconnectScheduled = false
                        serverUrl?.let { connect(it) }
                    }, 5000)
                }
            } else {
                setStatus("未连接")
            }
        }
    }

    private fun handleServerText(text: String) {
        val obj = runCatching { JSONObject(text) }.getOrNull() ?: return
        when (obj.optString("type")) {
            "welcome", "groups" -> {
                val arr = obj.optJSONArray("groups") ?: return
                val list = mutableListOf<GroupInfo>()
                for (i in 0 until arr.length()) {
                    val g = arr.getJSONObject(i)
                    list.add(GroupInfo(g.getInt("id"), g.getString("name")))
                }
                val active =
                    if (obj.has("active_group_id") && !obj.isNull("active_group_id"))
                        obj.getInt("active_group_id") else null
                ui.post { updateGroups(list, active) }
            }
            "created" -> {
                val gid = obj.optInt("group_id")
                val name = obj.optString("name", "")
                listeners.forEach { it.onCreated(gid, name) }
            }
            "error" -> {
                listeners.forEach { it.onError(obj.optString("message", "操作失败")) }
            }
            "rejected" -> {
                listeners.forEach { it.onRejected(obj.optString("reason", "被拒绝")) }
            }
        }
    }

    // ---------------- 群组状态 ----------------

    private fun updateGroups(list: List<GroupInfo>, active: Int?) {
        groups.clear()
        groups.addAll(list)
        activeGroupId = active ?: list.firstOrNull()?.id
        val snapshot = list.toList()
        listeners.forEach { it.onGroups(snapshot, activeGroupId) }
    }

    // ---------------- PTT 采集 ----------------

    /** 返回 false 表示当前不能开始讲话(未连接/无群组/无权限由 Activity 处理) */
    fun startTalk(): Boolean {
        if (talking.get()) return true
        if (ws == null) {
            ui.post { toast("请先连接服务器") }
            return false
        }
        if (activeGroupId == null) {
            ui.post { toast("未加入任何群组,请先新建或加入群组") }
            return false
        }

        // 半双工:开始讲话时丢弃待播放内容
        synchronized(playLock) { playQueue.clear() }
        talking.set(true)
        listeners.forEach { it.onTalkingChanged(true) }

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
        return true
    }

    fun stopTalk() {
        if (!talking.getAndSet(false)) return
        listeners.forEach { it.onTalkingChanged(false) }
        recordThread?.join(500)
        recordThread = null
        record = null
    }

    private fun stopTalkingInternal() {
        if (talking.getAndSet(false)) {
            listeners.forEach { it.onTalkingChanged(false) }
            recordThread?.join(500)
            recordThread = null
            record = null
        }
    }

    // ---------------- 播放 ----------------

    private fun startPlayer() {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        // USAGE_MEDIA 保证走扬声器外放(VOICE_COMMUNICATION 可能路由到听筒导致音量小)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
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

    // ---------------- 保活 / 通知 ----------------

    private fun acquireLocks() {
        if (wakeLock == null) {
            val pm = getSystemService(POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "esp32talking:ws").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
        if (wifiLock == null) {
            val wm = applicationContext.getSystemService(WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION")
            wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "esp32talking:wifi")
                .apply {
                    setReferenceCounted(false)
                    acquire()
                }
        }
    }

    private fun releaseLocks() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    private fun startForegroundNow() {
        val pi = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        val notif = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("esp32talking 对讲机")
            .setContentText(statusText.ifEmpty { "保持连接中" })
            .setSmallIcon(android.R.drawable.ic_btn_speak_now)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
        startForeground(NOTIF_ID, notif)
        foreground = true
    }

    private fun stopForegroundAndNotification() {
        if (foreground) {
            stopForeground(STOP_FOREGROUND_REMOVE)
            foreground = false
        }
    }

    // ---------------- 杂项 ----------------

    private fun deviceId(): String {
        val p = getSharedPreferences(PREFS, MODE_PRIVATE)
        p.getString("device_id", null)?.let { return it }
        val id = java.util.UUID.randomUUID().toString()
        p.edit().putString("device_id", id).apply()
        return id
    }

    private fun setStatus(s: String) {
        statusText = s
        listeners.forEach { it.onStatus(s) }
    }

    private fun notifyError(msg: String) {
        listeners.forEach { it.onError(msg) }
    }

    private fun toast(s: String) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show()
    }
}
