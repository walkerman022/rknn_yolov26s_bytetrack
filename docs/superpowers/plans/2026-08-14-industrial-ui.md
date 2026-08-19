# 电子围栏系统工业界面 (DetectUI2) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增独立 Qt5 程序 `DetectUI2`(暗黑工业风电子围栏界面: 顶部工具栏 + 棋盘格渲染区 + OK/NG 状态卡 + 事件数据表 + 状态栏), 复用现有 `Detect` 二进制与日志协议, 并对 `main_MultiDetect.cpp` 加 3 行可回退的 `[ENTER]` 事件输出。

**Architecture:** 纯 Qt5 Widgets 单文件 UI 程序(`src/industrial_ui.{h,cpp}`), 通过 QProcess 启动现有 `Detect`, 解析 stdout 日志行(复用现有正则), 轮询 `frame.jpg` 显示画面, 设置持久化到 `detectui2.ini`。所有交互通过对话框收敛(系统参数/数据查询等), 主界面严格按参考图布局。现有 `DetectUI`/`Detect` 代码不动, CMake 新增独立目标。

**Tech Stack:** Qt5 Widgets (板上 `sudo apt install -y qtbase5-dev`), C++11, CMake (现有构建体系), 不引入新依赖。

---

### Task 1: Detect 侧新增 [ENTER] 事件输出(可回退)

**Files:**
- Modify: `src/main_MultiDetect.cpp:4313`(zone 判断循环内, `person_in_zone.find` 之前)

- [ ] **Step 1: 加入 [ENTER] 输出**

在 `src/main_MultiDetect.cpp` 第 4313 行(`auto it = person_in_zone.find(tid);`)之前插入:

```cpp
                // 事件: 人物首次出现在画面(未进围栏也算) -> "进入"事件, 一个 id 只记一次
                // person_in_zone 在循环末尾才写入, find 失败即首次出现
                if (person_in_zone.find(tid) == person_in_zone.end())
                    push_event("[ENTER] track=%d\n", tid);
```

位置说明: 该循环位于 `tracklet_len >= zone_confirm_frames` 确认之后, 只有确认的轨迹才发事件, 与围栏进出判定使用同一批 track(避免瞬时误检产生"进入"噪音)。

- [ ] **Step 2: 编译验证**

Run: `cd build && make Detect` (板上) 或 `bash build-linux_RK3588.sh`(交叉)
Expected: 编译通过, 无警告新增。`DetectUI`/`DetectUI2` 不受影响(未改动其源码)。

- [ ] **Step 3: 手动验证输出格式**

用一段有人的调试视频运行 `./Detect --no-display --file xxx.mp4`, 观察 stdout:
Expected: 出现 `[ENTER] track=N` 且每个 track 只出现一次; 之后围栏进出仍输出 `[Zone] ... ENTERED/EXITED`。

- [ ] **Step 4: Commit**

```bash
git add src/main_MultiDetect.cpp
git commit -m "feat: add [ENTER] event output for person first appearance (DetectUI2 事件协议)"
```

---

### Task 2: 新建 industrial_ui.h 头文件

**Files:**
- Create: `src/industrial_ui.h`

- [ ] **Step 1: 写入头文件**

```cpp
#ifndef INDUSTRIAL_UI_H
#define INDUSTRIAL_UI_H

#include <QMainWindow>
#include <QMap>
#include <QSet>

class QLabel;
class QLineEdit;
class QProcess;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QTableWidget;
class QTimer;
class QFile;

// 可点击的图片标签(误检帧缩略图, 点击放大)
class ClickableLabel2 : public QLabel {
    Q_OBJECT
public:
    explicit ClickableLabel2(QWidget* parent = nullptr) : QLabel(parent) {}
signals:
    void clicked();
protected:
    void mousePressEvent(QMouseEvent* ev) override;
};

// RK3588 电子围栏 - 工业暗黑风界面 (DetectUI2)
// 布局按 ui/电子围栏系统界面设计.md 参考图, 逻辑复用现有 Detect 日志协议
class IndustrialUI : public QMainWindow {
    Q_OBJECT
public:
    explicit IndustrialUI(QWidget* parent = nullptr);

private:
    // ---- UI 控件 ----
    QLabel* lbl_video = nullptr;        // 渲染区(棋盘格背景)
    QLabel* lbl_ok_card = nullptr;      // OK/NG 状态卡
    QLabel* lbl_video_info = nullptr;   // 视频信息: 处理/实时FPS/分辨率/帧号
    QTableWidget* tbl_events = nullptr; // 事件数据表
    QPlainTextEdit* logEdit = nullptr;  // 控制台输出(对话框内)
    QScrollArea* fp_scroll = nullptr;   // 误检帧画廊(对话框内)
    QWidget* fp_container = nullptr;
    QHBoxLayout* fp_hbox = nullptr;

    QPushButton* btn_start = nullptr;
    QPushButton* btn_stop = nullptr;

    QLabel* lbl_user = nullptr;         // 状态栏: 当前用户
    QLabel* lbl_scheme = nullptr;       // 状态栏: 方案名称
    QLabel* lbl_plc = nullptr;          // 状态栏: PLC连接
    QLabel* lbl_cam = nullptr;          // 状态栏: 相机连接
    QLabel* lbl_disk = nullptr;         // 状态栏: 剩余存储
    QLabel* lbl_clock = nullptr;        // 状态栏: 精确时间

    // ---- 运行状态 ----
    QMap<QString, QString> var;         // 配置项 (与 detect_ui 同键)
    QMap<QString, QString> st;          // 统计信息 (与 detect_ui 同键)
    QSet<QString> fp_shown;             // 已显示的误检帧
    QSet<QString> t_logged;             // 已打印过的计时标记
    QSet<int> entered_ids;              // 已记录"进入"事件的 track id
    QProcess* proc = nullptr;
    QTimer* timer = nullptr;
    QFile* mon_fh = nullptr;
    QByteArray out_buf;
    QString mon_path;
    QString proc_kind;                  // "detect" / "draw"
    qint64 frame_mt = -1, frame_sz = -1;
    qint64 last_video_t = -1;
    qint64 last_fp_scan = -1;
    qint64 t_click = -1;
    QString pending_log;
    bool log_dirty = false;
    bool sudo_fail = false;
    bool pending_run_after_draw = false;
    bool ng_state = false;              // 当前 NG 状态 (围栏内人数>=1)
    int event_seq = 0;                  // 事件表序号

    // ---- 界面构建 ----
    void buildUi();
    void buildTopBar();
    void buildMainArea();
    void buildStatusBar();
    void buildSettingsDialog();
    void buildQueryDialog();

    // ---- 工具 ----
    QStringList listVideos() const;
    QString zonePath() const;
    QStringList cmd() const;
    QStringList drawCmd() const;
    QString settingsPath() const;

    // ---- 控制 ----
    void start();
    void stop();
    void drawZone();
    void startMonitor();
    void launch(const QStringList& cmdline, const QString& kind);

    // ---- 设置持久化 ----
    void loadSettings();
    void saveSettings();

    // ---- 事件 ----
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void poll();
    void closeEvent(QCloseEvent* ev) override;

    // ---- 解析/刷新 ----
    void handleLine(const QString& line);
    void refresh();
    void clearStats();
    void updateVideo();
    void updateFpGallery();
    void showFpImage(const QString& path);
    void onDrawDone();
    void prepareFpDir();
    void addEvent(const QString& type, const QString& pos);
    void updateOkCard();
    void log(const QString& text);
    void flushLog();
    void tlog(const QString& tag, bool once = false);
};

#endif // INDUSTRIAL_UI_H
```

