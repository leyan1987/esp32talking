package com.esp32talking.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.MotionEvent
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.util.concurrent.TimeUnit

data class GroupInfo(val id: Int, val name: String)
data class MemberInfo(val id: String, val name: String, val online: Boolean)

/**
 * esp32talking 安卓客户端 (M2.2) — 界面层。
 *
 * 连接与音频全部在 [TalkService] 前台服务中,灭屏/划掉 App 后仍保持在线。
 * 本类只负责:绑定服务、下发命令(连接/PTT/建群/加群/改名)、渲染状态。
 */
class MainActivity : Activity() {

    companion object {
        private const val TAG = "esp32talking"
        private const val REQ_PERM_RECORD = 1
        private const val REQ_PERM_NOTIF = 2
        private const val PREFS = "cfg"
        private const val KEY_SERVER = "server"
        private const val DEFAULT_SERVER = "ws://192.168.1.100:8000/ws"
    }

    private lateinit var tvStatus: TextView
    private lateinit var tvMyName: TextView
    private lateinit var btnMyName: Button
    private lateinit var btnGain: Button
    private lateinit var etServer: EditText
    private lateinit var btnConnect: Button
    private lateinit var btnPtt: Button
    private lateinit var spGroup: Spinner
    private lateinit var btnCreateGroup: Button
    private lateinit var btnJoinGroup: Button
    private lateinit var btnRenameGroup: Button
    private lateinit var btnMembers: Button

    private val ui = Handler(Looper.getMainLooper())

    private var svc: TalkService? = null
    private var bound = false

    private val groups = mutableListOf<GroupInfo>()
    private var activeGroupId: Int? = null
    private var spinnerBusy = false
    private var myName: String? = null
    private var floorHolder: String? = null
    private var talkingNow = false

    // 成员弹窗(WS 轮询刷新)
    private var membersDialog: AlertDialog? = null
    private var membersContainer: LinearLayout? = null
    private var membersGid: Int? = null

    private val listener = object : TalkService.Listener {
        override fun onStatus(status: String) = runOnUiThread {
            tvStatus.text = status
            btnConnect.text = if (svc?.isConnected() == true) "断开" else "连接服务器"
        }

        override fun onDeviceInfo(name: String) = runOnUiThread {
            myName = name.ifEmpty { null }
            tvMyName.text = if (name.isEmpty()) "昵称: -" else "昵称: $name"
            updatePttButton()
        }

        override fun onGroups(list: List<GroupInfo>, active: Int?) = runOnUiThread {
            updateGroups(list, active)
        }

        override fun onCreated(groupId: Int, name: String) = runOnUiThread {
            toast("已创建「$name」,群号 $groupId,把号码告诉别人即可加入")
        }

        override fun onError(message: String) = runOnUiThread { toast(message) }

        override fun onRejected(reason: String) = runOnUiThread {
            toast("服务器拒绝: $reason")
        }

        override fun onTalkingChanged(talking: Boolean) = runOnUiThread {
            talkingNow = talking
            updatePttButton()
        }

        override fun onFloorChanged(holderName: String?) = runOnUiThread {
            floorHolder = holderName
            updatePttButton()
        }

        override fun onMembers(groupId: Int, members: List<MemberInfo>) = runOnUiThread {
            if (groupId == membersGid && membersDialog?.isShowing == true) {
                renderMembers(members)
            }
        }
    }

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            val s = (binder as TalkService.LocalBinder).service()
            svc = s
            btnGain.text = s.gainLabel()
            s.addListener(listener)
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            svc = null
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        tvStatus = findViewById(R.id.tvStatus)
        tvMyName = findViewById(R.id.tvMyName)
        btnMyName = findViewById(R.id.btnMyName)
        btnGain = findViewById(R.id.btnGain)
        etServer = findViewById(R.id.etServer)
        btnConnect = findViewById(R.id.btnConnect)
        btnPtt = findViewById(R.id.btnPtt)
        spGroup = findViewById(R.id.spGroup)
        btnCreateGroup = findViewById(R.id.btnCreateGroup)
        btnJoinGroup = findViewById(R.id.btnJoinGroup)
        btnRenameGroup = findViewById(R.id.btnRenameGroup)
        btnMembers = findViewById(R.id.btnMembers)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        etServer.setText(prefs.getString(KEY_SERVER, DEFAULT_SERVER))

        bindService(Intent(this, TalkService::class.java), conn, Context.BIND_AUTO_CREATE)
        bound = true

        btnConnect.setOnClickListener {
            val s = svc ?: return@setOnClickListener
            if (s.isConnected()) {
                s.disconnect()
            } else {
                connectToServer()
            }
        }

        spGroup.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                if (spinnerBusy) return
                val g = groups.getOrNull(position) ?: return
                if (g.id != activeGroupId) {
                    activeGroupId = g.id
                    svc?.selectGroup(g.id)
                    toast("当前群组: ${g.name}")
                }
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        btnCreateGroup.setOnClickListener {
            showCreateGroupDialog()
        }
        btnJoinGroup.setOnClickListener {
            showJoinGroupDialog()
        }
        btnRenameGroup.setOnClickListener {
            showRenameDialog()
        }
        btnMembers.setOnClickListener {
            showMembersDialog()
        }
        btnMyName.setOnClickListener {
            showNicknameDialog()
        }
        btnGain.setOnClickListener {
            val idx = svc?.cycleGain() ?: return@setOnClickListener
            btnGain.text = "音量x${idx + 1}"
            toast(if (idx == 0) "已恢复原始音量" else "播放音量增强 x${idx + 1}(在服务里对 PCM 放大)")
        }

