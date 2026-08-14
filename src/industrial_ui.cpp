// industrial_ui.cpp - RK3588 电子围栏 工业暗黑风界面 (DetectUI2)
// 布局/配色按 ui/电子围栏系统界面设计.md 参考图; 逻辑复用现有 Detect 日志协议
// 板上编译: cd build && make DetectUI2
#include "industrial_ui.h"

#include <QApplication>
#include <QBrush>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSize>
#include <QSizePolicy>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStringList>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <QAbstractNativeEventFilter>
#else
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// QProcess 子类: exec 前把子进程放入独立进程组, 停止时可用 killpg 一次杀掉 sudo + Detect
class IndustrialProcess : public QProcess {
public:
    explicit IndustrialProcess(QObject* parent = nullptr) : QProcess(parent) {}
protected:
    void setupChildProcess() override {
#ifndef _WIN32
        ::setpgid(0, 0);
#endif
    }
};

// ---- 日志正则(detect_ui.cpp 原样移植 + 新增 [ENTER]/[Zone]) ----
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

// ---------------- RenderView (渲染区: 棋盘格 + 视频帧 + 围栏叠加) ----------------
void RenderView::paintEvent(QPaintEvent* ev) {
    Q_UNUSED(ev);
    QPainter p(this);
    // 1) 棋盘格背景: 20x20 格子, 两个灰阶 #2B2B2B / #3A3A3A (替代失效的 :/checker.png 资源)
    const int cell = 20;
    const int nx = (width() + cell - 1) / cell;
    const int ny = (height() + cell - 1) / cell;
    p.fillRect(rect(), QColor(0x2B, 0x2B, 0x2B));
    p.setBrush(QColor(0x3A, 0x3A, 0x3A));
    p.setPen(Qt::NoPen);
    for (int i = 0; i < ny; ++i)
        for (int j = (i & 1); j < nx; j += 2)
            p.drawRect(j * cell, i * cell, cell, cell);
    // 边框: RenderView 是自绘控件, QSS 的 #renderView border 不会自动画, 这里补画
    p.setPen(QColor(0x3A, 0x3A, 0x3A));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // 2) 视频帧: 等比缩放居中 (无帧时无操作)
    QRect vr;   // 帧在控件内的目标矩形(缩放+居中)
    if (!frame.isNull()) {
        QSize s = frame.size().scaled(size(), Qt::KeepAspectRatio);
        vr = QRect((width() - s.width()) / 2, (height() - s.height()) / 2,
                   s.width(), s.height());
        p.drawPixmap(vr, frame);
    }

    // 3) 围栏多边形: 半透明橙填充 + 描边; 坐标从视频像素映射到控件
    if (hasZone) {
        QPolygonF mapped;
        if (!frame.isNull()) {
            // 帧映射: 视频坐标 → 控件坐标 (等比缩放的线性映射)
            double sx = double(vr.width()) / frame.width();
            double sy = double(vr.height()) / frame.height();
            double ox = vr.x(), oy = vr.y();
            for (const QPointF& pt : zone) mapped << QPointF(pt.x() * sx + ox, pt.y() * sy + oy);
        } else if (vw > 0 && vh > 0) {
            // 无帧: 用已知分辨率等比映射到控件(棋盘格满幅)
            QSize s = QSize(vw, vh).scaled(size(), Qt::KeepAspectRatio);
            int x0 = (width() - s.width()) / 2, y0 = (height() - s.height()) / 2;
            double sx = double(s.width()) / vw;
            double sy = double(s.height()) / vh;
            for (const QPointF& pt : zone) mapped << QPointF(pt.x() * sx + x0, pt.y() * sy + y0);
        } else {
            mapped = zone;   // 未知分辨率: 按原始坐标画(少见)
        }
        p.setBrush(QColor(255, 153, 0, 60));
        p.setPen(QPen(QColor(255, 153, 0, 200), 2));
        p.drawPolygon(mapped);
    }
}

// ---------------- 工业风样式 (暗黑主题) ----------------
static const char* const STYLE_SHEET = R"(
QMainWindow, QDialog { background-color: #2B2B2B; }
QWidget { color: #E0E0E0; }
QToolButton {
    background: transparent; border: none; border-radius: 4px;
    padding: 6px 10px; font-size: 13px; color: #CCCCCC;
}
/* 顶部功能按钮: 文字橙色 + 一圈轮廓边框便于分辨点击区域 */
#topBar QToolButton {
    color: #FF9900;
    border: 1px solid #5A5A5A;
    border-radius: 6px;
    padding: 4px 12px;
}
#topBar QToolButton:hover { background: #3A3A3A; border-color: #FF9900; }
#topBar QToolButton:pressed { background: #454545; }
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
#statusBar #statusDot { color: #00FF00; font-weight: bold; }
#statusBar #statusDot[bad="true"] { color: #FFFF00; }
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
QToolButton#startBtn { background-color: #00B027; color: #FFFFFF; border: none; font-weight: bold; }
QToolButton#stopBtn  { background-color: #D32F2F; color: #FFFFFF; border: none; font-weight: bold; }
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
    // 无边框: 去掉系统标题栏(最小化/最大化/关闭由自建顶部横条右侧按钮提供)
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);

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