(前向声明 `QHBoxLayout`/`QMouseEvent` 已在对应源文件 include, 头文件内按需补充。)

- [ ] **Step 2: 校验头文件**

Run: 与 Task 3 一起编译验证(头文件单独无产物)。

---

### Task 3: 新建 industrial_ui.cpp — 主窗口/布局/状态栏

**Files:**
- Create: `src/industrial_ui.cpp`

- [ ] **Step 1: 写入主窗口骨架(布局与参考图一致)**

```cpp
// industrial_ui.cpp - RK3588 电子围栏 工业暗黑风界面 (DetectUI2)
// 布局/配色按 ui/电子围栏系统界面设计.md 参考图; 逻辑复用现有 Detect 日志协议
// 板上编译: cd build && make DetectUI2
#include "industrial_ui.h"

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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

// QProcess 子类: exec 前把子进程放入独立进程组, 停止时可用 killpg 一次杀掉 sudo + Detect
class IndustrialProcess : public QProcess {
public:
    explicit IndustrialProcess(QObject* parent = nullptr) : QProcess(parent) {}
protected:
    void setupChildProcess() override {
        ::setpgid(0, 0);
    }
};

// ---- 日志正则(detect_ui.cpp 原样移植 + 新增 [ENTER]) ----
static const QRegularExpression reFrame(
    R"(\[Frame (\d+)\] (\d+) tracked, (\d+) persons \| zone: (\d+) in / (\d+) out \(now (\d+)\) \| ([\d.]+) fps)");
static const QRegularExpression reInfo(R"(\[INFO f\d+\] raw_dets=(\d+) tracked=(\d+))");
static const QRegularExpression reFp(R"(\[FALSE-POS\] track=\d+ confirmed \(total: (\d+)\))");
static const QRegularExpression reFpz(R"(\[FALSE-POS\] track=\d+ ZONE ENTERED \(zone total: (\d+)\))");
static const QRegularExpression reMiss(R"(\[MISS\] track=\d+ lost \d+ frames \(total: (\d+))");
static const QRegularExpression reVideo(R"(Video:\s*(\d+)x(\d+)\s*@\s*(\d+))");
static const QRegularExpression reStatsFp(R"(\[STATS\] frames=(\d+)\s+confirmed_false_pos=(\d+)\s+zone_false_pos=(\d+))");
static const QRegularExpression reStatsMiss(R"(\[STATS\] frames=(\d+)\s+miss_count=(\d+))");
static const QRegularExpression reEnter(R"(\[ENTER\] track=(\d+))");
static const QRegularExpression reZoneEnter(R"(\[Zone\] Person id=(\d+) ENTERED)");
static const QRegularExpression reZoneExit(R"(\[Zone\] Person id=(\d+) EXITED)");

// ---------------- ClickableLabel2 ----------------
void ClickableLabel2::mousePressEvent(QMouseEvent* ev) {
    emit clicked();
    QLabel::mousePressEvent(ev);
}

// ---------------- 工业风样式 (暗黑主题) ----------------
static const char* const STYLE_SHEET = R"(
QMainWindow, QDialog { background-color: #2B2B2B; }
QWidget { color: #E0E0E0; }
QToolButton {
    background: transparent; border: none; border-radius: 4px;
    padding: 6px 10px; font-size: 13px; color: #CCCCCC;
}
QToolButton:hover { background: #3A3A3A; color: #FFFFFF; }
QToolButton:pressed { background: #454545; }
QToolButton:checked { background: #00B027; color: #FFFFFF; }
#topBar { background-color: #333333; border-bottom: 1px solid #1F1F1F; }
#logoText { color: #FF9900; font-size: 15px; font-weight: bold; }
#renderView { background-color: #1A1A1A; border: 1px solid #3A3A3A; }
#okCard {
    border-radius: 8px; font-size: 72px; font-weight: bold;
}
#okCard[state="ok"]   { background-color: #00B027; color: #000000; }
#okCard[state="ng"]   { background-color: #D32F2F; color: #FFFFFF; }
#okCard[state="idle"] { background-color: #444444; color: #888888; }
#videoInfo { background-color: #333333; color: #CCCCCC; font-size: 13px; padding: 6px; }
#statusBar QLabel { color: #CCCCCC; padding: 0 8px; }
#statusDot { color: #00FF00; font-weight: bold; }
#statusDot[bad="true"] { color: #FFFF00; }
QTableWidget { background-color: #FFFFFF; color: #000000; gridline-color: #CCCCCC;
               font-size: 13px; }
QHeaderView::section { background-color: #DDDDDD; color: #000000;
                       padding: 4px; border: 1px solid #BBBBBB; font-weight: bold; }
QTableWidget::item { padding: 2px 6px; }
QTableWidget::item:selected { background-color: #C8E6C9; color: #000000; }
QPlainTextEdit { background-color: #1A1A1A; color: #00CC00;
                 font-family: "DejaVu Sans Mono"; font-size: 12px; }
QLineEdit, QComboBox { background-color: #FFFFFF; color: #000000; }
QPushButton { background-color: #3A3A3A; color: #E0E0E0; border: 1px solid #555555;
              border-radius: 4px; padding: 5px 14px; }
QPushButton:hover { background-color: #4A4A4A; }
QPushButton:disabled { background-color: #2E2E2E; color: #777777; }
QPushButton#startBtn { background-color: #00B027; color: #FFFFFF; border: none; font-weight: bold; }
QPushButton#stopBtn  { background-color: #D32F2F; color: #FFFFFF; border: none; font-weight: bold; }
QGroupBox { border: 1px solid #3A3A3A; border-radius: 4px; margin-top: 8px;
            padding-top: 8px; color: #CCCCCC; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }
QScrollArea { background: transparent; }
QScrollBar:vertical { background: #2B2B2B; width: 10px; }
QScrollBar::handle:vertical { background: #555555; border-radius: 5px; min-height: 20px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

// ---------------- IndustrialUI ----------------
IndustrialUI::IndustrialUI(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8("JEE 电子围栏系统"));
    resize(1600, 900);
    setStyleSheet(QString::fromUtf8(STYLE_SHEET));
    setWindowFlags(Qt::Window |
                   Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint |
                   Qt::WindowCloseButtonHint);

    var["mode"] = "fp+miss";
    var["src"] = "run";
    var["zone"] = "hybrid";
    var["zone_name"] = QString::fromUtf8("电子围栏");
    var["scheme"] = QString::fromUtf8("电子围栏");
    var["plc"] = "error";

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
    updateOkCard();

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &IndustrialUI::poll);
    timer->start(120);
    // 状态栏时钟每秒刷新
    QTimer* clock = new QTimer(this);
    connect(clock, &QTimer::timeout, this, [this]() {
        lbl_clock->setText(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    });
    clock->start(1000);
}
```

