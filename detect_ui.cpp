// detect_ui.cpp - RK3588 电子围栏检测信息软件
// detect_ui.py 的 C++ Qt 移植版(界面/逻辑 1:1)
// 板上编译: sudo apt install -y qtbase5-dev && cd build && make DetectUI
#include "detect_ui.h"

#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

// QProcess 子类: exec 前把子进程放入独立进程组, 停止时可用 killpg 一次杀掉 sudo + Detect
class DetectProcess : public QProcess {
public:
    explicit DetectProcess(QObject* parent = nullptr) : QProcess(parent) {}
protected:
    void setupChildProcess() override {
        ::setpgid(0, 0);
    }
};


// ---- 日志正则(detect_ui.py 原样移植) ----
static const QRegularExpression reFrame(
    R"(\[Frame (\d+)\] (\d+) tracked, (\d+) persons \| zone: (\d+) in / (\d+) out \(now (\d+)\) \| ([\d.]+) fps)");
static const QRegularExpression reInfo(R"(\[INFO f\d+\] raw_dets=(\d+) tracked=(\d+))");
static const QRegularExpression reFp(R"(\[FALSE-POS\] track=\d+ confirmed \(total: (\d+)\))");
static const QRegularExpression reFpz(R"(\[FALSE-POS\] track=\d+ ZONE ENTERED \(zone total: (\d+)\))");
static const QRegularExpression reMiss(R"(\[MISS\] track=\d+ lost \d+ frames \(total: (\d+))");
static const QRegularExpression reVideo(R"(Video:\s*(\d+)x(\d+)\s*@\s*(\d+))");
static const QRegularExpression reStatsFp(R"(\[STATS\] frames=(\d+)\s+confirmed_false_pos=(\d+)\s+zone_false_pos=(\d+))");
static const QRegularExpression reStatsMiss(R"(\[STATS\] frames=(\d+)\s+miss_count=(\d+))");

// ---------------- ClickableLabel ----------------
void ClickableLabel::mousePressEvent(QMouseEvent* ev) {
    emit clicked();
    QLabel::mousePressEvent(ev);
}

// ---------------- DetectUI ----------------
DetectUI::DetectUI(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8("RK3588 电子围栏 – 检测信息软件 (C++ Qt)"));
    resize(1380, 880);

    var["mode"] = "fp+miss";
    var["src"] = "run";
    var["zone"] = "hybrid";

    st["video"] = "";
    st["width"] = "?";
    st["height"] = "?";
    st["vfps"] = "?";
    st["frame"] = "0";
    st["fps"] = "0";
    st["raw"] = "0";
    st["tracked"] = "0";
    st["persons"] = "0";
    st["zone_in"] = "0";
    st["zone_out"] = "0";
    st["zone_now"] = "0";
    st["fp"] = "0";
    st["fpz"] = "0";
    st["miss"] = "0";

    buildUi();
    loadSettings();
    setDefaultVideo();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &DetectUI::poll);
    timer->start(120);
}

void DetectUI::appendLog(const QString& text) {
    log(text);
}

