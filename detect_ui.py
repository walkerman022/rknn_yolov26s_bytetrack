#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RK3588 电子围栏 —— 检测信息显示软件 (Qt: PyQt5 / PySide6 自动适配)
==================================================================
在板子上运行, 两种方式任选:

方式 A: 由本软件启动 Detect (推荐, 需要 passwordless sudo)
  1) 配置免密 sudo (只针对 Detect 这一个命令):
       sudo visudo -f /etc/sudoers.d/detect
     写入一行:
       orangepi ALL=(root) NOPASSWD: /home/orangepi/rknn_yolov8_bytetrack-main/build/Detect
  2) 启动软件:  python3 detect_ui.py
  3) 界面里选 视频/模型/统计模式, 点"启动"。
     软件自动带 --no-display --gui-frame, 视频画面显示在左侧。
     第一次运行(无 zone.txt)会先弹出画围栏窗口, 画完保存后自动开始检测。

方式 B: 监视日志文件 + 画面文件 (零配置)
  1) 另开终端手动启动 Detect (注意带 --gui-frame):
       cd ~/rknn_yolov8_bytetrack-main/build
       sudo XAUTHORITY=$HOME/.Xauthority DISPLAY=localhost:10.0 ./Detect --no-display \
           --gui-frame /home/orangepi/rknn_yolov8_bytetrack-main/build/frame.jpg \
           --fp-save /home/orangepi/rknn_yolov8_bytetrack-main/build/fp_frames \
           --file /home/orangepi/Videos/<你的视频> --model yolov26s_v2_int8.rknn \
           > detect.log 2>&1
  2) 本软件选"监视文件"指向 detect.log / frame.jpg 即可。

Qt 绑定安装 (二选一, 脚本自动识别):
  Ubuntu 20.04 / 板子:   sudo apt install -y python3-pyqt5
  新版 Ubuntu/pip:       pip install PySide6
  (若 JPG 画面不显示:    sudo apt install -y qt5-image-formats-plugins 或 qt6-image-formats-plugins)