(第 2 步起续写 `buildUi`/`buildTopBar`/`buildMainArea`/`buildStatusBar` — 按下列结构逐步加入同一文件。)

- [ ] **Step 2: buildUi / buildTopBar — 顶部工具栏**

```cpp
void IndustrialUI::buildUi() {
    QWidget* central = new QWidget();
    setCentralWidget(central);
    QVBoxLayout* root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    buildTopBar();
    root->addWidget(toolbar_widget);
    buildMainArea();
    root->addLayout(main_layout, 1);
    statusBar()->setSizeGripEnabled(false);
}

void IndustrialUI::buildTopBar() {
    // 顶部工具栏: 左 Logo, 中按钮组, 右窗口控制
    QWidget* tb = new QWidget();
    tb->setObjectName("topBar");
    tb->setFixedHeight(64);
    QHBoxLayout* lay = new QHBoxLayout(tb);
    lay->setContentsMargins(12, 0, 12, 0);
    lay->setSpacing(6);

    // 左侧 Logo + 名称
    QLabel* logo = new QLabel(QString::fromUtf8("JEE"));
    logo->setStyleSheet(QString::fromUtf8("background:#FF9900; color:#000000; font-weight:bold; "
                                          "font-size:20px; padding:4px 12px; border-radius:4px;"));
    lay->addWidget(logo);
    QLabel* name = new QLabel(QString::fromUtf8("安徽巨一科技股份有限公司"));
    name->setObjectName("logoText");
    lay->addWidget(name);
    lay->addSpacing(24);

    // 中间按钮组 (图标在上文字在下, QToolButton)
    struct TopBtn { const char* text; const char* icon; };
    TopBtn btns[] = {
        {u8"打开项目", "📁"}, {u8"系统参数", "⚙"}, {u8"相机设备", "📷"},
        {u8"外部通信", "🔌"}, {u8"数据查询", "🔍"}, {u8"画布设置", "🎨"},
        {u8"用户管理", "👤"}, {u8"开始", "▶"}, {u8"停止", "■"},
    };
    for (int i = 0; i < 9; ++i) {
        QToolButton* b = new QToolButton(tb);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setText(QString::fromUtf8(btns[i].text));
        b->setIcon(QIcon());  // 单色线条图标可后续替换, 暂用文字
        b->setFixedHeight(58);
        if (i == 7) { b->setObjectName("startBtn"); b->setStyleSheet(b->styleSheet() + QString::fromUtf8("background:#00B027; color:#FFFFFF; font-weight:bold;")); }
        if (i == 8) { b->setObjectName("stopBtn");  b->setStyleSheet(b->styleSheet() + QString::fromUtf8("background:#D32F2F; color:#FFFFFF; font-weight:bold;")); }
        lay->addWidget(b);
        // 信号连接
        switch (i) {
        case 0: connect(b, &QToolButton::clicked, this, &IndustrialUI::pickVideo); break;
        case 1: connect(b, &QToolButton::clicked, this, &IndustrialUI::openSettings); break;
        case 2: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCameraDlg); break;
        case 3: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCommDlg); break;
        case 4: connect(b, &QToolButton::clicked, this, &IndustrialUI::openQuery); break;
        case 5: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCanvasDlg); break;
        case 6: connect(b, &QToolButton::clicked, this, &IndustrialUI::openUserDlg); break;
        case 7: btn_start = b; connect(b, &QToolButton::clicked, this, &IndustrialUI::start); break;
        case 8: btn_stop = b; btn_stop->setEnabled(false); connect(b, &QToolButton::clicked, this, &IndustrialUI::stop); break;
        }
    }
    lay->addStretch(1);

    // 右侧窗口控制 (最小化/最大化/关闭)
    QToolButton* b_min = new QToolButton(tb);
    b_min->setText(QString::fromUtf8("—"));
    b_min->setToolTip(QString::fromUtf8("最小化"));
    connect(b_min, &QToolButton::clicked, this, &QWidget::showMinimized);
    QToolButton* b_max = new QToolButton(tb);
    b_max->setText(QString::fromUtf8("□"));
    b_max->setToolTip(QString::fromUtf8("最大化"));
    connect(b_max, &QToolButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
    });
    QToolButton* b_close = new QToolButton(tb);
    b_close->setText(QString::fromUtf8("✕"));
    b_close->setToolTip(QString::fromUtf8("关闭"));
    connect(b_close, &QToolButton::clicked, this, &QWidget::close);
    lay->addWidget(b_min);
    lay->addWidget(b_max);
    lay->addWidget(b_close);
}
```

(注意: `toolbar_widget`/`main_layout` 与 `pickVideo`/`openSettings` 等槽成员需在头文件补齐声明 — 见 Task 5 补齐。)

- [ ] **Step 3: buildMainArea — 渲染区 + OK卡 + 事件表 + 视频信息**