// ---------------- UI ----------------
void DetectUI::buildUi() {
    QWidget* central = new QWidget();
    setCentralWidget(central);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);

    // 单选按钮辅助: 选中后写入 var[key]
    auto radio = [this](const QString& text, bool checked, const QString& key, const QString& val) {
        QRadioButton* rb = new QRadioButton(text);
        rb->setChecked(checked);
        connect(rb, &QRadioButton::toggled, this,
                [this, key, val](bool on) { if (on) var[key] = val; });
        radio_btns[key + "/" + val] = rb;
        return rb;
    };

    // --- 配置行 1: 视频文件 ---
    QHBoxLayout* r1 = new QHBoxLayout();
    r1->addWidget(new QLabel(QString::fromUtf8("视频文件:")));
    var_file = new QLineEdit("");
    var_file->setMinimumWidth(420);
    r1->addWidget(var_file);
    QPushButton* b_pick = new QPushButton(QString::fromUtf8("浏览"));
    connect(b_pick, &QPushButton::clicked, this, [this]() {
        QString p = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择视频"), "/home/orangepi/Videos");
        if (!p.isEmpty()) var_file->setText(p);
    });
    r1->addWidget(b_pick);
    root->addLayout(r1);

    // --- 配置行 2: 模型 / 工作目录 / 阈值 ---
    QHBoxLayout* r2 = new QHBoxLayout();
    r2->addWidget(new QLabel(QString::fromUtf8("模型:")));
    var_model = new QLineEdit("yolov26s_v2_int8.rknn");
    var_model->setMinimumWidth(240);
    r2->addWidget(var_model);
    QPushButton* b_model = new QPushButton(QString::fromUtf8("浏览"));
    connect(b_model, &QPushButton::clicked, this, [this]() {
        QString mdir = QDir::cleanPath(QDir(var_cwd->text()).filePath("../models"));
        if (!QDir(mdir).exists()) mdir = QString("/home/orangepi/rknn_yolov8_bytetrack-main/models");
        QString p = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择模型"), mdir,
                                                 QString::fromUtf8("RKNN 模型 (*.rknn)"));
        if (p.isEmpty()) return;
        // Detect 固定从 ../models/ 加载, 这里只填文件名(不能填绝对路径)
        if (QDir::cleanPath(QFileInfo(p).absolutePath()) != QDir::cleanPath(mdir))
            log(QString::fromUtf8("[模型] 注意: 该模型不在默认 models 目录(%1), 请复制到那里, 否则 Detect 找不到")
                    .arg(mdir));
        var_model->setText(QFileInfo(p).fileName());
        log(QString::fromUtf8("[模型] 已选择: %1").arg(var_model->text()));
    });
    r2->addWidget(b_model);
    r2->addWidget(new QLabel(QString::fromUtf8("工作目录:")));
    var_cwd = new QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build");
    var_cwd->setMinimumWidth(320);
    r2->addWidget(var_cwd);
    r2->addWidget(new QLabel(QString::fromUtf8("检测阈值:")));
    var_conf = new QLineEdit("0.65");
    var_conf->setMaximumWidth(70);
    r2->addWidget(var_conf);
    root->addLayout(r2);

    // --- 配置行 3: 统计模式 / 运行方式 / 控制按钮 ---
    QHBoxLayout* r3 = new QHBoxLayout();
    r3->addWidget(new QLabel(QString::fromUtf8("统计模式:")));
    r3->addWidget(radio(QString::fromUtf8("误检+漏检"), true, "mode", "fp+miss"));
    r3->addWidget(radio(QString::fromUtf8("只误检"), false, "mode", "fp-only"));
    r3->addWidget(radio(QString::fromUtf8("只漏检"), false, "mode", "no-fp"));
    r3->addSpacing(12);
    r3->addWidget(new QLabel(QString::fromUtf8("运行方式:")));
    r3->addWidget(radio(QString::fromUtf8("本软件启动(sudo -n)"), true, "src", "run"));
    r3->addWidget(radio(QString::fromUtf8("监视文件"), false, "src", "monitor"));
    var_sudo = new QCheckBox(QString::fromUtf8("使用sudo"));
    // 默认不勾: Detect 以普通用户跑(NPU 无需 root), 这样"停止"才能杀掉它;
    // 若确实需要 root(如摄像头权限), 勾上后界面本身也要用 sudo 启动
    var_sudo->setChecked(false);
    r3->addWidget(var_sudo);
    r3->addStretch(1);
    btn_start = new QPushButton(QString::fromUtf8("启动"));
    connect(btn_start, &QPushButton::clicked, this, &DetectUI::start);
    r3->addWidget(btn_start);
    btn_stop = new QPushButton(QString::fromUtf8("停止"));
    btn_stop->setEnabled(false);
    connect(btn_stop, &QPushButton::clicked, this, &DetectUI::stop);
    r3->addWidget(btn_stop);
    btn_draw = new QPushButton(QString::fromUtf8("画围栏"));
    connect(btn_draw, &QPushButton::clicked, this, &DetectUI::drawZone);
    r3->addWidget(btn_draw);
    root->addLayout(r3);

    // --- 配置行 4: 日志 / 画面 / 误检目录 ---
    QHBoxLayout* r4 = new QHBoxLayout();
    r4->addWidget(new QLabel(QString::fromUtf8("日志文件:")));
    var_log = new QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/detect.log");
    r4->addWidget(var_log, 1);
    r4->addWidget(new QLabel(QString::fromUtf8("画面文件:")));
    var_frame = new QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/frame.jpg");
    r4->addWidget(var_frame, 1);
    r4->addWidget(new QLabel(QString::fromUtf8("误检目录:")));
    var_fpdir = new QLineEdit("/home/orangepi/rknn_yolov8_bytetrack-main/build/fp_frames");
    r4->addWidget(var_fpdir, 1);
    root->addLayout(r4);

    // --- 配置行 5: 围栏判定 ---
    QHBoxLayout* r5 = new QHBoxLayout();
    r5->addWidget(new QLabel(QString::fromUtf8("围栏判定:")));
    r5->addWidget(radio(QString::fromUtf8("混合(脚点+面积,推荐)"), true, "zone", "hybrid"));
    r5->addWidget(radio(QString::fromUtf8("脚点"), false, "zone", "foot"));
    r5->addWidget(radio(QString::fromUtf8("面积IoB"), false, "zone", "area"));
    r5->addStretch(1);
    root->addLayout(r5);

    // --- 中部: 视频(左) + 信息/日志(右) ---
    QHBoxLayout* mid = new QHBoxLayout();
    mid->setSpacing(8);

    QWidget* left = new QWidget();
    QVBoxLayout* left_layout = new QVBoxLayout(left);
    left_layout->setSpacing(6);

    QGroupBox* vf = new QGroupBox(QString::fromUtf8("视频画面"));
    QVBoxLayout* vv = new QVBoxLayout(vf);
    lbl_video = new QLabel(QString::fromUtf8("等待画面..."));
    lbl_video->setFixedSize(640, 360);
    lbl_video->setAlignment(Qt::AlignCenter);
    lbl_video->setStyleSheet(QString::fromUtf8("background:#101010; color:#cccccc;"));
    vv->addWidget(lbl_video);
    left_layout->addWidget(vf);

    QGroupBox* fpbox = new QGroupBox(QString::fromUtf8("误检帧"));
    QVBoxLayout* fpl = new QVBoxLayout(fpbox);
    fp_scroll = new QScrollArea();
    fp_scroll->setWidgetResizable(true);
    fp_scroll->setFixedHeight(150);
    fp_container = new QWidget();
    fp_hbox = new QHBoxLayout(fp_container);
    fp_hbox->setAlignment(Qt::AlignLeft);
    fp_hbox->setSpacing(6);
    fp_scroll->setWidget(fp_container);
    fpl->addWidget(fp_scroll);
    left_layout->addWidget(fpbox);

    mid->addWidget(left, 0);

    QVBoxLayout* right = new QVBoxLayout();
    right->setSpacing(6);

    QGroupBox* box = new QGroupBox(QString::fromUtf8("运行信息"));
    QGridLayout* g = new QGridLayout(box);
    struct Field { const char* key; const char* title; };
    Field fields[] = {
        {"video", "视频"}, {"size", "分辨率/帧率"}, {"frame", "帧号"}, {"fps", "处理FPS"},
        {"det", "检测/跟踪"}, {"zone", "围栏进出"}, {"fp", "误检"}, {"miss", "漏检"}
    };
    for (int i = 0; i < 8; ++i) {
        int r = i / 2, c = i % 2;
        g->addWidget(new QLabel(QString::fromUtf8(fields[i].title) + ":"), r, c * 2);
        QLabel* v = new QLabel("-");
        v->setStyleSheet(QString::fromUtf8("color:#0066cc; font-weight:bold; font-size:13px;"));
        g->addWidget(v, r, c * 2 + 1);
        lbl[QString::fromUtf8(fields[i].key)] = v;
    }
    g->setColumnStretch(1, 1);
    g->setColumnStretch(3, 1);
    right->addWidget(box);

    QGroupBox* lf = new QGroupBox(QString::fromUtf8("控制台输出"));
    QVBoxLayout* ll = new QVBoxLayout(lf);
    logEdit = new QPlainTextEdit();
    logEdit->setReadOnly(true);
    logEdit->setMaximumBlockCount(600);
    logEdit->setFont(QFont("DejaVu Sans Mono", 9));
    ll->addWidget(logEdit);
    right->addWidget(lf, 1);

    mid->addLayout(right, 1);
    root->addLayout(mid, 1);

    statusBar()->showMessage("");
}