// ---------------- UI ----------------
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
    buildStatusBar();
    statusBar()->setSizeGripEnabled(false);
}

void IndustrialUI::buildTopBar() {
    // 顶部工具栏: 左 Logo, 中按钮组, 右窗口控制
    toolbar_widget = new QWidget();
    toolbar_widget->setObjectName("topBar");
    toolbar_widget->setFixedHeight(64);
    QHBoxLayout* lay = new QHBoxLayout(toolbar_widget);
    lay->setContentsMargins(12, 0, 12, 0);
    lay->setSpacing(6);

    // 左侧 Logo + 名称
    QLabel* logo = new QLabel();
    QPixmap logo_pm(":/JEE.png");
    if (!logo_pm.isNull()) {
        logo_pm = logo_pm.scaledToHeight(44, Qt::SmoothTransformation);
        logo->setPixmap(logo_pm);
    } else {
        logo->setText(QString::fromUtf8("JEE"));
        logo->setStyleSheet(QString::fromUtf8("background:#FF9900; color:#000000; font-weight:bold; "
                                              "font-size:20px; padding:4px 12px; border-radius:4px;"));
    }
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
        {u8"开始", "▶"}, {u8"停止", "■"},
    };
    for (int i = 0; i < 8; ++i) {
        QToolButton* b = new QToolButton(toolbar_widget);
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setText(QString::fromUtf8(btns[i].text));
        b->setIcon(QIcon());  // 单色线条图标可后续替换, 暂用文字
        if (i < 6) {
            b->setFixedSize(72, 52);   // 功能按钮
        } else {
            b->setFixedSize(52, 40);   // 开始/停止: 调小
            b->setStyleSheet(QString::fromUtf8(
                "background:#00B027; color:#FFFFFF; font-weight:bold; border-radius:6px;"
                "font-size:13px; padding:0 4px;"));
        }
        if (i == 7) { b->setObjectName("stopBtn"); b->setStyleSheet(QString::fromUtf8("background:#D32F2F; color:#FFFFFF; font-weight:bold; border-radius:6px; font-size:13px; padding:0 4px;")); }
        lay->addWidget(b);
        // 信号连接
        switch (i) {
        case 0: connect(b, &QToolButton::clicked, this, &IndustrialUI::pickVideo); break;
        case 1: connect(b, &QToolButton::clicked, this, &IndustrialUI::openSettings); break;
        case 2: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCameraDlg); break;
        case 3: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCommDlg); break;
        case 4: connect(b, &QToolButton::clicked, this, &IndustrialUI::openQuery); break;
        case 5: connect(b, &QToolButton::clicked, this, &IndustrialUI::openCanvasDlg); break;
        case 6: btn_start = b; connect(b, &QToolButton::clicked, this, &IndustrialUI::start); break;
        case 7: btn_stop = b; btn_stop->setEnabled(false); connect(b, &QToolButton::clicked, this, &IndustrialUI::stop); break;
        }
    }
    lay->addStretch(1);

    // 右侧窗口控制 (最小化/最大化/关闭)
    QToolButton* b_min = new QToolButton(toolbar_widget);
    b_min->setText(QString::fromUtf8("—"));
    b_min->setToolTip(QString::fromUtf8("最小化"));
    connect(b_min, &QToolButton::clicked, this, &QWidget::showMinimized);
    QToolButton* b_max = new QToolButton(toolbar_widget);
    b_max->setText(QString::fromUtf8("□"));
    b_max->setToolTip(QString::fromUtf8("最大化"));
    connect(b_max, &QToolButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
    });
    QToolButton* b_close = new QToolButton(toolbar_widget);
    b_close->setText(QString::fromUtf8("✕"));
    b_close->setToolTip(QString::fromUtf8("关闭"));
    connect(b_close, &QToolButton::clicked, this, &QWidget::close);
    lay->addWidget(b_min);
    lay->addWidget(b_max);
    lay->addWidget(b_close);
}