```cpp
void IndustrialUI::buildMainArea() {
    main_layout = new QHBoxLayout();
    main_layout->setContentsMargins(10, 10, 10, 10);
    main_layout->setSpacing(10);

    // ---- 左侧: 渲染结果区(棋盘格) ----
    QWidget* left = new QWidget();
    QVBoxLayout* left_lay = new QVBoxLayout(left);
    left_lay->setContentsMargins(0, 0, 0, 0);
    left_lay->setSpacing(8);

    lbl_video = new QLabel(QString::fromUtf8("等待画面..."));
    lbl_video->setObjectName("renderView");
    lbl_video->setMinimumSize(700, 460);
    lbl_video->setAlignment(Qt::AlignCenter);
    // 棋盘格背景
    lbl_video->setStyleSheet(QString::fromUtf8(
        "#renderView { background-image: url(:/checker.png); background-repeat: repeat; }"));
    left_lay->addWidget(lbl_video, 1);

    // 视频信息栏
    lbl_video_info = new QLabel();
    lbl_video_info->setObjectName("videoInfo");
    left_lay->addWidget(lbl_video_info);
    main_layout->addWidget(left, 3);

    // ---- 右侧: OK/NG 卡 + 事件表 ----
    QWidget* right = new QWidget();
    QVBoxLayout* right_lay = new QVBoxLayout(right);
    right_lay->setContentsMargins(0, 0, 0, 0);
    right_lay->setSpacing(10);

    lbl_ok_card = new QLabel(QString::fromUtf8("OK"));
    lbl_ok_card->setObjectName("okCard");
    lbl_ok_card->setProperty("state", "idle");
    lbl_ok_card->setAlignment(Qt::AlignCenter);
    lbl_ok_card->setMinimumHeight(200);
    lbl_ok_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    right_lay->addWidget(lbl_ok_card);

    QGroupBox* ev = new QGroupBox(QString::fromUtf8("事件记录"));
    QVBoxLayout* ev_lay = new QVBoxLayout(ev);
    tbl_events = new QTableWidget(0, 4);
    tbl_events->setHorizontalHeaderLabels(
        QStringList() << QString::fromUtf8("序号") << QString::fromUtf8("时间")
                      << QString::fromUtf8("事件") << QString::fromUtf8("位置"));
    tbl_events->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tbl_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl_events->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl_events->verticalHeader()->setVisible(false);
    ev_lay->addWidget(tbl_events);
    right_lay->addWidget(ev, 1);
    main_layout->addWidget(right, 2);
}
```

- [ ] **Step 4: buildStatusBar — 底部状态栏**

```cpp
void IndustrialUI::buildStatusBar() {
    statusBar()->setObjectName("statusBar");
    lbl_user = new QLabel(QString::fromUtf8("用户: ") + qgetenv("USER"));
    lbl_scheme = new QLabel(QString::fromUtf8("方案: 电子围栏"));
    lbl_plc = new QLabel(QString::fromUtf8("● PLC连接: 错误"));
    lbl_plc->setProperty("bad", "true");
    lbl_cam = new QLabel(QString::fromUtf8("● 相机连接: 未启动"));
    lbl_cam->setProperty("bad", "true");
    lbl_disk = new QLabel();
    lbl_clock = new QLabel(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    statusBar()->addWidget(lbl_user);
    statusBar()->addWidget(lbl_scheme);
    statusBar()->addPermanentWidget(lbl_plc);
    statusBar()->addPermanentWidget(lbl_cam);
    statusBar()->addPermanentWidget(lbl_disk);
    statusBar()->addPermanentWidget(lbl_clock);
    updateDiskInfo();
}

void IndustrialUI::updateDiskInfo() {
    QStorageInfo si("/");
    double gb = si.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
    lbl_disk->setText(QString::fromUtf8("剩余存储: %1 GB").arg(gb, 0, 'f', 1));
}
```

- [ ] **Step 5: 编译验证(本机语法)**

Run: `cd build && make DetectUI2` (需先完成 Task 5 的 CMake 目标)
Expected: 编译通过。若报未声明成员(如 `toolbar_widget`/`main_layout`/`pickVideo`), 属 Task 5 头文件补齐范畴, 一并补齐后重编译。

---

### Task 4: industrial_ui.cpp — 运行控制 / 进程管理 (从 detect_ui.cpp 移植)

**Files:**
- Modify: `src/industrial_ui.cpp` (续写)

- [ ] **Step 1: 命令构造 / 视频列表 / 画围栏命令**

```cpp
QStringList IndustrialUI::listVideos() const {
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

QString IndustrialUI::zonePath() const {
    return QDir::cleanPath(QDir(var_cwd->text()).filePath("../models/zone.txt"));
}

QStringList IndustrialUI::cmd() const {
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

QStringList IndustrialUI::drawCmd() const {
    QStringList c;
    c << "./Detect" << "--draw-zone-only" << "--file" << var_file->text();
    return c;
}
```

(注意: `var_file`/`var_model`/`var_cwd`/`var_conf`/`var_fpdir`/`var_sudo` 为设置对话框中的控件指针, 需在头文件声明 — 见 Task 5。)

- [ ] **Step 2: start / stop / drawZone / startMonitor / launch**

```cpp
void IndustrialUI::start() {
    stop();
    if (var_sudo->isChecked() && ::geteuid() != 0) {
        var_sudo->setChecked(false);
        log(QString::fromUtf8("[警告] 勾选了「使用sudo」但界面非 root 启动: Detect 会以 root 运行, 停止按钮将失效。"
                              "已自动取消勾选, 以普通用户运行 Detect(NPU 无需 root)。如需 root 请用 sudo ./DetectUI2 启动界面。"));
    }
    saveSettings();
    clearStats();
    t_logged.clear();
    entered_ids.clear();
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

void IndustrialUI::stop() {
    if (proc && proc->state() != QProcess::NotRunning) {
        qint64 pid = proc->processId();
        if (pid > 0) {
            if (::kill(-(pid_t)pid, SIGTERM) != 0)
                log(QString::fromUtf8("[停止] 停止信号发送失败(errno=%1): Detect 可能以 root 运行而界面是普通用户, "
                                      "请用 sudo ./DetectUI2 启动界面或取消勾选「使用sudo」")
                        .arg(errno));
        }
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
    updateOkCard();   // 停止后恢复 idle
}

void IndustrialUI::drawZone() {
    stop();
    pending_run_after_draw = false;
    prepareFpDir();
    log(QString::fromUtf8("进入画围栏模式: 在弹出来的窗口里点4个以上点, 回车保存 / ESC跳过"));
    launch(drawCmd(), "draw");
}

void IndustrialUI::startMonitor() {
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

void IndustrialUI::launch(const QStringList& cmdline, const QString& kind) {
    st["video"] = QFileInfo(var_file->text()).fileName();
    log("$ " + cmdline.join(" "));
    proc = new IndustrialProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(var_cwd->text());
    connect(proc, &QProcess::readyReadStandardOutput, this, &IndustrialUI::onReadyRead);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &IndustrialUI::onProcessFinished);
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
```

- [ ] **Step 3: onReadyRead / onProcessFinished / onDrawDone / prepareFpDir**