QStringList DetectUI::listVideos() const {
    QStringList exts;
    exts << ".mp4" << ".avi" << ".mkv" << ".mov" << ".m4v" << ".mpg";
    QDir d("/home/orangepi/Videos");
    QStringList out;
    if (d.exists()) {
        const QFileInfoList infos = d.entryInfoList(QDir::Files);
        for (const QFileInfo& fi : infos) {
            QString suf = "." + fi.suffix().toLower();
            if (exts.contains(suf)) out << fi.fileName();
        }
    }
    out.sort();
    return out;
}

void DetectUI::setDefaultVideo() {
    if (!var_file->text().isEmpty()) return;   // 已有上次设置的视频, 不覆盖
    QStringList vids = listVideos();
    if (!vids.isEmpty()) {
        var_file->setText("/home/orangepi/Videos/" + vids.first());
        log(QString::fromUtf8("默认视频(第一个): ") + vids.first());
    } else {
        var_file->setText("");
        log(QString::fromUtf8("未在 /home/orangepi/Videos 找到视频文件, 请手动选择"));
    }
}

QString DetectUI::zonePath() const {
    return QDir::cleanPath(QDir(var_cwd->text()).filePath("../models/zone.txt"));
}

QStringList DetectUI::cmd() const {
    QStringList c;
    if (var_sudo->isChecked()) c << "sudo" << "-n" << "./Detect";
    else c << "./Detect";
    c << "--no-display" << "--gui-frame" << var_frame->text()
      << "--file" << var_file->text() << "--model" << var_model->text();
    QString m = var["mode"];
    if (m == "no-fp") c << "--no-fp";
    else if (m == "fp-only") c << "--fp-only";
    c << "--zone-mode" << var["zone"] << "--fp-save" << var_fpdir->text()
      << "--conf" << var_conf->text();
    return c;
}