void IndustrialUI::buildMainArea() {
    main_layout = new QHBoxLayout();
    main_layout->setContentsMargins(10, 10, 10, 10);
    main_layout->setSpacing(10);

    // ---- 左侧: 渲染结果区(棋盘格) ----
    QWidget* left = new QWidget();
    QVBoxLayout* left_lay = new QVBoxLayout(left);
    left_lay->setContentsMargins(0, 0, 0, 0);
    left_lay->setSpacing(8);

    // 渲染区: 棋盘格背景 + 视频帧 + 围栏叠加 (RenderView 自绘, 见 paintEvent)
    lbl_video = new RenderView();
    lbl_video->setObjectName("renderView");
    lbl_video->setMinimumSize(700, 460);
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

    lbl_ok_card = new QLabel(QString::fromUtf8("----"));
    lbl_ok_card->setObjectName("okCard");
    lbl_ok_card->setProperty("state", "idle");
    lbl_ok_card->setAlignment(Qt::AlignCenter);
    lbl_ok_card->setMinimumHeight(200);
    lbl_ok_card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    right_lay->addWidget(lbl_ok_card);

    QGroupBox* ev = new QGroupBox(QString::fromUtf8("事件记录"));
    QWidget* ev_wrap = new QWidget(ev);   // 表格 + 空态提示叠在同一格
    QGridLayout* ev_gl = new QGridLayout(ev_wrap);
    ev_gl->setContentsMargins(0, 0, 0, 0);
    tbl_events = new QTableWidget(0, 4, ev_wrap);
    tbl_events->setHorizontalHeaderLabels(
        QStringList() << QString::fromUtf8("序号") << QString::fromUtf8("时间")
                      << QString::fromUtf8("事件") << QString::fromUtf8("位置"));
    tbl_events->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tbl_events->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl_events->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl_events->verticalHeader()->setVisible(false);
    // 空态提示 (规格 §4.4): 0 行时叠加显示, addEvent 时隐藏
    lbl_empty_events = new QLabel(QString::fromUtf8("暂无事件记录"), ev_wrap);
    lbl_empty_events->setAlignment(Qt::AlignCenter);
    lbl_empty_events->setStyleSheet(QString::fromUtf8(
        "background: rgba(255,255,255,190); color:#666666; font-size:14px;"));
    lbl_empty_events->raise();
    ev_gl->addWidget(tbl_events, 0, 0);
    ev_gl->addWidget(lbl_empty_events, 0, 0);
    QVBoxLayout* ev_lay = new QVBoxLayout(ev);
    ev_lay->setContentsMargins(6, 6, 6, 6);
    ev_lay->addWidget(ev_wrap);
    right_lay->addWidget(ev, 1);
    main_layout->addWidget(right, 2);
}

void IndustrialUI::buildStatusBar() {
    statusBar()->setObjectName("statusBar");
    lbl_user = new QLabel(QString::fromUtf8("用户: ") + qgetenv("USER"));
    lbl_scheme = new QLabel(QString::fromUtf8("方案: 电子围栏"));
    lbl_plc = new QLabel(QString::fromUtf8("● PLC连接: 错误"));
    lbl_plc->setObjectName("statusDot");
    lbl_plc->setProperty("bad", "true");
    lbl_cam = new QLabel(QString::fromUtf8("● 相机连接: 未启动"));
    lbl_cam->setObjectName("statusDot");
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

// ---- 无边框窗口拖动/缩放 (无系统标题栏) ----
// 缩放用 Windows 原生 WM_NCHITTEST: 系统自己处理边缘检测/光标/拖动, 不受子控件遮挡影响
bool IndustrialUI::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef _WIN32
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCHITTEST) {
        const int BORDER = 8;
        RECT rc;
        GetWindowRect(msg->hwnd, &rc);
        int x = GET_X_LPARAM(msg->lParam), y = GET_Y_LPARAM(msg->lParam);
        bool l = x < rc.left + BORDER, r = x > rc.right - BORDER;
        bool t = y < rc.top + BORDER, b = y > rc.bottom - BORDER;
        *result = l && t ? HTTOPLEFT : r && t ? HTTOPRIGHT :
                  l && b ? HTBOTTOMLEFT : r && b ? HTBOTTOMRIGHT :
                  l ? HTLEFT : r ? HTRIGHT : t ? HTTOP : b ? HTBOTTOM : 0;
        if (*result) return true;   // 边缘: 交给系统缩放
    }
#else
    Q_UNUSED(eventType); Q_UNUSED(message); Q_UNUSED(result);
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

// 顶部横条拖动 (鼠标事件只在无子控件区域/空白处能到达主窗口, 足够拖动)
void IndustrialUI::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton &&
        toolbar_widget && toolbar_widget->geometry().contains(ev->pos())) {
        drag_offset = ev->globalPos() - frameGeometry().topLeft();
        ev->accept();
        return;
    }
    QMainWindow::mousePressEvent(ev);
}

void IndustrialUI::mouseMoveEvent(QMouseEvent* ev) {
    if (ev->buttons() & Qt::LeftButton && !drag_offset.isNull()) {
        move(ev->globalPos() - drag_offset);
        ev->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(ev);
}

void IndustrialUI::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) drag_offset = QPoint();
    QMainWindow::mouseReleaseEvent(ev);
}

// ---------------- 工具 ----------------
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

void IndustrialUI::setDefaultVideo() {
    if (!var["video"].isEmpty()) return;   // 已有上次设置的视频, 不覆盖
    QStringList vids = listVideos();
    if (!vids.isEmpty()) {
        var["video"] = "/home/orangepi/Videos/" + vids.first();
        log(QString::fromUtf8("默认视频(第一个): ") + vids.first());
    } else {
        var["video"] = "";
        log(QString::fromUtf8("未在 /home/orangepi/Videos 找到视频文件, 请手动选择"));
    }
}