        btnPtt.setOnTouchListener { _, e ->
            when (e.action) {
                MotionEvent.ACTION_DOWN -> {
                    startTalking()
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    svc?.stopTalk()
                    true
                }
                else -> false
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        svc?.removeListener(listener)
        if (bound) {
            unbindService(conn)
            bound = false
        }
        // 服务继续在后台运行,不 stopService
    }

    // ---------------- 连接 ----------------

    private fun connectToServer() {
        val url = etServer.text.toString().trim()
        if (!url.startsWith("ws://") && !url.startsWith("wss://")) {
            toast("地址需以 ws:// 或 wss:// 开头")
            return
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(KEY_SERVER, url).apply()

        // Android 13+ 通知权限(拒绝也不影响连接,只是看不到常驻通知)
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_PERM_NOTIF)
        }

        val intent = Intent(this, TalkService::class.java)
            .setAction(TalkService.ACTION_CONNECT)
            .putExtra(TalkService.EXTRA_URL, url)
        startForegroundService(intent)
    }

    // ---------------- PTT ----------------

    private fun startTalking() {
        val s = svc ?: return
        if (s.currentUrl() == null) {
            toast("请先连接服务器")
            return
        }
        if (activeGroupId == null) {
            toast("未加入任何群组,请先新建或加入群组")
            return
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_PERM_RECORD)
            return
        }
        s.startTalk()
    }

    // ---------------- 群组 ----------------

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

    /** PTT 按钮文案:由"我在讲话 / 他人持话权 / 空闲"三种状态驱动 */
    private fun updatePttButton() {
        btnPtt.text = when {
            talkingNow -> "正在讲话…"
            floorHolder != null && floorHolder != myName -> "等候:$floorHolder 讲话中"
            else -> "按住 说话"
        }
    }

    /** 修改自己的昵称 */
    private fun showNicknameDialog() {        if (svc?.isConnected() != true) {
            toast("请先连接服务器")
            return
        }
        val input = EditText(this)
        input.hint = "最多 20 个字"
        val current = tvMyName.text.toString().removePrefix("昵称: ")
        if (current.isNotEmpty() && current != "-") input.setText(current)
        AlertDialog.Builder(this)
            .setTitle("修改我的昵称")
            .setView(input)
            .setPositiveButton("确定") { _, _ ->
                val name = input.text.toString().trim()
                if (name.isNotEmpty()) svc?.setName(name) else toast("昵称不能为空")
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showCreateGroupDialog() {
        if (svc?.isConnected() != true) {
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
                    svc?.createGroup(name)
                } else {
                    toast("群组名不能为空")
                }
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun showJoinGroupDialog() {
        if (svc?.isConnected() != true) {
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
                    svc?.joinGroup(code)
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
                if (newName.isNotEmpty() && newName != g.name) svc?.renameGroup(g.id, newName)
            }
            .setNegativeButton("取消", null)
            .show()
    }

    /** 群组成员在线状态(WS 轮询,每 3 秒刷新,关窗即停) */
    private fun showMembersDialog() {
        val gid = activeGroupId
        if (gid == null) {
            toast("未加入任何群组")
            return
        }
        if (svc?.isConnected() != true) {
            toast("请先连接服务器")
            return
        }
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(16), dp(24), dp(8))
        }
        val dialog = AlertDialog.Builder(this)
            .setTitle("群组成员(在线 ● / 离线 ○)")
            .setView(container)
            .setPositiveButton("关闭", null)
            .create()

        membersGid = gid
        membersContainer = container
        membersDialog = dialog

        val poll = object : Runnable {
            override fun run() {
                if (membersDialog?.isShowing != true) return
                svc?.requestMembers(gid)
                ui.postDelayed(this, 3000)
            }
        }
        dialog.setOnShowListener { poll.run() }
        dialog.setOnDismissListener {
            ui.removeCallbacks(poll)
            membersDialog = null
            membersContainer = null
            membersGid = null
        }
        dialog.show()
    }

    private fun renderMembers(members: List<MemberInfo>) {
        val container = membersContainer ?: return
        container.removeAllViews()
        if (members.isEmpty()) {
            addMemberRow(container, "○", "暂无成员", false)
        } else {
            for (m in members.sortedByDescending { it.online }) {
                addMemberRow(
                    container,
                    if (m.online) "●" else "○",
                    "${m.name}  (${m.id.takeLast(4)})",
                    m.online
                )
            }
        }
    }

    private fun addMemberRow(container: LinearLayout, dot: String, text: String, online: Boolean) {
        val s = SpannableString("$dot $text")
        s.setSpan(
            ForegroundColorSpan(if (online) 0xFF1B9E1B.toInt() else 0xFFAAAAAA.toInt()),
            0, 1, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
        )
        container.addView(
            TextView(this).apply {
                setText(s)
                textSize = 15f
                setPadding(0, dp(8), 0, dp(8))
            }
        )
    }

    // ---------------- 杂项 ----------------

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun toast(s: String) =
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show()

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