```cpp
void IndustrialUI::onReadyRead() {
    if (!proc) return;
    out_buf += proc->readAllStandardOutput();
    int idx;
    while ((idx = out_buf.indexOf('\n')) >= 0) {
        QByteArray line = out_buf.left(idx);
        out_buf.remove(0, idx + 1);
        handleLine(QString::fromUtf8(line).trimmed());
    }
}

void IndustrialUI::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status);
    QProcess* p = qobject_cast<QProcess*>(sender());
    if (!p) return;
    log(QString::fromUtf8("=== 进程已退出(exit=%1) ===").arg(exitCode));
    if (proc != p) {
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
    updateOkCard();   // 检测停止后状态卡回 idle
}

void IndustrialUI::onDrawDone() {
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

void IndustrialUI::prepareFpDir() {
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
    while (fp_hbox->count()) {
        QLayoutItem* item = fp_hbox->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    fp_shown.clear();
    log(QString::fromUtf8("误检帧目录已清空并准备: ") + d);
}
```

---

### Task 5: 头文件补齐 + 设置对话框 + 数据查询对话框

**Files:**
- Modify: `src/industrial_ui.h` (补齐成员声明)
- Modify: `src/industrial_ui.cpp` (续写)

- [ ] **Step 1: 头文件补齐 (在 Task 2 基础上追加)**

在 `industrial_ui.h` 的 `private:` 区追加:

```cpp
    // ---- 工具栏/对话框成员 ----
    QWidget* toolbar_widget = nullptr;
    QHBoxLayout* main_layout = nullptr;
    // 设置对话框控件
    QLineEdit* var_file = nullptr;
    QLineEdit* var_model = nullptr;
    QLineEdit* var_cwd = nullptr;
    QLineEdit* var_conf = nullptr;
    QLineEdit* var_interval = nullptr;
    QLineEdit* var_fp_frames = nullptr;
    QLineEdit* var_miss_frames = nullptr;
    QLineEdit* var_zone_confirm = nullptr;
    QLineEdit* var_zone_debounce = nullptr;
    QLineEdit* var_log = nullptr;
    QLineEdit* var_frame = nullptr;
    QLineEdit* var_fpdir = nullptr;
    QLineEdit* var_zone_name = nullptr;
    QLineEdit* var_scheme = nullptr;
    QCheckBox* var_sudo = nullptr;
    QMap<QString, QRadioButton*> radio_btns;

    // ---- 槽(对话框) ----
    void pickVideo();
    void openSettings();
    void openCameraDlg();
    void openCommDlg();
    void openQuery();
    void openCanvasDlg();
    void openUserDlg();
    void updateDiskInfo();
```

(前置声明补充: `class QCheckBox; class QHBoxLayout; class QMouseEvent;`)

- [ ] **Step 2: 系统参数对话框 buildSettingsDialog**

```cpp
void IndustrialUI::openSettings() {
    if (proc && proc->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QString::fromUtf8("系统参数"),
            QString::fromUtf8("检测运行中, 请先停止再修改参数。"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("系统参数"));
    dlg.resize(560, 520);
    QVBoxLayout* v = new QVBoxLayout(&dlg);
    QGridLayout* g = new QGridLayout();
    int r = 0;
    auto addRow = [&](const QString& label, QWidget* w) {
        g->addWidget(new QLabel(label), r, 0);
        g->addWidget(w, r, 1);
        r++;
    };
    var_file = new QLineEdit(var["video"]);
    var_model = new QLineEdit(var["model"]);
    var_cwd = new QLineEdit(var["cwd"]);
    var_conf = new QLineEdit(var["conf"]);
    var_interval = new QLineEdit(var["interval"]);
    var_fp_frames = new QLineEdit(var["fp_frames"]);
    var_miss_frames = new QLineEdit(var["miss_frames"]);
    var_zone_confirm = new QLineEdit(var["zone_confirm"]);
    var_zone_debounce = new QLineEdit(var["zone_debounce"]);
    var_log = new QLineEdit(var["log"]);
    var_frame = new QLineEdit(var["frame"]);
    var_fpdir = new QLineEdit(var["fpdir"]);
    var_zone_name = new QLineEdit(var["zone_name"]);
    var_scheme = new QLineEdit(var["scheme"]);
    addRow(QString::fromUtf8("视频文件:"), var_file);
    addRow(QString::fromUtf8("模型:"), var_model);
    addRow(QString::fromUtf8("工作目录:"), var_cwd);
    addRow(QString::fromUtf8("检测阈值:"), var_conf);
    addRow(QString::fromUtf8("抽帧间隔:"), var_interval);
    addRow(QString::fromUtf8("误检判断帧:"), var_fp_frames);
    addRow(QString::fromUtf8("漏检判断帧:"), var_miss_frames);
    addRow(QString::fromUtf8("围栏判断帧:"), var_zone_confirm);
    addRow(QString::fromUtf8("围栏防抖:"), var_zone_debounce);
    addRow(QString::fromUtf8("日志文件:"), var_log);
    addRow(QString::fromUtf8("画面文件:"), var_frame);
    addRow(QString::fromUtf8("误检目录:"), var_fpdir);
    addRow(QString::fromUtf8("围栏名称:"), var_zone_name);
    addRow(QString::fromUtf8("方案名称:"), var_scheme);
    v->addLayout(g);

    // 围栏判定模式
    QGroupBox* zb = new QGroupBox(QString::fromUtf8("围栏判定"));
    QHBoxLayout* zl = new QHBoxLayout(zb);
    auto radio = [this](QHBoxLayout* l, const QString& text, bool chk, const QString& key, const QString& val) {
        QRadioButton* rb = new QRadioButton(text);
        rb->setChecked(chk);
        connect(rb, &QRadioButton::toggled, this,
                [this, key, val](bool on) { if (on) var[key] = val; });
        radio_btns[key + "/" + val] = rb;
        l->addWidget(rb);
        return rb;
    };
    radio(zl, QString::fromUtf8("混合(脚点+面积,推荐)"), var["zone"] == "hybrid", "zone", "hybrid");
    radio(zl, QString::fromUtf8("脚点"), var["zone"] == "foot", "zone", "foot");
    radio(zl, QString::fromUtf8("面积IoB"), var["zone"] == "area", "zone", "area");
    v->addWidget(zb);

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);

    if (dlg.exec() == QDialog::Accepted) {
        var["video"] = var_file->text();
        var["model"] = var_model->text();
        var["cwd"] = var_cwd->text();
        var["conf"] = var_conf->text();
        var["interval"] = var_interval->text();
        var["fp_frames"] = var_fp_frames->text();
        var["miss_frames"] = var_miss_frames->text();
        var["zone_confirm"] = var_zone_confirm->text();
        var["zone_debounce"] = var_zone_debounce->text();
        var["log"] = var_log->text();
        var["frame"] = var_frame->text();
        var["fpdir"] = var_fpdir->text();
        var["zone_name"] = var_zone_name->text();
        var["scheme"] = var_scheme->text();
        lbl_scheme->setText(QString::fromUtf8("方案: ") + var["scheme"]);
        saveSettings();
        log(QString::fromUtf8("[设置] 已保存: 模型=%1 阈值=%2 视频=%3")
                .arg(var["model"], var["conf"], var["video"]));
    }
    // 释放对话框内控件, 防悬挂指针
    var_file = var_model = var_cwd = var_conf = var_interval = nullptr;
    var_fp_frames = var_miss_frames = var_zone_confirm = var_zone_debounce = nullptr;
    var_log = var_frame = var_fpdir = var_zone_name = var_scheme = nullptr;
    var_sudo = nullptr;
}
```