void IndustrialUI::loadZone() {
    // 读取 models/zone.txt 围栏顶点(x,y 每行, # 注释), 画到渲染区 (main_MultiDetect.cpp 同格式)
    QPolygonF poly;
    QFile f(zonePath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            QStringList xy = line.split(',');
            if (xy.size() >= 2) {
                bool okx = false, oky = false;
                double x = xy[0].trimmed().toDouble(&okx);
                double y = xy[1].trimmed().toDouble(&oky);
                if (okx && oky) poly << QPointF(x, y);
            }
        }
        f.close();
    }
    if (poly.size() >= 3) {
        lbl_video->setZonePolygon(poly);
        log(QString::fromUtf8("围栏已加载: %1 个顶点 (%2)").arg(poly.size()).arg(zonePath()));
    } else {
        lbl_video->setZonePolygon(QPolygonF());
        if (QFileInfo::exists(zonePath()))
            log(QString::fromUtf8("围栏文件顶点不足 3 个, 不叠加: ") + zonePath());
    }
}

QString IndustrialUI::zonePath() const {
    return QDir::cleanPath(QDir(var["cwd"]).filePath("../models/zone.txt"));
}

QStringList IndustrialUI::cmd() const {
    QStringList c;
    if (var["sudo"] == "true") c << "sudo" << "-n" << "./Detect";
    else c << "./Detect";
    c << "--no-display" << "--gui-frame" << var["frame"]
      << "--file" << var["video"] << "--model" << var["model"];
    QString m = var["mode"];
    if (m == "no-fp") c << "--no-fp";
    else if (m == "fp-only") c << "--fp-only";
    c << "--zone-mode" << var["zone"] << "--fp-save" << var["fpdir"]
      << "--conf" << var["conf"];
    // 抽帧间隔: 每 N 帧视频帧检测 1 次 (传给 Detect --detect-interval, 非法忽略)
    bool ok = false;
    int iv = var["interval"].trimmed().toInt(&ok);
    if (ok && iv >= 1) c << "--detect-interval" << QString::number(iv);
    // 阈值判断帧数(检测次数): 传给 Detect --fp-frames/--miss-frames/--zone-confirm
    int fpv = var["fp_frames"].trimmed().toInt(&ok);
    if (ok && fpv >= 1) c << "--fp-frames" << QString::number(fpv);
    int msv = var["miss_frames"].trimmed().toInt(&ok);
    if (ok && msv >= 1) c << "--miss-frames" << QString::number(msv);
    int zcv = var["zone_confirm"].trimmed().toInt(&ok);
    if (ok && zcv >= 1) c << "--zone-confirm" << QString::number(zcv);
    int zdv = var["zone_debounce"].trimmed().toInt(&ok);
    if (ok && zdv >= 1) c << "--zone-debounce" << QString::number(zdv);
    return c;
}

QStringList IndustrialUI::drawCmd() const {
    QStringList c;
    c << "./Detect" << "--draw-zone-only" << "--file" << var["video"];
    return c;
}

// ---------------- 控制 ----------------
void IndustrialUI::start() {
    stop();
    // 勾选"使用sudo"但界面不是 root 时: Detect 会以 root 运行, 普通用户界面杀不掉它 → 自动取消并提示
    bool not_root = false;
#ifndef _WIN32
    not_root = (::geteuid() != 0);
#endif
    if (var["sudo"] == "true" && not_root) {
        var["sudo"] = "false";
        log(QString::fromUtf8("[警告] 勾选了「使用sudo」但界面非 root 启动: Detect 会以 root 运行, 停止按钮将失效。"
                              "已自动取消勾选, 以普通用户运行 Detect(NPU 无需 root)。如需 root 请用 sudo ./DetectUI2 启动界面。"));
    }
    saveSettings();   // 记录本次使用的模型/阈值等, 下次打开自动恢复
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
#ifndef _WIN32
            if (::kill(-(pid_t)pid, SIGTERM) != 0)
                log(QString::fromUtf8("[停止] 停止信号发送失败(errno=%1): Detect 可能以 root 运行而界面是普通用户, "
                                      "请用 sudo ./DetectUI2 启动界面或取消勾选「使用sudo」")
                        .arg(errno));
#else
            log(QString::fromUtf8("[停止] Windows 预览版: 请求终止子进程 PID %1").arg(pid));
            proc->terminate();
#endif
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
    monitoring = false;
    stopping = true;   // 置位: updateOkCard 视作未运行, 状态卡立即回 idle(不等进程退出)
    refresh();         // running 翻转触发 updateOkCard 刷新状态卡与统计
}

void IndustrialUI::drawZone() {
    stop();
    pending_run_after_draw = false;
    prepareFpDir();   // 清空误检帧栏和旧文件
    log(QString::fromUtf8("进入画围栏模式: 在弹出来的窗口里点4个以上点, 回车保存 / ESC跳过"));
    launch(drawCmd(), "draw");
}

void IndustrialUI::startMonitor() {
    stopping = false;   // 解除 stop() 的 idle 锁定(与 launch 一致)
    monitoring = true;
    mon_path = var["log"];
    mon_fh = new QFile(mon_path);
    if (!mon_fh->open(QIODevice::ReadOnly | QIODevice::Text)) {
        log(QString::fromUtf8("监视: 无法打开 ") + mon_path + QString::fromUtf8(", 文件出现后会自动读取"));
        delete mon_fh;
        mon_fh = nullptr;
        last_mon_retry = -1;   // 立即进入重试节奏(poll 里每 3 秒重试)
    }
    st["video"] = QFileInfo(var["video"]).fileName();
    btn_start->setEnabled(false);
    btn_stop->setEnabled(true);
    statusBar()->showMessage(QString::fromUtf8("监视中: ") + mon_path + QString::fromUtf8(" (Detect 请手动启动)"));
}