QStringList DetectUI::drawCmd() const {
    QStringList c;
    c << "./Detect" << "--draw-zone-only" << "--file" << var_file->text();
    return c;
}

// ---------------- 控制 ----------------
void DetectUI::start() {
    stop();
    // 勾选"使用sudo"但界面不是 root 时: Detect 会以 root 运行, 普通用户界面杀不掉它 → 自动取消并提示
    if (var_sudo->isChecked() && ::geteuid() != 0) {
        var_sudo->setChecked(false);
        log(QString::fromUtf8("[警告] 勾选了「使用sudo」但界面非 root 启动: Detect 会以 root 运行, 停止按钮将失效。"
                              "已自动取消勾选, 以普通用户运行 Detect(NPU 无需 root)。如需 root 请用 sudo ./DetectUI 启动界面。"));
    }
    saveSettings();   // 记录本次使用的模型/阈值等, 下次打开自动恢复
    clearStats();
    t_logged.clear();
    t_click = QDateTime::currentMSecsSinceEpoch();
    tlog(QString::fromUtf8("点击启动"));
    if (var["src"] == "monitor") {
        startMonitor();
        return;
    }
    prepareFpDir();
    QString zpath = zonePath();
    if (QFileInfo::exists(zpath)) {
        log(QString::fromUtf8("使用已有围栏: ") + zpath);
        launch(cmd(), "detect");
    } else {
        log(QString::fromUtf8("未找到围栏文件: ") + zpath);
        log(QString::fromUtf8("先画电子围栏: 在弹出来的窗口里点4个以上点, 回车保存后自动开始检测(ESC=跳过)"));
        pending_run_after_draw = true;
        launch(drawCmd(), "draw");
    }
}

void DetectUI::stop() {
    if (proc && proc->state() != QProcess::NotRunning) {
        qint64 pid = proc->processId();
        if (pid > 0) {
            if (::kill(-(pid_t)pid, SIGTERM) != 0)
                log(QString::fromUtf8("[停止] 停止信号发送失败(errno=%1): Detect 可能以 root 运行而界面是普通用户, "
                                      "请用 sudo ./DetectUI 启动界面或取消勾选「使用sudo」")
                        .arg(errno));
        }
        // 不阻塞 UI: 由 finished 信号收尾; 3 秒仍未退出则强杀
        QProcess* p = proc;
        QTimer::singleShot(3000, this, [this, p]() {
            if (proc == p && p->state() != QProcess::NotRunning)
                p->kill();
        });
    }
    if (mon_fh) {
        mon_fh->close();
        delete mon_fh;
        mon_fh = nullptr;
    }
    btn_start->setEnabled(true);
    btn_stop->setEnabled(false);
    pending_run_after_draw = false;
    proc_kind.clear();
}

void DetectUI::drawZone() {
    stop();
    pending_run_after_draw = false;
    prepareFpDir();   // 清空误检帧栏和旧文件
    log(QString::fromUtf8("进入画围栏模式: 在弹出来的窗口里点4个以上点, 回车保存 / ESC跳过"));
    launch(drawCmd(), "draw");
}

