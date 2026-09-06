package com.esp32talking.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
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
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONObject
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

private data class GroupInfo(val id: Int, val name: String)

/**
 * esp32talking 安卓客户端 (M2)
 *
 * 协议与 ESP32 一致:16kHz/16bit/单声道 PCM,20ms 一帧(640 字节)二进制帧。
 * - 连接后发送 hello(id=持久化 UUID),服务器回 welcome(设备所在群组列表)
 * - 可加入多个群组,通过下拉框选择"当前群组"进行监听和通讯(select_group)
 * - 群组改名:走服务器 REST 接口,成功后刷新群组列表
 * - 服务器按"当前群组"路由语音;发送方不会听到自己的回声
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
    private lateinit var spGroup: Spinner
    private lateinit var btnCreateGroup: Button
    private lateinit var btnJoinGroup: Button
    private lateinit var btnRenameGroup: Button

    private val ui = Handler(Looper.getMainLooper())
    private val http = OkHttpClient.Builder()
        .pingInterval(15, TimeUnit.SECONDS)   // WebSocket 保活
        .connectTimeout(5, TimeUnit.SECONDS)
        .build()

    private var ws: WebSocket? = null
    private var userClosed = false
    private var reconnectScheduled = false

    private val groups = mutableListOf<GroupInfo>()
    private var activeGroupId: Int? = null
    private var spinnerBusy = false

    // ---- 采集(按住 PTT 期间) ----
    private val talking = AtomicBoolean(false)
    private var record: AudioRecord? = null
    private var recordThread: Thread? = null

    // ---- 播放(常驻) ----
    private var track: AudioTrack? = null
    private var playerThread: Thread? = null
    private val playQueue = ArrayDeque<ByteArray>()
    private val playLock = Object()

    /** 设备唯一 ID(安卓拿不到稳定 MAC,用持久化 UUID 代替) */
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
        spGroup = findViewById(R.id.spGroup)
        btnCreateGroup = findViewById(R.id.btnCreateGroup)
        btnJoinGroup = findViewById(R.id.btnJoinGroup)
        btnRenameGroup = findViewById(R.id.btnRenameGroup)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        etServer.setText(prefs.getString(KEY_SERVER, DEFAULT_SERVER))

        btnConnect.setOnClickListener {
            if (ws == null) connect() else disconnect()
        }

        spGroup.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (spinnerBusy) return
                val g = groups.getOrNull(position) ?: return
                if (g.id != activeGroupId) {
                    activeGroupId = g.id
                    ws?.send("{\"type\":\"select_group\",\"group_id\":${g.id}}")
                    toast("当前群组: ${g.name}")
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        btnCreateGroup.setOnClickListener { showCreateGroupDialog() }
        btnJoinGroup.setOnClickListener { showJoinGroupDialog() }
        btnRenameGroup.setOnClickListener { showRenameDialog() }

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

    private fun disconnect() {
        userClosed = true
        stopTalking()
        ws?.close(1000, "bye")
        ws = null
        btnConnect.text = "连接服务器"
        updateGroups(emptyList(), null)
        setStatus("未连接")
    }

    private fun onLinkDown(msg: String) {
        ws = null
        stopTalking()
        ui.post {
            btnConnect.text = "连接服务器"
            updateGroups(emptyList(), null)
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

    /** 处理服务器下发的文本(JSON)消息 */
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
            "rejected" -> {
                val reason = obj.optString("reason", "被拒绝")
                ui.post { toast("服务器拒绝: $reason") }
            }
            "created" -> {
                // 新建群组成功,告知群号方便分享给其他人
                val gid = obj.optInt("group_id")
                val name = obj.optString("name", "")
                ui.post { toast("已创建「$name」,群号 $gid,把号码告诉别人即可加入") }
            }
            "error" -> {
                ui.post { toast(obj.optString("message", "操作失败")) }
            }
        }
    }

    /** 刷新群组下拉框;active 为服务器指定的当前群组 */
    private fun updateGroups(list: List<GroupInfo>, active: Int?) {
        groups.clear()
        groups.addAll(list)
        spinnerBusy = true
        val names =
            if (groups.isEmpty()) mutableListOf("(未加入群组)")
            else groups.map { "${it.name} (${it.id})" }.toMutableList()
        spGroup.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, names)
        val idx = active?.let { a -> groups.indexOfFirst { it.id == a } }
            ?.takeIf { it >= 0 } ?: 0
        if (groups.isNotEmpty()) spGroup.setSelection(idx, false)
        activeGroupId = groups.getOrNull(idx)?.id
        spinnerBusy = false
    }

    /** 新建群组:创建后本机自动加入并切换为当前群组 */
    private fun showCreateGroupDialog() {
        if (ws == null) {
            toast("请先连接服务器")
            return
        }
        val input = EditText(this)
        input.hint = "群组名称"
        AlertDialog.Builder(this)
            .setTitle("新建群组")
            .setView(input)
            .setPositiveButton("创建") { _, _ ->
                val name = input.text.toString().trim()
                if (name.isNotEmpty()) {
                    ws?.send(JSONObject().put("type", "create_group").put("name", name).toString())
                } else {
                    toast("群组名不能为空")
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 凭 6 位群号加入群组 */
    private fun showJoinGroupDialog() {
        if (ws == null) {
            toast("请先连接服务器")
            return
        }
        val input = EditText(this)
        input.hint = "6 位群号,如 382746"
        input.inputType = android.text.InputType.TYPE_CLASS_NUMBER
        AlertDialog.Builder(this)
            .setTitle("加入群组")
            .setView(input)
            .setPositiveButton("加入") { _, _ ->
                val code = input.text.toString().trim()
                if (code.isNotEmpty() && code.all { it.isDigit() }) {
                    ws?.send(JSONObject().put("type", "join_group").put("code", code).toString())
                } else {
                    toast("请输入数字群号")
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showRenameDialog() {
        val g = groups.getOrNull(spGroup.selectedItemPosition)
        if (g == null) {
            toast("没有可改名的群组")
            return
        }
        val input = EditText(this)
        input.setText(g.name)
        AlertDialog.Builder(this)
            .setTitle("重命名群组「${g.name}」")
            .setView(input)
            .setPositiveButton("确定") { _, _ ->
                val newName = input.text.toString().trim()
                if (newName.isNotEmpty() && newName != g.name) renameGroup(g.id, newName)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 通过 REST 接口改群组名,成功后请求服务器刷新群组列表 */
    private fun renameGroup(groupId: Int, newName: String) {
        val base = etServer.text.toString().trim()
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
                        ws?.send("{\"type\":\"list_groups\"}")
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
        if (activeGroupId == null) {
            toast("未加入任何群组,请先在管理台加入群组")
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