void IndustrialUI::launch(const QStringList& cmdline, const QString& kind) {
    stopping = false;   // 新进程启动, 解除 stop() 的 idle 锁定(运行状态改由 proc 判定)
    st["video"] = QFileInfo(var["video"]).fileName();
    log("$ " + cmdline.join(" "));
    proc = new IndustrialProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setWorkingDirectory(var["cwd"]);
    // 压掉 librga 的 RGA_BLIT fail 刷屏(板子 rga_api 1.9.3 与 librga 2.2.0
    // 不兼容, 每帧报错污染日志; 不影响功能, 只是噪音)
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("ROCKCHIP_RGA_LOG", "0");
    env.insert("ROCKCHIP_RGA_LOG_LEVEL", "0");
    proc->setProcessEnvironment(env);
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

// ---------------- 事件 ----------------
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
    updateOkCard();   // 检测停止后状态卡回 idle
}

void IndustrialUI::poll() {
    flushLog();   // 日志节流刷新
    // 监视文件模式: 增量读日志
    if (mon_fh) {
        while (mon_fh->canReadLine()) {
            QByteArray line = mon_fh->readLine();
            handleLine(QString::fromUtf8(line).trimmed());
        }
    } else if (monitoring) {
        // 打开失败/被外部删除: 每 3 秒节流重试一次, 文件出现后自动恢复
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (last_mon_retry < 0 || now - last_mon_retry >= 3000) {
            last_mon_retry = now;
            mon_fh = new QFile(mon_path);
            if (mon_fh->open(QIODevice::ReadOnly | QIODevice::Text)) {
                log(QString::fromUtf8("监视: 已打开日志文件 ") + mon_path);
            } else {
                delete mon_fh;
                mon_fh = nullptr;
            }
        }
    }
    updateVideo();
    if (fp_hbox) updateFpGallery();
    refresh();
}

// ---------------- 解析/刷新 ----------------
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
        // 属性变化后刷新样式(#statusDot[bad="true"] 规则才能生效)
        lbl_cam->style()->unpolish(lbl_cam);
        lbl_cam->style()->polish(lbl_cam);
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

void IndustrialUI::addEvent(const QString& type, const QString& pos) {
    if (!tbl_events) return;
    if (lbl_empty_events) lbl_empty_events->hide();   // 有事件了, 收起空态提示
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
    // 进程运行或监视文件模式进行中都算"运行中"(监视文件模式下 Detect 是外部进程,
    // proc 始终为 null, 靠 mon_fh/monitoring 判断, 否则 zone_now>=1 永远翻不了 NG);
    // stop() 置位 stopping 后一律按未运行处理: 停止瞬间卡片立即回 idle(不等进程退出)
    bool running = ((proc != nullptr) || (mon_fh != nullptr) || monitoring) && !stopping;
    bool ng = running && st["zone_now"].toInt() > 0;
    // 按 (running, ng) 元组比较: running 翻转(开始/停止)与 ng 翻转都会触发刷新;
    // 只按 ng 比较会漏掉"一直 OK → stop"和"从未 NG → start"两种场景
    if (running != was_running || ng != ng_state) {
        was_running = running;
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
    if (lbl_empty_events) lbl_empty_events->show();   // 回到空态提示
    event_seq = 0;
    entered_ids.clear();
    ng_state = false;
    lbl_ok_card->setProperty("state", "idle");
    lbl_ok_card->setText(QString::fromUtf8("----"));
    lbl_ok_card->style()->unpolish(lbl_ok_card);
    lbl_ok_card->style()->polish(lbl_ok_card);
    refresh();
}

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
        lbl_video->setVideoSize(pm.width(), pm.height());
        lbl_video->setFramePixmap(pm);
        tlog(QString::fromUtf8("画面已显示"), true);
    }
}