void DetectUI::startMonitor() {
    mon_path = var_log->text();
    mon_fh = new QFile(mon_path);
    if (!mon_fh->open(QIODevice::ReadOnly | QIODevice::Text)) {
        log(QString::fromUtf8("监视: 无法打开 ") + mon_path + QString::fromUtf8(", 文件出现后会自动读取"));
        delete mon_fh;
        mon_fh = nullptr;
    }
    st["video"] = QFileInfo(var_file->text()).fileName();
    btn_start->setEnabled(false);
    btn_stop->setEnabled(true);
    statusBar()->showMessage(QString::fromUtf8("监视中: ") + mon_path + QString::fromUtf8(" (Detect 请手动启动)"));
}

void DetectUI::launch(const QStringList& cmdline, const QString& kind) {
    st["video"] = QFileInfo(var_file->text()).fileName();
    log("$ " + cmdline.join(" "));
    proc = new DetectProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(var_cwd->text());
    connect(proc, &QProcess::readyReadStandardOutput, this, &DetectUI::onReadyRead);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &DetectUI::onProcessFinished);
    connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && proc)
            log(QString::fromUtf8("启动失败: ") + proc->errorString());
    });
    out_buf.clear();
    proc->start(cmdline.first(), cmdline.mid(1));
    proc_kind = kind;
    btn_start->setEnabled(false);
    btn_stop->setEnabled(true);
    statusBar()->showMessage(QString::fromUtf8("%1 (PID %2) — 按“停止”可结束")
                                 .arg(kind).arg(proc->processId()));
}

// ---------------- 事件 ----------------
void DetectUI::onReadyRead() {
    if (!proc) return;
    out_buf += proc->readAllStandardOutput();
    int idx;
    while ((idx = out_buf.indexOf('\n')) >= 0) {
        QByteArray line = out_buf.left(idx);
        out_buf.remove(0, idx + 1);
        handleLine(QString::fromUtf8(line).trimmed());
    }
}

void DetectUI::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    QProcess* p = qobject_cast<QProcess*>(sender());
    if (!p) return;
    log(QString::fromUtf8("=== 进程已退出(exit=%1) ===").arg(exitCode));
    if (proc != p) {   // 旧进程(已被新进程替换或停止), 只回收不碰界面
        p->deleteLater();
        return;
    }
    proc = nullptr;
    QString kind = proc_kind;
    proc_kind.clear();
    statusBar()->showMessage(QString::fromUtf8("已结束(退出码 %1)").arg(exitCode));
    btn_start->setEnabled(true);
    btn_stop->setEnabled(false);
    p->deleteLater();
    if (kind == "draw") {
        onDrawDone();
    } else if (sudo_fail) {
        sudo_fail = false;
        QMessageBox::warning(this, QString::fromUtf8("需要免密sudo"),
            QString::fromUtf8(
                "sudo 需要密码导致启动失败。\n\n"
                "办法1: 配置免密sudo —— 终端执行\n"
                "  sudo visudo -f /etc/sudoers.d/detect\n"
                "添加一行(用户名按实际改):\n"
                "  orangepi ALL=(root) NOPASSWD: /home/orangepi/rknn_yolov8_bytetrack-main/build/Detect\n\n"
                "办法2: 取消勾选“使用sudo”直接运行(若板子NPU无需root)。\n"
                "办法3: 用“监视文件”方式, 手动启动Detect(见文件头注释)。"));
    }
}

void DetectUI::poll() {
    // 监视文件模式: 增量读日志
    if (mon_fh) {
        while (mon_fh->canReadLine()) {
            QByteArray line = mon_fh->readLine();
            handleLine(QString::fromUtf8(line).trimmed());
        }
    }
    updateVideo();
    updateFpGallery();
    refresh();
}