- [ ] **Step 3: 数据查询对话框 buildQueryDialog (误检画廊 + 控制台)**

```cpp
void IndustrialUI::openQuery() {
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("数据查询"));
    dlg.resize(1000, 620);
    QVBoxLayout* v = new QVBoxLayout(&dlg);
    QTabWidget* tabs = new QTabWidget();
    v->addWidget(tabs);

    // Tab1: 误检帧画廊
    QWidget* fp_tab = new QWidget();
    QVBoxLayout* fv = new QVBoxLayout(fp_tab);
    fp_scroll = new QScrollArea();
    fp_scroll->setWidgetResizable(true);
    fp_container = new QWidget();
    fp_hbox = new QHBoxLayout(fp_container);
    fp_hbox->setAlignment(Qt::AlignLeft);
    fp_hbox->setSpacing(6);
    fp_scroll->setWidget(fp_container);
    fv->addWidget(fp_scroll);
    tabs->addTab(fp_tab, QString::fromUtf8("误检帧"));
    // 打开时先清空再重扫 (画廊内容在 poll 中持续更新)
    while (fp_hbox->count()) {
        QLayoutItem* item = fp_hbox->takeAt(0);
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    fp_shown.clear();
    updateFpGallery();

    // Tab2: 控制台输出
    QWidget* log_tab = new QWidget();
    QVBoxLayout* lv = new QVBoxLayout(log_tab);
    logEdit = new QPlainTextEdit();
    logEdit->setReadOnly(true);
    logEdit->setMaximumBlockCount(2000);
    lv->addWidget(logEdit);
    tabs->addTab(log_tab, QString::fromUtf8("控制台"));

    dlg.exec();
    // 关闭后断开画廊指针, 防 poll 写已销毁控件
    fp_scroll = nullptr; fp_container = nullptr; fp_hbox = nullptr; logEdit = nullptr;
}
```

- [ ] **Step 4: 其余占位对话框**

```cpp
void IndustrialUI::pickVideo() {
    QString p = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择视频"), "/home/orangepi/Videos");
    if (!p.isEmpty()) var["video"] = p;
}

void IndustrialUI::openCameraDlg() {
    QMessageBox::information(this, QString::fromUtf8("相机设备"),
        QString::fromUtf8("运行方式: %1\n使用sudo: %2")
            .arg(var["src"] == "monitor" ? QString::fromUtf8("监视文件") : QString::fromUtf8("本软件启动"))
            .arg(var_sudo ? QString::fromUtf8("开") : QString::fromUtf8("关")));
}

void IndustrialUI::openCommDlg() {
    QMessageBox::information(this, QString::fromUtf8("外部通信"),
        QString::fromUtf8("PLC 连接状态: %1 (在系统参数中配置)").arg(var["plc"]));
}

void IndustrialUI::openCanvasDlg() {
    QMessageBox::information(this, QString::fromUtf8("画布设置"),
        QString::fromUtf8("围栏名称: %1\n画布背景: 棋盘格").arg(var["zone_name"]));
}

void IndustrialUI::openUserDlg() {
    QMessageBox::information(this, QString::fromUtf8("用户管理"),
        QString::fromUtf8("当前用户: %1 (单用户系统, 无管理功能)").arg(QString::fromUtf8(qgetenv("USER"))));
}
```

---

### Task 6: industrial_ui.cpp — 日志解析 / 事件表 / OK卡 / 画面轮询

**Files:**
- Modify: `src/industrial_ui.cpp` (续写)

- [ ] **Step 1: handleLine — 复用现有正则 + [ENTER]/[Zone] 事件**

```cpp
void IndustrialUI::handleLine(const QString& line) {
    log(line);
    if (line.contains(QString::fromUtf8("password is required"))) sudo_fail = true;

    QRegularExpressionMatch m = reVideo.match(line);
    if (m.hasMatch()) {
        st["width"] = m.captured(1);
        st["height"] = m.captured(2);
        st["vfps"] = m.captured(3);
        lbl_cam->setProperty("bad", "false");   // 相机连接 = 已连接
        lbl_cam->setText(QString::fromUtf8("● 相机连接: 已连接"));
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

    // ---- 事件 ----
    m = reEnter.match(line);
    if (m.hasMatch()) {
        int tid = m.captured(1).toInt();
        if (!entered_ids.contains(tid)) {
            entered_ids.insert(tid);
            addEvent(QString::fromUtf8("进入"), var["zone_name"]);
        }
        return;   // 每行只处理一个事件
    }
    m = reZoneEnter.match(line);
    if (m.hasMatch()) addEvent(QString::fromUtf8("警报"), var["zone_name"]);
    m = reZoneExit.match(line);
    if (m.hasMatch()) addEvent(QString::fromUtf8("离开"), var["zone_name"]);
}
```

- [ ] **Step 2: addEvent / updateOkCard / refresh / clearStats**