void IndustrialUI::updateFpGallery() {
    if (!fp_hbox) return;
    QString d = var["fpdir"];
    if (d.isEmpty()) return;
    // 扫描节流: 500ms 一次(误检图约 1 秒一张, 空转扫描纯浪费 CPU)
    qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    if (last_fp_scan > 0 && now_ms - last_fp_scan < 500) return;
    last_fp_scan = now_ms;
    QDir dir(d);
    if (!dir.exists()) return;
    QStringList files = dir.entryList(QStringList("fp_*.jpg"), QDir::Files, QDir::Name);
    for (const QString& f : files) {
        if (f.endsWith(".tmp.jpg")) continue;  // 正在写入的临时文件(通配符一起匹配到了)
        QString full = dir.filePath(f);
        if (fp_shown.contains(full)) continue;
        QPixmap pm(full);
        if (pm.isNull()) continue;   // 未写完的帧(可能瞬间被 .tmp.jpg 重命名)跳过, 下轮重试
        fp_shown.insert(full);
        pm = pm.scaled(200, 112, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        ClickableLabel2* lbl = new ClickableLabel2();
        lbl->setPixmap(pm);
        lbl->setFixedSize(200, 112);
        lbl->setToolTip(f + QString::fromUtf8("（点击放大）"));
        lbl->setCursor(Qt::PointingHandCursor);
        connect(lbl, &ClickableLabel2::clicked, this,
                [this, full]() { showFpImage(full); });
        fp_hbox->addWidget(lbl);
        // 只显示最新 15 张: 超出时摘掉最旧的缩略图。
        // 文件仍保留在磁盘目录(fp_shown 不删, 不会重复添加), 只是界面不展示。
        while (fp_hbox->count() > 15) {
            QLayoutItem* item = fp_hbox->takeAt(0);   // 最旧的加在尾部 -> 头部是最旧
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
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

void IndustrialUI::onDrawDone() {
    QString zpath = zonePath();
    if (QFileInfo::exists(zpath)) loadZone();   // 重新读取围栏并叠加到渲染区
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
    QString d = var["fpdir"];
    if (d.isEmpty()) return;
    if (!QDir().mkpath(d)) {
        log(QString::fromUtf8("误检目录准备失败: mkpath ") + d);
        return;
    }
    QStringList olds = QDir(d).entryList(QStringList("fp_*.jpg"), QDir::Files);
    int fail = 0;
    for (const QString& f : olds) {
        if (f.endsWith(".tmp.jpg")) continue;  // 不删未写完的临时文件
        if (!QFile::remove(QDir(d).filePath(f))) fail++;
    }
    if (fail > 0)
        log(QString::fromUtf8("误检帧目录: %1 个文件无法删除(权限?), 可执行 sudo chown orangepi:orangepi %2")
                .arg(fail).arg(d));
    // 清空界面上的旧缩略图 (数据查询对话框可能从未打开, fp_hbox 为 null)
    if (fp_hbox) {
        while (fp_hbox->count()) {
            QLayoutItem* item = fp_hbox->takeAt(0);
            if (QWidget* w = item->widget()) w->deleteLater();
            delete item;
        }
    }
    fp_shown.clear();
    last_fp_scan = -1;   // 清空后立即恢复扫描(否则 500ms 节流内看不到新误检图)
    log(QString::fromUtf8("误检帧目录已清空并准备: ") + d);
}

void IndustrialUI::log(const QString& text) {
    // 节流: 只入缓冲, 由 poll() 每 120ms 合并刷新一次, 避免高频日志(如 Detect
    // 每秒 30 行的 [Frame ...])让 logEdit 反复重排拖卡 UI 线程
    if (log_dirty) pending_log += "\n";
    pending_log += text;
    log_dirty = true;
    // 无头调试: 设置 DETECTUI_TIMELOG=1 时把日志镜像到 stderr,
    // 便于 SSH 无显示环境抓完整时间线(启动耗时/点击/视频信息/画面已显示/首帧)
    if (qgetenv("DETECTUI_TIMELOG") == "1") {
        fprintf(stderr, "%s\n", text.toUtf8().constData());
        fflush(stderr);
    }
}

// poll() 里统一刷新缓冲日志(节流合并); 界面无显示/阻塞时可调此兜底
void IndustrialUI::flushLog() {
    if (log_dirty && logEdit) {
        logEdit->appendPlainText(pending_log);
        QScrollBar* sb = logEdit->verticalScrollBar();
        sb->setValue(sb->maximum());
        pending_log.clear();
        log_dirty = false;
        return;
    }
    // 控制台对话框关闭期间 logEdit 为 null: pending_log 只进不出, 必须截断防无界增长
    if (log_dirty && pending_log.size() > 200000) {
        // 丢掉最旧的一半(行首截断到 50% 处, 保留尾部最新日志)
        int half = pending_log.size() / 2;
        int cut = pending_log.indexOf('\n', half);
        if (cut < 0) cut = half;
        pending_log = pending_log.mid(cut + 1);
        pending_log.prepend(QString::fromUtf8("... [日志缓冲超限, 已截断丢弃旧日志] ...\n"));
    }
}

void IndustrialUI::tlog(const QString& tag, bool once) {
    if (t_click <= 0) return;   // 尚未点击启动, 不计时
    if (once && t_logged.contains(tag)) return;
    t_logged.insert(tag);
    double dt = (QDateTime::currentMSecsSinceEpoch() - t_click) / 1000.0;
    log(QString::fromUtf8("[计时] %1 (+%2s)").arg(tag).arg(dt, 0, 'f', 2));
}

// ---------------- 对话框 ----------------
void IndustrialUI::pickVideo() {
    QString p = QFileDialog::getOpenFileName(this, QString::fromUtf8("选择视频"), "/home/orangepi/Videos");
    if (!p.isEmpty()) var["video"] = p;
}

void IndustrialUI::openSettings() {
    if (proc && proc->state() != QProcess::NotRunning) {
        QMessageBox::information(this, QString::fromUtf8("系统参数"),
            QString::fromUtf8("检测运行中, 请先停止再修改参数。"));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("系统参数"));
    dlg.resize(620, 720);
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

    // 围栏判定模式: radio 只用于展示/选择, 点击不写 var["zone"],
    // 仅 Accept 时读取选中项写回 (Cancel 不影响当前配置, 也不留悬挂指针)
    QGroupBox* zb = new QGroupBox(QString::fromUtf8("围栏判定"));
    QHBoxLayout* zl = new QHBoxLayout(zb);
    QRadioButton* rb_hybrid = new QRadioButton(QString::fromUtf8("混合(脚点+面积,推荐)"));
    QRadioButton* rb_foot = new QRadioButton(QString::fromUtf8("脚点"));
    QRadioButton* rb_area = new QRadioButton(QString::fromUtf8("面积IoB"));
    rb_hybrid->setChecked(var["zone"] == "hybrid");
    rb_foot->setChecked(var["zone"] == "foot");
    rb_area->setChecked(var["zone"] == "area");
    zl->addWidget(rb_hybrid);
    zl->addWidget(rb_foot);
    zl->addWidget(rb_area);
    v->addWidget(zb);

    // PLC 状态: 正常/错误, Accept 时写 var["plc"] 并同步状态栏指示灯
    QGroupBox* pb = new QGroupBox(QString::fromUtf8("PLC 通信"));
    QHBoxLayout* pl = new QHBoxLayout(pb);
    QLabel* pl_lab = new QLabel(QString::fromUtf8("连接状态:"));
    QComboBox* plc_combo = new QComboBox();
    plc_combo->addItem(QString::fromUtf8("正常"), "ok");
    plc_combo->addItem(QString::fromUtf8("错误"), "error");
    int pi = plc_combo->findData(var["plc"]);
    if (pi >= 0) plc_combo->setCurrentIndex(pi);
    pl->addWidget(pl_lab);
    pl->addWidget(plc_combo);
    pl->addStretch(1);
    v->addWidget(pb);

    // 使用 sudo (与相机设备对话框共用 var["sudo"])
    QCheckBox* sudo_box = new QCheckBox(QString::fromUtf8("使用sudo运行 Detect(需免密sudo)"));
    sudo_box->setChecked(var["sudo"] == "true");
    v->addWidget(sudo_box);

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
        // 围栏判定: 读取选中 radio (仅 Accept 时写回, Cancel 不生效)
        if (rb_foot->isChecked()) var["zone"] = "foot";
        else if (rb_area->isChecked()) var["zone"] = "area";
        else var["zone"] = "hybrid";
        // PLC 状态: 写 var 并同步状态栏指示灯 (含 unpolish/polish 刷新样式)
        var["plc"] = plc_combo->currentData().toString();
        if (var["plc"].isEmpty()) var["plc"] = "error";
        bool plc_ok = (var["plc"] == "ok");
        lbl_plc->setText(QString::fromUtf8("● PLC连接: ") +
                         (plc_ok ? QString::fromUtf8("正常") : QString::fromUtf8("错误")));
        lbl_plc->setProperty("bad", plc_ok ? "false" : "true");
        lbl_plc->style()->unpolish(lbl_plc);
        lbl_plc->style()->polish(lbl_plc);
        // 使用 sudo
        var["sudo"] = sudo_box->isChecked() ? "true" : "false";
        lbl_scheme->setText(QString::fromUtf8("方案: ") + var["scheme"]);
        saveSettings();
        log(QString::fromUtf8("[设置] 已保存: 模型=%1 阈值=%2 视频=%3")
                .arg(var["model"], var["conf"], var["video"]));
    }
    // 释放对话框内控件, 防悬挂指针 (radio/combo/checkbox 为对话框子控件, 随对话框销毁)
    var_file = var_model = var_cwd = var_conf = var_interval = nullptr;
    var_fp_frames = var_miss_frames = var_zone_confirm = var_zone_debounce = nullptr;
    var_log = var_frame = var_fpdir = var_zone_name = var_scheme = nullptr;
}

void IndustrialUI::openCameraDlg() {
    // 可配置对话框: 运行方式(本软件启动/监视文件) + 使用sudo, Accept 写 var["src"]/var["sudo"]
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("相机设备"));
    dlg.resize(400, 220);
    QVBoxLayout* v = new QVBoxLayout(&dlg);
    QGroupBox* gb = new QGroupBox(QString::fromUtf8("运行方式"));
    QVBoxLayout* gl = new QVBoxLayout(gb);
    QRadioButton* rb_run = new QRadioButton(QString::fromUtf8("本软件启动(自动拉起 Detect)"));
    QRadioButton* rb_mon = new QRadioButton(QString::fromUtf8("监视文件(Detect 手动启动, 界面读取日志)"));
    rb_run->setChecked(var["src"] != "monitor");
    rb_mon->setChecked(var["src"] == "monitor");
    gl->addWidget(rb_run);
    gl->addWidget(rb_mon);
    v->addWidget(gb);
    QCheckBox* sudo_box = new QCheckBox(QString::fromUtf8("使用sudo运行 Detect(需免密sudo)"));
    sudo_box->setChecked(var["sudo"] == "true");
    v->addWidget(sudo_box);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);
    if (dlg.exec() == QDialog::Accepted) {
        var["src"] = rb_mon->isChecked() ? "monitor" : "run";
        var["sudo"] = sudo_box->isChecked() ? "true" : "false";
        saveSettings();
        log(QString::fromUtf8("[设置] 相机运行方式: %1, sudo: %2")
                .arg(var["src"] == "monitor" ? QString::fromUtf8("监视文件") : QString::fromUtf8("本软件启动"),
                     var["sudo"] == "true" ? QString::fromUtf8("开") : QString::fromUtf8("关")));
    }
}

void IndustrialUI::openCommDlg() {
    QMessageBox::information(this, QString::fromUtf8("外部通信"),
        QString::fromUtf8("PLC 连接状态: %1 (在系统参数中配置)").arg(var["plc"]));
}

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

    // Tab2: 控制台输出 (类成员, 父对象是 main window, 跨次打开保留历史)
    QWidget* log_tab = new QWidget();
    QVBoxLayout* lv = new QVBoxLayout(log_tab);
    if (!logEdit) {
        logEdit = new QPlainTextEdit(this);
        logEdit->setReadOnly(true);
        logEdit->setMaximumBlockCount(2000);
    }
    // 复用已有 logEdit: 从 main window(上次关闭后挂回的位置)移进新对话框的 tab
    if (logEdit->parentWidget() != log_tab) logEdit->setParent(log_tab);
    logEdit->show();
    lv->addWidget(logEdit);
    // 打开时把对话框关闭期间积压的日志补上
    flushLog();
    tabs->addTab(log_tab, QString::fromUtf8("控制台"));

    dlg.exec();
    // 把 logEdit 从对话框层级移回 main window, 否则对话框析构会连带删除它(成员变悬挂指针);
    // 移出后隐藏, 不显示在主窗口上。历史内容在成员里保留, 下次打开复用
    if (logEdit) {
        logEdit->setParent(this);
        logEdit->hide();
    }
    // 关闭后断开画廊指针, 防 poll 写已销毁控件
    fp_scroll = nullptr; fp_container = nullptr; fp_hbox = nullptr;
}