// ---------------- 解析/刷新 ----------------
void DetectUI::handleLine(const QString& line) {
    log(line);
    if (line.contains(QString::fromUtf8("password is required"))) sudo_fail = true;

    QRegularExpressionMatch m = reVideo.match(line);
    if (m.hasMatch()) {
        st["width"] = m.captured(1);
        st["height"] = m.captured(2);
        st["vfps"] = m.captured(3);
        tlog(QString::fromUtf8("收到视频信息 %1x%2@%3")
                 .arg(st["width"], st["height"], st["vfps"]), true);
    }
    m = reFrame.match(line);
    if (m.hasMatch()) {
        st["frame"] = m.captured(1);
        st["tracked"] = m.captured(2);
        st["persons"] = m.captured(3);
        st["zone_in"] = m.captured(4);
        st["zone_out"] = m.captured(5);
        st["zone_now"] = m.captured(6);
        st["fps"] = m.captured(7);
    }
    m = reInfo.match(line);
    if (m.hasMatch()) {
        st["raw"] = m.captured(1);
        st["tracked"] = m.captured(2);
    }
    m = reFp.match(line);
    if (m.hasMatch()) st["fp"] = m.captured(1);
    m = reFpz.match(line);
    if (m.hasMatch()) st["fpz"] = m.captured(1);
    m = reMiss.match(line);
    if (m.hasMatch()) st["miss"] = m.captured(1);
    m = reStatsFp.match(line);
    if (m.hasMatch()) {
        st["frame"] = m.captured(1);
        st["fp"] = m.captured(2);
        st["fpz"] = m.captured(3);
    }
    m = reStatsMiss.match(line);
    if (m.hasMatch()) {
        st["frame"] = m.captured(1);
        st["miss"] = m.captured(2);
    }
}

void DetectUI::refresh() {
    if (lbl.contains("video")) lbl["video"]->setText(st["video"]);
    if (lbl.contains("size"))
        lbl["size"]->setText(QString("%1x%2 @ %3 fps")
                                 .arg(st["width"], st["height"], st["vfps"]));
    if (lbl.contains("frame")) lbl["frame"]->setText(st["frame"]);
    if (lbl.contains("fps")) lbl["fps"]->setText(QString("%1").arg(st["fps"].toDouble(), 0, 'f', 1));
    if (lbl.contains("det"))
        lbl["det"]->setText(QString::fromUtf8("raw %1 | tracked %2 | 人数 %3")
                                .arg(st["raw"], st["tracked"], st["persons"]));
    if (lbl.contains("zone"))
        lbl["zone"]->setText(QString::fromUtf8("进 %1 | 出 %2 | 在场 %3")
                                 .arg(st["zone_in"], st["zone_out"], st["zone_now"]));
    int frame = st["frame"].toInt();
    double fpr = frame ? 100.0 * st["fp"].toInt() / frame : 0.0;
    double mr = frame ? 100.0 * st["miss"].toInt() / frame : 0.0;
    if (lbl.contains("fp")) {
        QString t = QString::fromUtf8("确认 %1 (围栏 %2)  %3")
                        .arg(st["fp"], st["fpz"]).arg(fpr, 0, 'f', 2);
        t += "%";
        lbl["fp"]->setText(t);
    }
    if (lbl.contains("miss")) {
        QString t = QString::fromUtf8("%1 次  %2").arg(st["miss"]).arg(mr, 0, 'f', 2);
        t += "%";
        lbl["miss"]->setText(t);
    }
}

void DetectUI::clearStats() {
    st["frame"] = "0";
    st["fps"] = "0";
    st["raw"] = "0";
    st["tracked"] = "0";
    st["persons"] = "0";
    st["zone_in"] = "0";
    st["zone_out"] = "0";
    st["zone_now"] = "0";
    st["fp"] = "0";
    st["fpz"] = "0";
    st["miss"] = "0";
    st["width"] = "?";
    st["height"] = "?";
    st["vfps"] = "?";
    refresh();
}

void DetectUI::updateVideo() {
    QString fp = var_frame->text();
    if (fp.isEmpty()) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_video_t < 250) return;   // 限速: 最多 ~4 fps
    QFileInfo fi(fp);
    if (!fi.exists()) return;
    qint64 mt = fi.lastModified().toMSecsSinceEpoch();



    qint64 sz = fi.size();



    // èæµååå¥é¢çéä½, åä¸ç§åå¯è½å¤æ¬¡åå¥(ä¿®æ¹æ¶é´ç²åº¦ä¸º 1 ç§), æä»¥å æä»¶å¤§å°ä½ç¬¬äºæ¡ä»¶,



    // å¦åæ¶é´ç¸åæ¶ GUI è®¤ä¸ºç»é¢æªæ´æ° -> ç»é¢å¡ä½åçªè·³(âç¯çä¹±çªâ)



    if (mt == frame_mt && sz == frame_sz) return;



    frame_mt = mt;



    frame_sz = sz;

    last_video_t = now;
    QPixmap pm(fp);
    if (!pm.isNull()) {
        pm = pm.scaled(640, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        lbl_video->setPixmap(pm);
        tlog(QString::fromUtf8("画面已显示"), true);
    }
}