"""

import os
import glob
import re
import queue
import signal
import subprocess
import threading
import time

_START_T = time.time()   # 软件启动计时起点(含加载Qt绑定)

# ---- Qt 绑定自动适配: PySide6 优先, 否则 PyQt5 ----
try:
    from PySide6 import QtCore, QtGui, QtWidgets
    QT_API = "PySide6"
except ImportError:
    try:
        from PyQt5 import QtCore, QtGui, QtWidgets
        QT_API = "PyQt5"
    except ImportError:
        raise SystemExit(
            "未安装 Qt 绑定!\n  Ubuntu 20.04: sudo apt install -y python3-pyqt5\n"
            "  新版系统/pip:  pip install PySide6")

Qt = QtCore.Qt
QTimer = QtCore.QTimer
QPixmap = QtGui.QPixmap
QFont = QtGui.QFont
QApplication = QtWidgets.QApplication
QMainWindow = QtWidgets.QMainWindow
QWidget = QtWidgets.QWidget
QLabel = QtWidgets.QLabel
QLineEdit = QtWidgets.QLineEdit
QPushButton = QtWidgets.QPushButton
QRadioButton = QtWidgets.QRadioButton
QCheckBox = QtWidgets.QCheckBox
QButtonGroup = QtWidgets.QButtonGroup
QPlainTextEdit = QtWidgets.QPlainTextEdit
QGridLayout = QtWidgets.QGridLayout
QHBoxLayout = QtWidgets.QHBoxLayout
QVBoxLayout = QtWidgets.QVBoxLayout
QGroupBox = QtWidgets.QGroupBox
QFileDialog = QtWidgets.QFileDialog
QMessageBox = QtWidgets.QMessageBox
QScrollArea = QtWidgets.QScrollArea
QDialog = QtWidgets.QDialog

# 枚举写法差异 (PyQt5 用非限定名, PySide6 用带作用域名)
if QT_API == "PyQt5":
    ALIGN_CENTER = Qt.AlignCenter
    ALIGN_LEFT = Qt.AlignLeft
    IGNORE_ASPECT = Qt.IgnoreAspectRatio
    SMOOTH = Qt.SmoothTransformation
    KEEP_ASPECT = Qt.KeepAspectRatio
    POINT_CURSOR = Qt.PointingHandCursor
else:
    ALIGN_CENTER = Qt.AlignmentFlag.AlignCenter
    ALIGN_LEFT = Qt.AlignmentFlag.AlignLeft
    IGNORE_ASPECT = Qt.AspectRatioMode.IgnoreAspectRatio
    SMOOTH = Qt.TransformationMode.SmoothTransformation
    KEEP_ASPECT = Qt.AspectRatioMode.KeepAspectRatio
    POINT_CURSOR = Qt.CursorShape.PointingHandCursor

try:
    _Signal = QtCore.Signal   # PySide6
except AttributeError:
    _Signal = QtCore.pyqtSignal  # PyQt5


class _ClickableLabel(QLabel):
    clicked = _Signal()

    def mousePressEvent(self, ev):
        self.clicked.emit()
        super().mousePressEvent(ev)


RE_FRAME = re.compile(
    r"\[Frame (\d+)\] (\d+) tracked, (\d+) persons \| zone: (\d+) in / (\d+) out \(now (\d+)\) \| ([\d.]+) fps")
RE_INFO = re.compile(r"\[INFO f\d+\] raw_dets=(\d+) tracked=(\d+)")
RE_FP = re.compile(r"\[FALSE-POS\] track=\d+ confirmed \(total: (\d+)\)")
RE_FPZ = re.compile(r"\[FALSE-POS\] track=\d+ ZONE ENTERED \(zone total: (\d+)\)")
RE_MISS = re.compile(r"\[MISS\] track=\d+ lost \d+ frames \(total: (\d+)")
RE_VIDEO = re.compile(r"Video:\s*(\d+)x(\d+)\s*@\s*(\d+)")
RE_STATS_FP = re.compile(r"\[STATS\] frames=(\d+)\s+confirmed_false_pos=(\d+)\s+zone_false_pos=(\d+)")
RE_STATS_MISS = re.compile(r"\[STATS\] frames=(\d+)\s+miss_count=(\d+)")


class DetectUI(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("RK3588 电子围栏 — 检测信息软件 (%s)" % QT_API)
        self.resize(1380, 880)

        self.proc = None
        self.q = queue.Queue()
        self.mon_fh = None
        self._photo = None
        self._frame_mt = None
        self._sudo_fail = False
        self._last_video_t = 0.0
        self._pending_run_after_draw = False
        self._proc_kind = None
        self._fp_shown = set()
        self._t_click = 0.0
        self._t_logged = set()

        self.st = dict(frame=0, fps=0.0, raw=0, tracked=0, persons=0,
                       zone_in=0, zone_out=0, zone_now=0,
                       fp=0, fpz=0, miss=0,
                       width="?", height="?", vfps="?", video="?")

        self._build_ui()
        self._set_default_video()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self._poll)
        self.timer.start(120)

    # ---------------- UI ----------------
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(8, 8, 8, 8)

        # --- config rows ---
        r1 = QHBoxLayout()
        r1.addWidget(QLabel("视频文件:"))
        self.var_file = QLineEdit("")   # 启动时自动扫描 Videos 目录, 默认选第一个视频
        self.var_file.setMinimumWidth(420)
        r1.addWidget(self.var_file)
        b = QPushButton("浏览")
        b.clicked.connect(self._pick)
        r1.addWidget(b)
        root.addLayout(r1)

        r2 = QHBoxLayout()
        r2.addWidget(QLabel("模型:"))
        self.var_model = QLineEdit("yolov26s_v2_int8.rknn")
        self.var_model.setMinimumWidth(240)
        r2.addWidget(self.var_model)
        r2.addWidget(QLabel("工作目录:"))
        self.var_cwd = QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build")
        self.var_cwd.setMinimumWidth(320)
        r2.addWidget(self.var_cwd)
        r2.addWidget(QLabel("检测阈值:"))
        self.var_conf = QLineEdit("0.65")
        self.var_conf.setMaximumWidth(70)
        r2.addWidget(self.var_conf)
        r2.addWidget(QLabel("抽帧间隔:"))
        self.var_interval = QLineEdit("5")
        self.var_interval.setMaximumWidth(60)
        self.var_interval.setToolTip("每 N 帧视频帧检测 1 次 (默认5, 即每5帧抽1帧送NPU)")
        r2.addWidget(self.var_interval)
        root.addLayout(r2)

        r3 = QHBoxLayout()
        r3.addWidget(QLabel("统计模式:"))
        self.grp_mode = QButtonGroup(self)
        self.var_mode = "fp+miss"
        for txt, val in [("误检+漏检", "fp+miss"), ("只误检", "fp-only"), ("只漏检", "no-fp")]:
            rb = QRadioButton(txt)
            rb.setChecked(val == "fp+miss")
            rb.toggled.connect(lambda chk, v=val: self._on_mode(chk, v))
            self.grp_mode.addButton(rb)
            r3.addWidget(rb)
        r3.addSpacing(12)
        r3.addWidget(QLabel("运行方式:"))
        self.grp_src = QButtonGroup(self)
        self.var_src = "run"
        for txt, val in [("本软件启动(sudo -n)", "run"), ("监视文件", "monitor")]:
            rb = QRadioButton(txt)
            rb.setChecked(val == "run")
            rb.toggled.connect(lambda chk, v=val: self._on_src(chk, v))
            self.grp_src.addButton(rb)
            r3.addWidget(rb)
        self.var_sudo = QCheckBox("使用sudo")
        self.var_sudo.setChecked(True)
        r3.addWidget(self.var_sudo)
        r3.addStretch(1)
        self.btn_start = QPushButton("启动")
        self.btn_start.clicked.connect(self.start)
        r3.addWidget(self.btn_start)
        self.btn_stop = QPushButton("停止")
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop)
        r3.addWidget(self.btn_stop)
        self.btn_draw = QPushButton("画围栏")
        self.btn_draw.clicked.connect(self.draw_zone)
        r3.addWidget(self.btn_draw)
        root.addLayout(r3)

        r4 = QHBoxLayout()
        r4.addWidget(QLabel("日志文件:"))
        self.var_log = QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/detect.log")
        r4.addWidget(self.var_log, 1)
        r4.addWidget(QLabel("画面文件:"))
        self.var_frame = QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/frame.jpg")
        r4.addWidget(self.var_frame, 1)
        r4.addWidget(QLabel("误检目录:"))
        self.var_fpdir = QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/fp_frames")
        r4.addWidget(self.var_fpdir, 1)
        root.addLayout(r4)

        r5 = QHBoxLayout()
        r5.addWidget(QLabel("围栏判定:"))
        self.grp_zone = QButtonGroup(self)
        self.var_zone_mode = "hybrid"
        for txt, val in [("混合(脚点+面积,推荐)", "hybrid"), ("脚点", "foot"), ("面积IoB", "area")]:
            rb = QRadioButton(txt)
            rb.setChecked(val == "hybrid")
            rb.toggled.connect(lambda chk, v=val: self._on_zone(chk, v))
            self.grp_zone.addButton(rb)
            r5.addWidget(rb)
        r5.addStretch(1)
        root.addLayout(r5)


        # --- middle: video (left) + info/log (right) ---
        mid = QHBoxLayout()
        mid.setSpacing(8)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setSpacing(6)
        vf = QGroupBox("视频画面")
        vv = QVBoxLayout(vf)
        self.lbl_video = QLabel("等待画面...")
        self.lbl_video.setFixedSize(640, 360)
        self.lbl_video.setAlignment(ALIGN_CENTER)
        self.lbl_video.setStyleSheet("background:#101010; color:#cccccc;")
        vv.addWidget(self.lbl_video)
        left_layout.addWidget(vf)

        # 误检帧缩略图区: 只误检模式下误检发生时保存并展示, 后续误检追加不覆盖
        fpbox = QGroupBox("误检帧")
        fpl = QVBoxLayout(fpbox)
        self.fp_scroll = QScrollArea()
        self.fp_scroll.setWidgetResizable(True)
        self.fp_scroll.setFixedHeight(150)
        self.fp_container = QWidget()
        self.fp_hbox = QHBoxLayout(self.fp_container)
        self.fp_hbox.setAlignment(ALIGN_LEFT)
        self.fp_hbox.setSpacing(6)
        self.fp_scroll.setWidget(self.fp_container)
        fpl.addWidget(self.fp_scroll)
        left_layout.addWidget(fpbox)

        mid.addWidget(left, 0)

        right = QVBoxLayout()
        right.setSpacing(6)

        box = QGroupBox("运行信息")
        g = QGridLayout(box)
        self.lbl = {}
        fields = [("video", "视频"), ("size", "分辨率/帧率"), ("frame", "帧号"), ("fps", "处理FPS"),
                  ("det", "检测/跟踪"), ("zone", "围栏进出"), ("fp", "误检"), ("miss", "漏检")]
        for i, (k, t) in enumerate(fields):
            r, c = divmod(i, 2)
            g.addWidget(QLabel(t + ":"), r, c * 2)
            v = QLabel("-")
            v.setStyleSheet("color:#0066cc; font-weight:bold; font-size:13px;")
            g.addWidget(v, r, c * 2 + 1)
            self.lbl[k] = v
        g.setColumnStretch(1, 1)
        g.setColumnStretch(3, 1)
        right.addWidget(box)

        lf = QGroupBox("控制台输出")
        ll = QVBoxLayout(lf)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(600)
        self.log.setFont(QFont("DejaVu Sans Mono", 9))
        ll.addWidget(self.log)
        right.addWidget(lf, 1)

        mid.addLayout(right, 1)
        root.addLayout(mid, 1)

        self.statusBar().showMessage("")   # 初始不显示提示文字

    def _on_mode(self, checked, val):
        if checked:
            self.var_mode = val

    def _on_src(self, checked, val):
        if checked:
            self.var_src = val

    def _on_zone(self, checked, val):
        if checked:
            self.var_zone_mode = val

    def _pick(self):
        p, _ = QFileDialog.getOpenFileName(self, "选择视频", "/home/orangepi/Videos")
        if p:
            self.var_file.setText(p)

    def _list_videos(self):
        d = "/home/orangepi/Videos"
        exts = (".mp4", ".avi", ".mkv", ".mov", ".m4v", ".mpg")
        try:
            return sorted(f for f in os.listdir(d) if f.lower().endswith(exts))
        except Exception:
            return []

    def _set_default_video(self):
        vids = self._list_videos()
        if vids:
            self.var_file.setText(os.path.join("/home/orangepi/Videos", vids[0]))
            self._log("默认视频(第一个): %s" % vids[0])
        else:
            self.var_file.setText("")
            self._log("未在 /home/orangepi/Videos 找到视频文件, 请手动选择")

    # ---------------- control ----------------
    def start(self):
        self.stop()
        self._clear_stats()
        self._t_logged = set()
        self._t_click = time.time()
        self._tlog("点击启动")
        if self.var_src == "monitor":
            self._start_monitor()
            return
        self._prepare_fp_dir()
        zpath = self._zone_path()
        if os.path.exists(zpath):
            self._log("使用已有围栏: %s" % zpath)
            self._launch(self._cmd(), "detect")
        else:
            self._log("未找到围栏文件 %s" % zpath)
            self._log("先画电子围栏: 在弹出的窗口里点4个以上点, 回车保存后自动开始检测 (ESC=跳过)")
            self._pending_run_after_draw = True
            self._launch(self._draw_cmd(), "draw")

    def _zone_path(self):
        return os.path.normpath(os.path.join(self.var_cwd.text(), "..", "models", "zone.txt"))

    def _cmd(self):
        base = "./Detect" if not self.var_sudo.isChecked() else "sudo -n ./Detect"
        cmd = base.split() + ["--no-display",
               "--gui-frame", self.var_frame.text(),
               "--file", self.var_file.text(), "--model", self.var_model.text()]
        # 抽帧间隔: 每 N 帧视频帧检测 1 次 (默认5, 传给 Detect --detect-interval)
        iv = self.var_interval.text().strip()
        if iv:
            try:
                n = int(iv)
                if n >= 1:
                    cmd += ["--detect-interval", str(n)]
            except ValueError:
                self._log("抽帧间隔无效, 忽略: %s (须为 >=1 的整数)" % iv)
        m = self.var_mode
        if m == "no-fp":
            cmd.append("--no-fp")
        elif m == "fp-only":
            cmd.append("--fp-only")
        cmd += ["--zone-mode", self.var_zone_mode, "--fp-save", self.var_fpdir.text(),
                "--conf", self.var_conf.text()]
        return cmd

    def _draw_cmd(self):
        # draw-only needs no model/NPU, so no sudo required
        return ["./Detect", "--draw-zone-only", "--file", self.var_file.text()]

    def _launch(self, cmd, kind):
        self.st["video"] = os.path.basename(self.var_file.text())
        self._log("$ " + " ".join(cmd))
        try:
            self.proc = subprocess.Popen(cmd, cwd=self.var_cwd.text(),
                                         stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT,
                                         text=True, bufsize=1,
                                         start_new_session=True)
        except Exception as ex:
            self._log("启动失败: %s" % ex)
            self.statusBar().showMessage("启动失败: %s" % ex)
            return
        self._tlog("C++ 进程已启动")
        self._proc_kind = kind
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.statusBar().showMessage("%s (PID %d) — 按“停止”可结束" % (kind, self.proc.pid))
        threading.Thread(target=self._reader, daemon=True).start()

    def draw_zone(self):
        self.stop()
        self._pending_run_after_draw = False
        self._log("进入画围栏模式: 在弹出的窗口里点4个以上点, 回车保存 / ESC跳过")
        self._launch(self._draw_cmd(), "draw")

    def _start_monitor(self):
        self.mon_path = self.var_log.text()
        try:
            self.mon_fh = open(self.mon_path, "r", errors="replace")
        except OSError as ex:
            self._log("监视: 无法打开 %s (%s), 文件出现后会自动读取" % (self.mon_path, ex))
            self.mon_fh = None
        self.st["video"] = os.path.basename(self.var_file.text())
        self.btn_start.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.statusBar().showMessage("监视中: %s (Detect 请手动启动)" % self.mon_path)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            except Exception:
                pass
            self.proc = None
        if self.mon_fh:
            self.mon_fh.close()
            self.mon_fh = None
        self.btn_start.setEnabled(True)
        self.btn_stop.setEnabled(False)
        self._pending_run_after_draw = False
        self._proc_kind = None

    def _reader(self):
        for line in self.proc.stdout:
            self.q.put(line)
        self.q.put(None)

    # ---------------- parsing ----------------
    def _clear_stats(self):
        self.st.update(frame=0, fps=0.0, raw=0, tracked=0, persons=0,
                       zone_in=0, zone_out=0, zone_now=0,
                       fp=0, fpz=0, miss=0, width="?", height="?", vfps="?")
        self._refresh()

    def _handle(self, line):
        self._log(line.rstrip("\n"))
        if "password is required" in line:
            self._sudo_fail = True
        s = self.st
        m = RE_VIDEO.search(line)
        if m:
            s["width"], s["height"], s["vfps"] = m.group(1), m.group(2), m.group(3)
            self._tlog("收到视频信息 %sx%s@%s" % (m.group(1), m.group(2), m.group(3)), once=True)
        m = RE_FRAME.search(line)
        if m:
            s["frame"] = int(m.group(1))
            s["tracked"] = int(m.group(2))
            s["persons"] = int(m.group(3))
            s["zone_in"] = int(m.group(4))
            s["zone_out"] = int(m.group(5))
            s["zone_now"] = int(m.group(6))
            s["fps"] = float(m.group(7))
        m = RE_INFO.search(line)
        if m:
            s["raw"] = int(m.group(1))
            s["tracked"] = int(m.group(2))
        m = RE_FP.search(line)
        if m:
            s["fp"] = int(m.group(1))
        m = RE_FPZ.search(line)
        if m:
            s["fpz"] = int(m.group(1))
        m = RE_MISS.search(line)
        if m:
            s["miss"] = int(m.group(1))
        m = RE_STATS_FP.search(line)
        if m:
            s["frame"] = int(m.group(1))
            s["fp"] = int(m.group(2))
            s["fpz"] = int(m.group(3))
        m = RE_STATS_MISS.search(line)
        if m:
            s["frame"] = int(m.group(1))
            s["miss"] = int(m.group(2))

    def _refresh(self):
        s = self.st
        self.lbl["video"].setText(s["video"])
        self.lbl["size"].setText("%sx%s @ %s fps" % (s["width"], s["height"], s["vfps"]))
        self.lbl["frame"].setText(str(s["frame"]))
        self.lbl["fps"].setText("%.1f" % s["fps"])
        self.lbl["det"].setText("raw %d | tracked %d | 人数 %d" % (s["raw"], s["tracked"], s["persons"]))
        self.lbl["zone"].setText("进 %d | 出 %d | 在场 %d" % (s["zone_in"], s["zone_out"], s["zone_now"]))
        fpr = 100.0 * s["fp"] / s["frame"] if s["frame"] else 0.0
        mr = 100.0 * s["miss"] / s["frame"] if s["frame"] else 0.0
        self.lbl["fp"].setText("确认 %d (围栏 %d)  %.2f%%" % (s["fp"], s["fpz"], fpr))
        self.lbl["miss"].setText("%d 次  %.2f%%" % (s["miss"], mr))

    def _update_video(self):
        fp = self.var_frame.text()
        if not fp:
            return
        now = time.time()
        if now - self._last_video_t < 0.25:   # throttle: at most ~4 fps
            return
        try:
            mt = os.path.getmtime(fp)
            if mt != self._frame_mt:
                self._frame_mt = mt
                self._last_video_t = now
                pm = QPixmap(fp)
                if not pm.isNull():
                    pm = pm.scaled(640, 360, IGNORE_ASPECT, SMOOTH)
                    self.lbl_video.setPixmap(pm)
                    self._tlog("画面已显示", once=True)
        except Exception:
            pass

    def _poll(self):
        try:
            while True:
                item = self.q.get_nowait()
                if item is None:
                    code = self.proc.poll() if self.proc else None
                    self.statusBar().showMessage("已结束 (退出码 %s)" % code)
                    self.btn_start.setEnabled(True)
                    self.btn_stop.setEnabled(False)
                    self._log("=== 进程已退出 (exit=%s) ===" % code)
                    kind = self._proc_kind
                    self.proc = None
                    self._proc_kind = None
                    if kind == "draw":
                        self._on_draw_done()
                    elif self._sudo_fail:
                        self._sudo_fail = False
                        QMessageBox.warning(self, "需要免密sudo",
                            "sudo 需要密码导致启动失败。\n\n"
                            "办法1: 配置免密sudo —— 终端执行\n"
                            "  sudo visudo -f /etc/sudoers.d/detect\n"
                            "添加一行(用户名按实际改):\n"
                            "  orangepi ALL=(root) NOPASSWD: /home/orangepi/rknn_yolov8_bytetrack-main/build/Detect\n\n"
                            "办法2: 取消勾选“使用sudo”直接运行(若板子NPU无需root)。\n"
                            "办法3: 用“监视文件”方式, 手动启动Detect(见文件头注释)。")
                else:
                    self._handle(item)
        except queue.Empty:
            pass
        if self.mon_fh:
            try:
                while True:
                    line = self.mon_fh.readline()
                    if not line:
                        break
                    self._handle(line)
            except Exception:
                pass
        self._update_video()
        self._update_fp_gallery()
        self._refresh()

    def _tlog(self, tag, once=False):
        if self._t_click <= 0:   # 尚未点击启动, 无计时基准, 不计时(如显示残留画面)
            return
        if once and tag in self._t_logged:
            return
        self._t_logged.add(tag)
        self._log("[计时] %s (+%.2fs)" % (tag, time.time() - self._t_click))

    def _prepare_fp_dir(self):
        d = self.var_fpdir.text()
        if not d:
            return
        try:
            os.makedirs(d, exist_ok=True)
            for f in glob.glob(os.path.join(d, "fp_*.jpg")):
                try:
                    os.remove(f)
                except OSError:
                    pass
        except Exception as e:
            self._log("误检目录准备失败: %s" % e)
        # 清空界面上的旧缩略图
        while self.fp_hbox.count():
            item = self.fp_hbox.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()
        self._fp_shown.clear()
        self._log("误检帧目录已清空并准备: %s" % d)

    def _update_fp_gallery(self):
        d = self.var_fpdir.text()
        if not d or not os.path.isdir(d):
            return
        try:
            files = sorted(glob.glob(os.path.join(d, "fp_*.jpg")))
        except Exception:
            return
        for f in files:
            if f in self._fp_shown:
                continue
            self._fp_shown.add(f)
            pm = QPixmap(f)
            if pm.isNull():
                continue
            pm = pm.scaled(200, 112, IGNORE_ASPECT, SMOOTH)
            lbl = _ClickableLabel()
            lbl.setPixmap(pm)
            lbl.setFixedSize(200, 112)
            lbl.setToolTip(os.path.basename(f) + "（点击放大）")
            lbl.setCursor(POINT_CURSOR)
            lbl.clicked.connect(lambda _=False, p=f: self._show_fp_image(p))
            self.fp_hbox.addWidget(lbl)

    def _show_fp_image(self, path):
        pm = QPixmap(path)
        if pm.isNull():
            return
        dlg = QDialog(self)
        dlg.setWindowTitle(os.path.basename(path))
        lay = QVBoxLayout(dlg)
        lbl = QLabel()
        lbl.setAlignment(ALIGN_CENTER)
        s = pm.size()
        maxw, maxh = 1100, 720
        if s.width() > maxw or s.height() > maxh:
            pm = pm.scaled(maxw, maxh, KEEP_ASPECT, SMOOTH)
        lbl.setPixmap(pm)
        lay.addWidget(lbl)
        dlg.exec_()

    def _on_draw_done(self):
        zpath = self._zone_path()
        if self._pending_run_after_draw:
            self._pending_run_after_draw = False
            if os.path.exists(zpath):
                self._log("围栏已保存: %s，自动开始检测..." % zpath)
                self._launch(self._cmd(), "detect")
            else:
                self._log("未生成围栏文件，已取消检测 (画围栏窗口按ESC=跳过)")
        else:
            self._log("画围栏完成: %s" % ("已保存 " + zpath if os.path.exists(zpath) else "未保存(ESC跳过)"))

    def _log(self, text):
        self.log.appendPlainText(text)
        sb = self.log.verticalScrollBar()
        sb.setValue(sb.maximum())


def main():
    app = QApplication([])
    ui = DetectUI()
    ui.show()
    dt = time.time() - _START_T
    msg = "[启动] 软件界面启动耗时: %.2f 秒" % dt
    print(msg)
    ui._log(msg)
    if hasattr(app, "exec_"):
        app.exec_()
    else:
        app.exec()


if __name__ == "__main__":
    main()