void IndustrialUI::openCanvasDlg() {
    QMessageBox::information(this, QString::fromUtf8("画布设置"),
        QString::fromUtf8("围栏名称: %1\n画布背景: 棋盘格").arg(var["zone_name"]));
}

// ---------------- 设置持久化 ----------------
QString IndustrialUI::settingsPath() const {
    return QCoreApplication::applicationDirPath() + "/detectui2.ini";
}

void IndustrialUI::loadSettings() {
    QSettings s(settingsPath(), QSettings::IniFormat);
    var["video"] = s.value("video", QString("")).toString();
    var["model"] = s.value("model", QString("yolov26s_v2_int8.rknn")).toString();
    // Detect 只认 ../models/ 下的文件名: 规范化掉路径, 只留文件名 (跨平台: 兼容 \ 与 /)
    QString savedModel = var["model"];
    int slash = qMax(savedModel.lastIndexOf('/'), savedModel.lastIndexOf('\\'));
    if (slash >= 0) var["model"] = savedModel.mid(slash + 1);
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
    var["sudo"] = s.value("sudo", false).toBool() ? "true" : "false";
    lbl_scheme->setText(QString::fromUtf8("方案: ") + var["scheme"]);
    lbl_plc->setText(QString::fromUtf8("● PLC连接: ") +
                     (var["plc"] == "ok" ? QString::fromUtf8("正常") : QString::fromUtf8("错误")));
    lbl_plc->setProperty("bad", var["plc"] != "ok" ? "true" : "false");
    lbl_plc->style()->unpolish(lbl_plc);
    lbl_plc->style()->polish(lbl_plc);
    // 上次视频为空时自动选 /home/orangepi/Videos 第一个视频 (detect_ui.cpp 同逻辑)
    setDefaultVideo();
    // 读取围栏多边形叠加到渲染区 (规格 §4.2)
    loadZone();
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
    s.setValue("sudo", var["sudo"] == "true");
    s.sync();
}

// ---------------- 退出 ----------------
void IndustrialUI::closeEvent(QCloseEvent* ev) {
    flushLog();   // 退出前把缓冲日志刷完
    saveSettings();
    QMainWindow::closeEvent(ev);
}

// ---------------- 无头/启动辅助 ----------------
void IndustrialUI::appendLog(const QString& text) {
    log(text);
}

void IndustrialUI::autoStart() {
    start();   // 等价于用户点击"启动"按钮(zone.txt 已存在则直接启动 Detect)
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

    IndustrialUI ui;
    t_ctor = start.elapsed() / 1000.0;            // [细分] IndustrialUI 构造(buildUi/loadSettings)
    ui.show();
    t_show = start.elapsed() / 1000.0;            // [细分] show() 到控件映射完成

    double sec = t_show;
    QString msg = QString::fromUtf8("[启动] 软件界面启动耗时: %1 秒").arg(sec, 0, 'f', 2);
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
    fprintf(stderr, "[细分] QApplication 构造: %.3fs, IndustrialUI 构造: %.3fs, show(): %.3fs\n",
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