```cpp
void IndustrialUI::addEvent(const QString& type, const QString& pos) {
    if (!tbl_events) return;
    event_seq++;
    int row = 0;
    tbl_events->insertRow(row);
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss");
    QStringList cells = { QString::number(event_seq), timeStr, type, pos };
    for (int c = 0; c < 4; ++c) {
        QTableWidgetItem* item = new QTableWidgetItem(cells[c]);
        item->setTextAlignment(Qt::AlignCenter);
        if (type == QString::fromUtf8("警报"))
            item->setForeground(QBrush(QColor(0xD3, 0x2F, 0x2F)));
        else if (type == QString::fromUtf8("离开"))
            item->setForeground(QBrush(QColor(0x00, 0x90, 0x27)));
        else
            item->setForeground(QBrush(QColor(0x20, 0x20, 0x20)));
        tbl_events->setItem(row, c, item);
    }
    tbl_events->scrollToTop();
    // 事件表行数上限, 防无限增长
    while (tbl_events->rowCount() > 5000) tbl_events->removeRow(tbl_events->rowCount() - 1);
    updateOkCard();
}

void IndustrialUI::updateOkCard() {
    bool running = (proc != nullptr);
    bool ng = running && st["zone_now"].toInt() > 0;
    if (ng != ng_state) {
        ng_state = ng;
        if (ng) {
            lbl_ok_card->setProperty("state", "ng");
            lbl_ok_card->setText(QString::fromUtf8("NG"));
        } else if (running) {
            lbl_ok_card->setProperty("state", "ok");
            lbl_ok_card->setText(QString::fromUtf8("OK"));
        } else {
            lbl_ok_card->setProperty("state", "idle");
            lbl_ok_card->setText(QString::fromUtf8("----"));
        }
        // 属性变化后刷新样式
        lbl_ok_card->style()->unpolish(lbl_ok_card);
        lbl_ok_card->style()->polish(lbl_ok_card);
    }
}

void IndustrialUI::refresh() {
    // 视频信息栏: 处理FPS/实时FPS/分辨率/帧号
    QString info = QString::fromUtf8("处理FPS: %1 | 实时FPS: %2 | 分辨率: %3x%4 | 帧号: %5")
                       .arg(st["fps"], st["vfps"], st["width"], st["height"], st["frame"]);
    lbl_video_info->setText(info);
    // 状态卡由 addEvent 与 poll 共同驱动, 这里也同步一次(zone_now 变化时)
    updateOkCard();
}

void IndustrialUI::clearStats() {
    st["frame"] = "0"; st["fps"] = "0"; st["raw"] = "0";
    st["tracked"] = "0"; st["persons"] = "0"; st["zone_in"] = "0";
    st["zone_out"] = "0"; st["zone_now"] = "0"; st["fp"] = "0";
    st["fpz"] = "0"; st["miss"] = "0"; st["width"] = "?"; st["height"] = "?";
    st["vfps"] = "?";
    // 停止后事件表清空, 序号归零
    if (tbl_events) tbl_events->setRowCount(0);
    event_seq = 0;
    entered_ids.clear();
    ng_state = false;
    lbl_ok_card->setProperty("state", "idle");
    lbl_ok_card->setText(QString::fromUtf8("----"));
    refresh();
}
```

- [ ] **Step 3: updateVideo / updateFpGallery / showFpImage / poll**

```cpp
void IndustrialUI::updateVideo() {
    QString fp = var["frame"];
    if (fp.isEmpty()) return;
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_video_t < 250) return;
    QFileInfo fi(fp);
    if (!fi.exists()) return;
    qint64 mt = fi.lastModified().toMSecsSinceEpoch();
    qint64 sz = fi.size();
    if (mt == frame_mt && sz == frame_sz) return;
    frame_mt = mt; frame_sz = sz;
    last_video_t = now;
    QPixmap pm(fp);
    if (!pm.isNull()) {
        pm = pm.scaled(lbl_video->width(), lbl_video->height(),
                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
        lbl_video->setPixmap(pm);
        tlog(QString::fromUtf8("画面已显示"), true);
    }
}

void IndustrialUI::updateFpGallery() {
    if (!fp_hbox) return;
    QString d = var["fpdir"];
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
        ClickableLabel2* lbl = new ClickableLabel2();
        lbl->setPixmap(pm);
        lbl->setFixedSize(200, 112);
        lbl->setToolTip(f + QString::fromUtf8("（点击放大）"));
        lbl->setCursor(Qt::PointingHandCursor);
        connect(lbl, &ClickableLabel2::clicked, this,
                [this, full]() { showFpImage(full); });
        fp_hbox->addWidget(lbl);
    }
}

void IndustrialUI::showFpImage(const QString& path) {
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

void IndustrialUI::poll() {
    if (mon_fh) {
        while (mon_fh->canReadLine()) {
            QByteArray line = mon_fh->readLine();
            handleLine(QString::fromUtf8(line).trimmed());
        }
    }
    updateVideo();
    if (fp_hbox) updateFpGallery();
    refresh();
}
```

---

### Task 7: 设置持久化 + 日志 + main

**Files:**
- Modify: `src/industrial_ui.cpp` (续写)

- [ ] **Step 1: loadSettings / saveSettings / settingsPath**

```cpp
QString IndustrialUI::settingsPath() const {
    return QCoreApplication::applicationDirPath() + "/detectui2.ini";
}

void IndustrialUI::loadSettings() {
    QSettings s(settingsPath(), QSettings::IniFormat);
    var["video"] = s.value("video", QString("")).toString();
    var["model"] = s.value("model", QString("yolov26s_v2_int8.rknn")).toString();
    var["cwd"] = s.value("cwd", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build")).toString();
    var["conf"] = s.value("conf", QString("0.65")).toString();
    var["interval"] = s.value("interval", QString("1")).toString();
    var["fp_frames"] = s.value("fp_frames", QString("8")).toString();
    var["miss_frames"] = s.value("miss_frames", QString("8")).toString();
    var["zone_confirm"] = s.value("zone_confirm", QString("3")).toString();
    var["zone_debounce"] = s.value("zone_debounce", QString("3")).toString();
    var["log"] = s.value("log", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/detect.log")).toString();
    var["frame"] = s.value("frame", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/frame.jpg")).toString();
    var["fpdir"] = s.value("fpdir", QString("/home/orangepi/rknn_yolov8_bytetrack-main/build/fp_frames")).toString();
    var["zone_name"] = s.value("zone_name", QString::fromUtf8("电子围栏")).toString();
    var["scheme"] = s.value("scheme", QString::fromUtf8("电子围栏")).toString();
    var["plc"] = s.value("plc", QString("error")).toString();
    var["mode"] = s.value("mode", QString("fp+miss")).toString();
    var["src"] = s.value("src", QString("run")).toString();
    var["zone"] = s.value("zone", QString("hybrid")).toString();
    var["sudo"] = s.value("sudo", false).toString();
    lbl_scheme->setText(QString::fromUtf8("方案: ") + var["scheme"]);
    lbl_plc->setText(QString::fromUtf8("● PLC连接: ") +
                     (var["plc"] == "ok" ? QString::fromUtf8("正常") : QString::fromUtf8("错误")));
    lbl_plc->setProperty("bad", var["plc"] != "ok" ? "true" : "false");
    lbl_plc->style()->unpolish(lbl_plc);
    lbl_plc->style()->polish(lbl_plc);
    log(QString::fromUtf8("[设置] 已加载: 模型=%1 阈值=%2 视频=%3 围栏=%4")
            .arg(var["model"], var["conf"], var["video"], var["zone_name"]));
}

void IndustrialUI::saveSettings() {
    QSettings s(settingsPath(), QSettings::IniFormat);
    s.setValue("video", var["video"]);
    s.setValue("model", var["model"]);
    s.setValue("cwd", var["cwd"]);
    s.setValue("conf", var["conf"]);
    s.setValue("interval", var["interval"]);
    s.setValue("fp_frames", var["fp_frames"]);
    s.setValue("miss_frames", var["miss_frames"]);
    s.setValue("zone_confirm", var["zone_confirm"]);
    s.setValue("zone_debounce", var["zone_debounce"]);
    s.setValue("log", var["log"]);
    s.setValue("frame", var["frame"]);
    s.setValue("fpdir", var["fpdir"]);
    s.setValue("zone_name", var["zone_name"]);
    s.setValue("scheme", var["scheme"]);
    s.setValue("plc", var["plc"]);
    s.setValue("mode", var["mode"]);
    s.setValue("src", var["src"]);
    s.setValue("zone", var["zone"]);
    s.setValue("sudo", var["sudo"]);
    s.sync();
}
```