void DetectUI::updateFpGallery() {
    QString d = var_fpdir->text();
    if (d.isEmpty()) return;
    QDir dir(d);
    if (!dir.exists()) return;
    QStringList files = dir.entryList(QStringList("fp_*.jpg"), QDir::Files, QDir::Name);
    for (const QString& f : files) {
        QString full = dir.filePath(f);
        if (fp_shown.contains(full)) continue;
        fp_shown.insert(full);
        QPixmap pm(full);
        if (pm.isNull()) continue;
        pm = pm.scaled(200, 112, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        ClickableLabel* lbl = new ClickableLabel();
        lbl->setPixmap(pm);
        lbl->setFixedSize(200, 112);
        lbl->setToolTip(f + QString::fromUtf8("（点击放大）"));
        lbl->setCursor(Qt::PointingHandCursor);
        connect(lbl, &ClickableLabel::clicked, this,
                [this, full]() { showFpImage(full); });
        fp_hbox->addWidget(lbl);
    }
}

void DetectUI::showFpImage(const QString& path) {
    QPixmap pm(path);
    if (pm.isNull()) return;
    QDialog dlg(this);
    dlg.setWindowTitle(QFileInfo(path).fileName());
    QVBoxLayout* lay = new QVBoxLayout(&dlg);
    QLabel* lbl = new QLabel();
    lbl->setAlignment(Qt::AlignCenter);
    QSize s = pm.size();
    int maxw = 1100, maxh = 720;
    if (s.width() > maxw || s.height() > maxh)
        pm = pm.scaled(maxw, maxh, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    lbl->setPixmap(pm);
    lay->addWidget(lbl);
    dlg.exec();
}

void DetectUI::onDrawDone() {
    QString zpath = zonePath();
    if (pending_run_after_draw) {
        pending_run_after_draw = false;
        if (QFileInfo::exists(zpath)) {
            log(QString::fromUtf8("围栏已保存: ") + zpath + QString::fromUtf8(", 自动开始检测..."));
            launch(cmd(), "detect");
        } else {
            log(QString::fromUtf8("未生成围栏文件, 已取消检测(画围栏窗口按ESC=跳过)"));
        }
    } else {
        log(QString::fromUtf8("画围栏完成: ") +
            QString(QFileInfo::exists(zpath) ? QString::fromUtf8("已保存 ") + zpath
                                             : QString::fromUtf8("未保存(ESC跳过)")));
    }
}

void DetectUI::prepareFpDir() {
    QString d = var_fpdir->text();
    if (d.isEmpty()) return;
    if (!QDir().mkpath(d)) {
        log(QString::fromUtf8("误检目录准备失败: mkpath ") + d);
        return;
    }
    QStringList olds = QDir(d).entryList(QStringList("fp_*.jpg"), QDir::Files);
    int fail = 0;
    for (const QString& f : olds)
        if (!QFile::remove(QDir(d).filePath(f))) fail++;
    if (fail > 0)
        log(QString::fromUtf8("误检帧目录: %1 个文件无法删除(权限?), 可执行 sudo chown orangepi:orangepi %2")
                .arg(fail).arg(d));
    // 清空界面上的旧缩略图
    while (fp_hbox->count()) {
        QLayoutItem* item = fp_hbox->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    fp_shown.clear();
    log(QString::fromUtf8("误检帧目录已清空并准备: ") + d);
}

void DetectUI::log(const QString& text) {
    logEdit->appendPlainText(text);
    QScrollBar* sb = logEdit->verticalScrollBar();
    sb->setValue(sb->maximum());
    // 无头调试: 设置 DETECTUI_TIMELOG=1 时把日志镜像到 stderr,
    // 便于 SSH 无显示环境抓完整时间线(启动耗时/点击/视频信息/画面已显示/首帧)
    if (qgetenv("DETECTUI_TIMELOG") == "1") {
        fprintf(stderr, "%s\n", text.toUtf8().constData());
        fflush(stderr);
    }
}

void DetectUI::tlog(const QString& tag, bool once) {
    if (t_click <= 0) return;   // 尚未点击启动, 不计时
    if (once && t_logged.contains(tag)) return;
    t_logged.insert(tag);
    double dt = (QDateTime::currentMSecsSinceEpoch() - t_click) / 1000.0;
    log(QString::fromUtf8("[计时] %1 (+%2s)").arg(tag).arg(dt, 0, 'f', 2));
}

void DetectUI::autoStart() {
    start();   // 等价于用户点击"启动"按钮(zone.txt 已存在则直接启动 Detect)
}

// ---------------- 设置持久化 ----------------
QString DetectUI::settingsPath() const {
    return QCoreApplication::applicationDirPath() + "/detectui.ini";
}

void DetectUI::loadSettings() {
    QSettings s(settingsPath(), QSettings::IniFormat);
    var_cwd->setText(s.value("cwd", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build")).toString());
    QString savedModel = s.value("model", QString("yolov26s_v2_int8.rknn")).toString();
    var_model->setText(QFileInfo(savedModel).fileName());   // Detect 只认 ../models/ 下的文件名
    var_conf->setText(s.value("conf", QString("0.65")).toString());
    var_file->setText(s.value("video", QString("")).toString());
    var_log->setText(s.value("log", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/detect.log")).toString());
    var_frame->setText(s.value("frame", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/frame.jpg")).toString());
    var_fpdir->setText(s.value("fpdir", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/fp_frames")).toString());
    var_sudo->setChecked(s.value("sudo", false).toBool());
    QString mode = s.value("mode", QString("fp+miss")).toString();
    QString src  = s.value("src",  QString("run")).toString();
    QString zone = s.value("zone", QString("hybrid")).toString();
    QRadioButton* rb = nullptr;
    if ((rb = radio_btns.value("mode/" + mode))) rb->setChecked(true);
    if ((rb = radio_btns.value("src/" + src)))   rb->setChecked(true);
    if ((rb = radio_btns.value("zone/" + zone))) rb->setChecked(true);
    log(QString::fromUtf8("[设置] 已加载上次设置: 模型=%1  阈值=%2  视频=%3")
            .arg(var_model->text(), var_conf->text(), var_file->text()));
}

void DetectUI::saveSettings() {
    QSettings s(settingsPath(), QSettings::IniFormat);
    s.setValue("video", var_file->text());
    s.setValue("model", var_model->text());
    s.setValue("cwd", var_cwd->text());
    s.setValue("conf", var_conf->text());
    s.setValue("log", var_log->text());
    s.setValue("frame", var_frame->text());
    s.setValue("fpdir", var_fpdir->text());
    s.setValue("mode", var["mode"]);
    s.setValue("src", var["src"]);
    s.setValue("zone", var["zone"]);
    s.setValue("sudo", var_sudo->isChecked());
    s.sync();
    log(QString::fromUtf8("[设置] 已保存: 模型=%1  阈值=%2  模式=%3")
            .arg(var_model->text(), var_conf->text(), var["mode"]));
}

void DetectUI::closeEvent(QCloseEvent* ev) {
    saveSettings();
    QMainWindow::closeEvent(ev);
}


// ---------------- main ----------------
int main(int argc, char** argv) {
    QElapsedTimer start;
    start.start();
    double t_ctor = 0.0, t_show = 0.0;
    QApplication app(argc, argv);
    double t_app = start.elapsed() / 1000.0;      // [细分] QApplication 构造(X11/字体/GL 初始化)
    bool auto_start = false;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--auto-start") == 0) auto_start = true;
    }
    DetectUI ui;
    t_ctor = start.elapsed() / 1000.0;            // [细分] DetectUI 构造(buildUi/loadSettings/视频扫描)
    ui.show();
    t_show = start.elapsed() / 1000.0;            // [细分] show() 到控件映射完成
    double sec = t_show;
    QString msg = QString::fromUtf8("[启动] 软件界面启动耗时: %1 秒").arg(sec, 0, 'f', 2);
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
    fprintf(stderr, "[细分] QApplication 构造: %.3fs, DetectUI 构造: %.3fs, show(): %.3fs\n",
            t_app, t_ctor - t_app, t_show - t_ctor);
    ui.appendLog(msg);
    // 无头测试: --auto-start 在界面渲染完成后自动点击"启动"(500ms 足够 show 后事件循环就绪)
    if (auto_start) {
        QTimer::singleShot(500, &ui, [&ui]() {
            ui.autoStart();
        });
    }
    return app.exec();
}