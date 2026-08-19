#ifndef INDUSTRIAL_UI_H
#define INDUSTRIAL_UI_H

#include <QByteArray>
#include <QLabel>
#include <QMainWindow>
#include <QMap>
#include <QPolygon>
#include <QPixmap>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

class QCheckBox;
class QCloseEvent;
class QFile;
class QHBoxLayout;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QTableWidget;
class QTimer;
class QToolButton;

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

// 渲染区: 棋盘格背景 + 视频帧(KeepAspectRatio 等比居中)
// (围栏由 Detect 绘制在视频帧上, 界面不自行叠加避免重影)
class RenderView : public QWidget {
public:
    explicit RenderView(QWidget* parent = nullptr) : QWidget(parent) {}
    void setFramePixmap(const QPixmap& pm) { frame = pm; update(); }
protected:
    void paintEvent(QPaintEvent* ev) override;
private:
    QPixmap frame;          // 当前视频帧(原始尺寸, paintEvent 内等比缩放)
};

// 可省略的 QLabel: 宽度不足时用省略号(...)替代被裁剪的文字
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const QString& text, QWidget* parent = nullptr)
        : QLabel(text, parent), full_text(text) {}
    void setFullText(const QString& t) { full_text = t; setText(t); }
protected:
    void paintEvent(QPaintEvent* ev) override;
private:
    QString full_text;
};

// RK3588 电子围栏系统界面 (JEEFence)
// 布局按 ui/电子围栏系统界面设计.md 参考图, 逻辑复用现有 Detect 日志协议
class IndustrialUI : public QMainWindow {
    Q_OBJECT
public:
    explicit IndustrialUI(QWidget* parent = nullptr);

    void appendLog(const QString& text);   // main() 启动日志用
    void autoStart();                       // --auto-start: 模拟点击"启动"(无头测试用)

private:
    // ---- UI 控件 ----
    QWidget* toolbar_widget = nullptr;  // 顶部横条(Logo+名称+窗口控制)
    QWidget* sidebar_widget = nullptr;  // 左侧竖排功能按钮栏
    RenderView* lbl_video = nullptr;    // 渲染区(棋盘格 + 视频帧 + 围栏叠加)
    QLabel* lbl_ok_card = nullptr;      // OK/NG 状态卡
    QLabel* lbl_video_info = nullptr;   // 视频信息: 处理/实时FPS/分辨率/帧号
    QTableWidget* tbl_events = nullptr; // 事件数据表
    QLabel* lbl_empty_events = nullptr; // 事件表空态提示(与表格同格叠加)
    QPlainTextEdit* logEdit = nullptr;  // 控制台输出(类成员, 父对象是 main window, 跨次打开保留历史)
    QScrollArea* fp_scroll = nullptr;   // 误检帧画廊(对话框内)
    QWidget* fp_container = nullptr;
    QHBoxLayout* fp_hbox = nullptr;

    QToolButton* btn_start = nullptr;
    QToolButton* btn_stop = nullptr;

    QLabel* lbl_scheme = nullptr;       // 状态栏: 方案名称
    QLabel* lbl_plc = nullptr;          // 状态栏: PLC连接
    QLabel* lbl_cam = nullptr;          // 状态栏: 相机连接
    QLabel* lbl_disk = nullptr;         // 状态栏: 剩余存储
    QLabel* lbl_clock = nullptr;        // 状态栏: 精确时间

    // ---- 工具栏/对话框成员 ----
    QHBoxLayout* main_layout = nullptr;
    // 设置对话框控件 (仅对话框 exec 期间有效, 关闭后置 nullptr;
    // 运行逻辑一律读取 var map, 不直接解引用这些指针)
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
    bool monitoring = false;            // 监视文件模式进行中 (mon_fh 为 null 时按此重试)
    bool stopping = false;              // stop() 已执行: updateOkCard 视作未运行, 卡片立即回 idle
    qint64 last_mon_retry = -1;         // 监视文件打开失败的重试节流时间戳(ms)
    bool ng_state = false;              // 当前 NG 状态 (围栏内人数>=1)
    bool was_running = false;           // 上一次 updateOkCard 的运行状态 (与 ng_state 组成元组比较)
    int event_seq = 0;                  // 事件表序号

    // ---- 界面构建 ----
    void buildUi();
    void buildTopBar();
    void buildSideBar();
    void buildMainArea();
    void buildStatusBar();
    void updateDiskInfo();

    // ---- 无边框窗口拖动/缩放 ----
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;   // 持续记录正常窗口位置(最小化还原用)
    void changeEvent(QEvent* ev) override;   // 监听激活变化: 最小化(缩小)状态下窗口重新激活时自动还原
    bool nativeEvent(const QByteArray& eventType, void* message, long* result) override;
    QPoint drag_offset;                 // 拖动窗口时的鼠标偏移
    // 无 WM(X11 转发)环境下的最大化/最小化模拟 + 优先尝试真 showMinimized/showMaximized 让 WM 处理
    void toggleMaximize();              // 最大化/还原(手动铺满主屏)
    void doMinimize();                  // 最小化(WM 可用时 showMinimized; 否则缩小到角标)
    void doRestore();                   // 还原(WM 可用时 showNormal; 否则恢复几何)
    void startMinimizePoll();           // 缩小角标模式的激活轮询(点任务栏图标自动还原)
    QRect mainScreenGeom();             // 主屏(锁定 2.5K 屏)可用区; 不再依赖 Qt primaryScreen
    QScreen* lockedMainScreen();       // 启动时锁定"用户主屏": 优先匹配 2560x1440, 否则最宽屏
    void diagnoseScreens();             // 启动时打印 Qt 看到的所有 screen 信息(诊断)
    QScreen* main_screen = nullptr;    // 锁定的 2.5K 主屏指针(避免每次重新扫描)
    bool win_maximized = false;         // 当前是否"最大化"
    bool win_minimized = false;         // 当前是否"最小化"
    QRect normal_geo;                   // 最大化前保存的正常几何

    // ---- 工具 ----
    QStringList listVideos() const;
    void setDefaultVideo();
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

    // ---- 槽(对话框) ----
    void pickVideo();
    void openSettings();
    void openCameraDlg();
    void openCommDlg();
    void openQuery();
    void openCanvasDlg();
};

#endif // INDUSTRIAL_UI_H