- [ ] **Step 2: log / tlog / flushLog / closeEvent**

```cpp
void IndustrialUI::log(const QString& text) {
    pending_log += text + "\n";
    log_dirty = true;
    // 无头调试: 镜像到 stderr
    if (qgetenv("DETECTUI_TIMELOG") == "1") {
        fprintf(stderr, "%s\n", text.toUtf8().constData());
        fflush(stderr);
    }
}

void IndustrialUI::flushLog() {
    if (!log_dirty || !logEdit) return;
    logEdit->appendPlainText(pending_log);
    QScrollBar* sb = logEdit->verticalScrollBar();
    sb->setValue(sb->maximum());
    pending_log.clear();
    log_dirty = false;
}

void IndustrialUI::tlog(const QString& tag, bool once) {
    if (t_click <= 0) return;
    if (once && t_logged.contains(tag)) return;
    t_logged.insert(tag);
    double dt = (QDateTime::currentMSecsSinceEpoch() - t_click) / 1000.0;
    log(QString::fromUtf8("[计时] %1 (+%2s)").arg(tag).arg(dt, 0, 'f', 2));
}

void IndustrialUI::closeEvent(QCloseEvent* ev) {
    saveSettings();
    QMainWindow::closeEvent(ev);
}
```

- [ ] **Step 3: main()**

```cpp
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    IndustrialUI ui;
    ui.show();
    return app.exec();
}
```

---

### Task 8: CMake 新增 DetectUI2 目标 + 编译验证

**Files:**
- Modify: `CMakeLists.txt` (Qt5 块内, 第 121 行后)

- [ ] **Step 1: 添加目标**

在 `CMakeLists.txt` 第 121 行 `add_executable(DetectUI src/detect_ui.cpp src/detect_ui.h)` 之后追加:

```cmake
  # DetectUI2: 工业暗黑风界面 (ui/电子围栏系统界面设计.md), 独立程序
  # 单独编译: cd build && make DetectUI2
  add_executable(DetectUI2 src/industrial_ui.cpp src/industrial_ui.h)
  target_link_libraries(DetectUI2 Qt5::Widgets ${CMAKE_THREAD_LIBS_INIT})
```

- [ ] **Step 2: 重新配置并编译**

Run: `cd build && cmake .. && make DetectUI2`
Expected: 编译通过, 生成 `build/DetectUI2`。`DetectUI` 目标不受影响。

- [ ] **Step 3: 编译现有目标回归**

Run: `cd build && make DetectUI`
Expected: 现有 DetectUI 编译通过(源码未动)。

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt src/industrial_ui.h src/industrial_ui.cpp src/main_MultiDetect.cpp
git commit -m "feat: add DetectUI2 industrial dark UI (电子围栏工业界面)"
```

---

### Task 9: 运行验证 (板上 / SSH 无头)

**Files:**
- (无源码改动)

- [ ] **Step 1: 无头运行 + 自动启动**

Run (SSH, 无显示器):
```bash
cd build && LIBGL_ALWAYS_SOFTWARE=1 DETECTUI_TIMELOG=1 \
  ./DetectUI2 --auto-start > /tmp/ui2.log 2>&1 &
sleep 8; head -50 /tmp/ui2.log
```
Expected: `[启动]` 日志出现; 若 zone.txt 存在则 Detect 自动启动, stdout 事件逐行出现。

- [ ] **Step 2: 验证事件输出**

Run: `grep -E "\[ENTER\]|\[Zone\]" /tmp/ui2.log | head -20`
Expected: 出现 `[ENTER] track=N` 与 `[Zone] Person id=N ENTERED/EXITED`。

- [ ] **Step 3: 屏幕验证(有显示器时)**

启动 `./DetectUI2`, 人工确认:
- 主界面: 顶部工具栏(Logo/9按钮/窗口控制)、棋盘格渲染区、OK卡(绿/OK)、事件表、状态栏(用户/方案/PLC/相机/存储/时间)
- 围栏有人时 OK卡翻转为 NG 红; 无人恢复 OK 绿
- 事件表依次出现 进入→警报→离开
- 「系统参数」对话框修改围栏名称后, 事件表位置列显示新名称
- 「数据查询」: 误检帧画廊缩略图可点击放大; 控制台输出实时滚动
- 关闭/重开界面: 设置从 detectui2.ini 恢复

---

## Self-Review 检查

- **规格覆盖**: §2 架构→Task 2-8; §3 视觉→Task 3 样式表/棋盘格; §4.1 工具栏→Task 3; §4.2 渲染区→Task 3/6; §4.3 OK卡→Task 6 updateOkCard; §4.4 数据表→Task 6 addEvent; §4.5 视频信息→Task 6 refresh; §4.6 状态栏→Task 3/7; §5 事件协议→Task 1 + Task 6; §6 对话框→Task 5; §7 运行逻辑→Task 4/6; §8 测试→Task 9; §9 范围外→无任务(符合 YAGNI)。
- **占位符**: 无 TODO/TBD; 所有代码步骤含完整代码。
- **类型一致性**: `var["..."]`/`st[...]` 键名与 detect_ui 一致; `updateOkCard`/`addEvent`/`handleLine` 签名在 Task 6 定义、Task 2 头文件声明一致; `fp_hbox`/`logEdit` 在 openQuery 中置空、poll/updateFpGallery 判空, 防悬挂指针。
