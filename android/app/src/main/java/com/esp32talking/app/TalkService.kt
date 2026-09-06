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
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.AutomaticGainControl
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
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONObject
import java.util.UUID
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
        // 软件播放增益档位(不同 ROM 外放差异大,靠放大 PCM 兜底)
        private val GAIN_STEPS = floatArrayOf(1f, 2f, 3f, 4f)
        private const val KEY_GAIN = "gain_idx"
        // 接收端自动响度:把语音拉到目标 RMS,抵消发送端波动/AGC 造成
        // 的"前几句正常、后来越来越小"
        private const val TARGET_RMS = 1500f      // 约 -27dBFS
        private const val RMS_FLOOR = 300f        // 低于此视为静音,不调整
        private const val AUTO_GAIN_MIN = 0.5f
        private const val AUTO_GAIN_MAX = 4f
        const val ACTION_CONNECT = "com.esp32talking.app.CONNECT"
        const val EXTRA_URL = "url"
    }

    interface Listener {
        fun onStatus(status: String)
        fun onDeviceInfo(name: String)
        fun onGroups(groups: List<GroupInfo>, activeGroupId: Int?)
        fun onCreated(groupId: Int, name: String)
        fun onError(message: String)
        fun onRejected(reason: String)
        fun onTalkingChanged(talking: Boolean)

        /** 当前群组话权变化:holderName 为 null 表示空闲 */
        fun onFloorChanged(holderName: String?)

        /** list_members 查询结果 */
        fun onMembers(groupId: Int, members: List<MemberInfo>)
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
    private var myDeviceName: String? = null

    // ---- 采集 ----
    private val talking = AtomicBoolean(false)
    private var record: AudioRecord? = null
    private var recordThread: Thread? = null
    private val encState = IntArray(2)             // ADPCM 跨帧状态

    // ---- 话权(M3):按键先申请,授权后才开始采集 ----
    @Volatile private var pendingTalk = false
    private var grantTimeout: Runnable? = null

    // ---- 播放 ----
    private var track: AudioTrack? = null
    private var playerThread: Thread? = null
    private val playQueue = ArrayDeque<ByteArray>()
    private val playLock = Object()
    private var gainIdx = 0
    private var autoGain = 1f

    // ---- 保活锁 ----
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    // ---------------- 生命周期 ----------------

    override fun onCreate() {
        super.onCreate()
        val chan = NotificationChannel(CHANNEL_ID, "对讲机连接", NotificationManager.IMPORTANCE_LOW)
        getSystemService(NotificationManager::class.java).createNotificationChannel(chan)
        gainIdx = getSharedPreferences(PREFS, MODE_PRIVATE).getInt(KEY_GAIN, 0)
            .coerceIn(0, GAIN_STEPS.size - 1)
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
            l.onDeviceInfo(myDeviceName ?: "")
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

    /** 修改自己的昵称(设备显示名),成功后服务器回 device_info */
    fun setName(name: String) {
        if (ws == null) {
            notifyError("请先连接服务器")
            return
        }
        ws?.send(JSONObject().put("type", "set_name").put("name", name).toString())
    }

    /** 修改自己所在群组的名称(走 WS,REST 已加管理台鉴权) */
    fun renameGroup(groupId: Int, newName: String) {
        if (ws == null) {
            notifyError("请先连接服务器")
            return
        }
        ws?.send(
            JSONObject()
                .put("type", "rename_group")
                .put("group_id", groupId)
                .put("name", newName)
                .toString()
        )
    }

    /** 查询某群组成员在线状态,结果经 onMembers 回调 */
    fun requestMembers(groupId: Int) {
        if (ws == null) return
        ws?.send(JSONObject().put("type", "list_members").put("group_id", groupId).toString())
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
                obj.optJSONObject("device")?.optString("name")?.let {
                    if (it.isNotEmpty()) myDeviceName = it
                }
                val arr = obj.optJSONArray("groups") ?: return
                val list = mutableListOf<GroupInfo>()
                for (i in 0 until arr.length()) {
                    val g = arr.getJSONObject(i)
                    list.add(GroupInfo(g.getInt("id"), g.getString("name")))
                }
                val active =
                    if (obj.has("active_group_id") && !obj.isNull("active_group_id"))
                        obj.getInt("active_group_id") else null
                val nameSnapshot = myDeviceName
                ui.post {
                    if (nameSnapshot != null) listeners.forEach { it.onDeviceInfo(nameSnapshot) }
                    updateGroups(list, active)
                }
            }
            "device_info" -> {
                myDeviceName = obj.optString("name", "")
                val nameSnapshot = myDeviceName
                ui.post { listeners.forEach { it.onDeviceInfo(nameSnapshot ?: "") } }
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
            "ptt_grant" -> ui.post {
                if (pendingTalk) {
                    cancelPendingTalk()
                    beginRecording()
                } else {
                    // 迟到的授权:退还话权
                    ws?.send("{\"type\":\"ptt_release\"}")
                }
            }
            "ptt_deny" -> {
                cancelPendingTalk()
                val holder = obj.optString("holder_name", "他人")
                listeners.forEach { it.onError("「$holder」正在讲话,请稍后再试") }
            }
            "ptt_revoke" -> ui.post {
                if (talking.get()) {
                    stopRecording()
                    listeners.forEach { it.onError("话权被更高优先级抢占") }
                }
            }
            "ptt_status" -> {
                val held = obj.optBoolean("held", false)
                val holder = if (held) obj.optString("holder_name", "") else null
                listeners.forEach { it.onFloorChanged(holder) }
            }
            "members" -> {
                val gid = obj.optInt("group_id")
                val arr = obj.optJSONArray("members")
                val list = mutableListOf<MemberInfo>()
                for (i in 0 until (arr?.length() ?: 0)) {
                    val m = arr!!.getJSONObject(i)
                    list.add(
                        MemberInfo(
                            m.getString("id"), m.getString("name"),
                            m.optBoolean("online", false)
                        )
                    )
                }
                listeners.forEach { it.onMembers(gid, list) }
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

    // ---------------- PTT 话权 ----------------

    /** PTT 按下:先向服务器申请话权,授权后自动开始采集 */
    fun startTalk() {
        if (talking.get() || pendingTalk) return
        if (ws == null) {
            ui.post { toast("请先连接服务器") }
            return
        }
        if (activeGroupId == null) {
            ui.post { toast("未加入任何群组,请先新建或加入群组") }
            return
        }
        pendingTalk = true
        ws?.send("{\"type\":\"ptt_request\"}")
        grantTimeout = Runnable {
            if (pendingTalk && !talking.get()) {
                cancelPendingTalk()
                notifyError("话权申请超时,请松开重试")
                ws?.send("{\"type\":\"ptt_release\"}")
            }
        }.also { ui.postDelayed(it, 2500) }
    }

    private fun beginRecording() {
        // 半双工:开始讲话时丢弃待播放内容
        synchronized(playLock) { playQueue.clear() }
        talking.set(true)
        listeners.forEach { it.onTalkingChanged(true) }

        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        // 用原始 MIC 源:VOICE_COMMUNICATION 自带 AGC 自动增益,
        // 会把说得久/说得响的后续句子自动压小(实测"前几句正常,后来越来越小")
        val rec = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            maxOf(minBuf, FRAME_BYTES * 4)
        )
        // 设备支持时再明确关闭 AGC/回声消除(半双工对讲无回声问题,不需要它们)
        try {
            if (AutomaticGainControl.isAvailable()) {
                AutomaticGainControl.create(rec.audioSessionId).setEnabled(false)
            }
        } catch (_: Exception) {
        }
        try {
            if (AcousticEchoCanceler.isAvailable()) {
                AcousticEchoCanceler.create(rec.audioSessionId).setEnabled(false)
            }
        } catch (_: Exception) {
        }
        record = rec
        rec.startRecording()

        recordThread = Thread {
            val shortBuf = ShortArray(FRAME_SAMPLES)
            val pcmBytes = ByteArray(FRAME_BYTES)
            val adpcmOut = ByteArray(Adpcm.ADPCM_BYTES)
            while (talking.get()) {
                val n = rec.read(shortBuf, 0, FRAME_SAMPLES)
                if (n <= 0) continue
                if (n == FRAME_SAMPLES) {
                    // M3:IMA ADPCM 压缩后上行(164 字节),服务器原样转发
                    Adpcm.encode(shortBuf, encState, adpcmOut)
                    ws?.send(adpcmOut.toByteString())
                } else {
                    // 采样数不足一帧(罕见):按 PCM 兜底发送
                    var j = 0
                    for (i in 0 until n) {
                        val v = shortBuf[i]
                        pcmBytes[j++] = (v.toInt() and 0xFF).toByte()
                        pcmBytes[j++] = ((v.toInt() shr 8) and 0xFF).toByte()
                    }
                    ws?.send(pcmBytes.toByteString(0, n * 2))
                }
            }
            try {
                rec.stop()
                rec.release()
            } catch (_: IllegalStateException) {
            }
        }.also { it.start() }
    }

    fun stopTalk() {
        cancelPendingTalk()
        if (talking.get()) stopRecording()
        // 无论是否真正持有话权,统一发释放(服务器无匹配时忽略)
        ws?.send("{\"type\":\"ptt_release\"}")
    }

    private fun cancelPendingTalk() {
        pendingTalk = false
        grantTimeout?.let { ui.removeCallbacks(it) }
        grantTimeout = null
    }

    private fun stopRecording() {
        talking.set(false)
        listeners.forEach { it.onTalkingChanged(false) }
        recordThread?.join(500)
        recordThread = null
        record = null
    }

    private fun stopTalkingInternal() {
        cancelPendingTalk()
        if (talking.get()) stopRecording()
    }

    // ---------------- 播放 ----------------

    /** 切换软件增益档位(1x→2x→3x→4x→1x),返回新档位下标 */
    fun cycleGain(): Int {
        gainIdx = (gainIdx + 1) % GAIN_STEPS.size
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(KEY_GAIN, gainIdx).apply()
        return gainIdx
    }

    fun gainIndex(): Int = gainIdx

    fun gainLabel(): String = "音量x${GAIN_STEPS[gainIdx].toInt()}"

    /**
     * 接收端自动响度 + 手动增益:
     * 先按帧 RMS 缓慢调整 autoGain(上调慢防把间隙噪声抬起来,下调快防炸),
     * 再乘上用户手动档位,最后削波保护。
     */
    private fun applyGain(chunk: ByteArray) {
        var sum = 0.0
        var i = 0
        while (i < chunk.size) {
            val lo = chunk[i].toInt() and 0xFF
            val hi = chunk[i + 1].toInt()
            val s = (hi shl 8) or lo
            sum += (s * s).toDouble()
            i += 2
        }
        val n = (chunk.size / 2).coerceAtLeast(1)
        val rms = kotlin.math.sqrt(sum / n).toFloat()
        if (rms > RMS_FLOOR) {
            val desired = (TARGET_RMS / rms).coerceIn(AUTO_GAIN_MIN, AUTO_GAIN_MAX)
            autoGain = if (desired > autoGain) {
                autoGain + (desired - autoGain) * 0.04f
            } else {
                autoGain + (desired - autoGain) * 0.30f
            }
        }
        val g = GAIN_STEPS[gainIdx] * autoGain
        if (g <= 0.99f || g >= 1.01f) {
            var j = 0
            while (j < chunk.size) {
                val lo = chunk[j].toInt() and 0xFF
                val hi = chunk[j + 1].toInt()
                var sample = ((hi shl 8) or lo) * g
                if (sample > 32767f) sample = 32767f
                if (sample < -32768f) sample = -32768f
                val s = sample.toInt()
                chunk[j] = (s and 0xFF).toByte()
                chunk[j + 1] = ((s shr 8) and 0xFF).toByte()
                j += 2
            }
        }
    }

    private fun startPlayer() {
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        // USAGE_MEDIA + CONTENT_TYPE_MUSIC:确保走"媒体音量"通道。
        // 部分 ROM(如 Flyme)对 SPEECH 内容类型走独立音量/路由,导致外放偏小。
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
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
            val decodeOut = ShortArray(Adpcm.FRAME_SAMPLES)
            val decodedBytes = ByteArray(Adpcm.FRAME_BYTES)
            while (!Thread.currentThread().isInterrupted) {
                val chunk: ByteArray? = synchronized(playLock) {
                    if (talking.get() || playQueue.isEmpty()) null else playQueue.removeFirst()
                }
                if (chunk == null) {
                    Thread.sleep(10)
                    continue
                }
                val pcm: ByteArray? = when (chunk.size) {
                    // 按帧长自动识别:M3 = ADPCM,旧客户端 = 裸 PCM
                    Adpcm.ADPCM_BYTES -> {
                        Adpcm.decode(chunk, decodeOut)
                        for (i in 0 until Adpcm.FRAME_SAMPLES) {
                            val v = decodeOut[i].toInt()
                            decodedBytes[2 * i] = (v and 0xFF).toByte()
                            decodedBytes[2 * i + 1] = ((v shr 8) and 0xFF).toByte()
                        }
                        decodedBytes
                    }
                    Adpcm.FRAME_BYTES -> chunk
                    else -> null
                }
                if (pcm != null) {
                    applyGain(pcm)
                    t.write(pcm, 0, pcm.size)
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

    /**
     * 设备唯一 ID。
     * 优先用 ANDROID_ID 派生:同一签名重装 App 后不变(恢复出厂或换签名才会变),
     * 因此重装后服务器仍能认出这台设备,群组关系不丢。
     * ANDROID_ID 不可用时回退到持久化随机 UUID。
     */
    private fun deviceId(): String {
        val p = getSharedPreferences(PREFS, MODE_PRIVATE)
        val aid = android.provider.Settings.Secure.getString(
            contentResolver, android.provider.Settings.Secure.ANDROID_ID
        )
        if (!aid.isNullOrEmpty() && aid != "9774d56d682e549c" && aid.length >= 8) {
            return UUID.nameUUIDFromBytes(aid.toByteArray()).toString()
        }
        p.getString("device_id", null)?.let { return it }
        val id = UUID.randomUUID().toString()
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
