#ifndef DETECT_UI_H

#define DETECT_UI_H



#include <QByteArray>

#include <QLabel>

#include <QMainWindow>

#include <QMap>

#include <QProcess>

#include <QSet>

#include <QString>

#include <QStringList>



class QCheckBox;

class QCloseEvent;

class QFile;

class QHBoxLayout;

class QLabel;

class QLineEdit;

class QMouseEvent;

class QPlainTextEdit;

class QPushButton;

class QRadioButton;

class QScrollArea;

class QTimer;



// 可点击的图片标签(误检帧缩略图, 点击放大)

class ClickableLabel : public QLabel {

    Q_OBJECT

public:

    explicit ClickableLabel(QWidget* parent = nullptr) : QLabel(parent) {}

signals:

    void clicked();

protected:

    void mousePressEvent(QMouseEvent* ev) override;

};



// RK3588 电子围栏 - 检测信息软件 (detect_ui.py 的 C++ Qt 移植版, 界面/逻辑 1:1)

class DetectUI : public QMainWindow {

    Q_OBJECT

public:

    explicit DetectUI(QWidget* parent = nullptr);

    void appendLog(const QString& text);   // main() 启动日志用

    void autoStart();                       // --auto-start: 模拟点击"启动"(无头测试用)



private:

    // ---- UI 控件 ----

    QLineEdit* var_file = nullptr;

    QLineEdit* var_model = nullptr;

    QLineEdit* var_cwd = nullptr;

    QLineEdit* var_conf = nullptr;

    QLineEdit* var_interval = nullptr;   // 抽帧间隔: 每 N 帧检测 1 次

    QLineEdit* var_fp_frames = nullptr;   // 误检判断帧(检测次数)

    QLineEdit* var_miss_frames = nullptr; // 漏检判断帧(检测次数)

    QLineEdit* var_zone_confirm = nullptr; // 围栏判断帧(检测次数)

    QLineEdit* var_zone_debounce = nullptr; // 围栏防抖(检测次数): 连续N次同侧才翻转进出

    QLineEdit* var_log = nullptr;

    QLineEdit* var_frame = nullptr;

    QLineEdit* var_fpdir = nullptr;

    QCheckBox* var_sudo = nullptr;

    QPushButton* btn_start = nullptr;

    QPushButton* btn_stop = nullptr;

    QPushButton* btn_draw = nullptr;

    QLabel* lbl_video = nullptr;

    QPlainTextEdit* logEdit = nullptr;

    QScrollArea* fp_scroll = nullptr;

    QWidget* fp_container = nullptr;

    QHBoxLayout* fp_hbox = nullptr;

    QMap<QString, QLabel*> lbl;      // 运行信息: key -> value 标签

    QMap<QString, QRadioButton*> radio_btns;  // "key/val" -> 单选按钮(设置恢复用)



    // ---- 运行状态 ----

    QMap<QString, QString> var;       // mode / src / zone

    QMap<QString, QString> st;        // 统计信息

    QSet<QString> fp_shown;           // 已显示的误检帧

    QSet<QString> t_logged;           // 已打印过的计时标记

    QProcess* proc = nullptr;

    QTimer* timer = nullptr;

    QFile* mon_fh = nullptr;

    QByteArray out_buf;               // 子进程输出缓冲

    QString mon_path;

    QString proc_kind;                // "detect" / "draw"

    qint64 frame_mt = -1;             // frame.jpg 修改时间(ms)
    qint64 frame_sz = -1;             // frame.jpg å¤§å°(byte), éåèæµåçåå¸§è¯å«

    qint64 last_fp_scan = -1;         // 误检帧栏上次扫描时间(ms), 500ms 节流


    qint64 t_click = -1;              // 点击"启动"时间(ms), <=0 表示未点击

    QString pending_log;              // 待刷新的日志行缓冲(节流, 每 poll 合并一次)

    bool log_dirty = false;           // pending_log 非空标记

    bool sudo_fail = false;

    bool pending_run_after_draw = false;



    // ---- 界面构建 ----

    void buildUi();

    void setDefaultVideo();

    QStringList listVideos() const;

    QString zonePath() const;

    QStringList cmd() const;

    QStringList drawCmd() const;



    // ---- 控制 ----

    void start();

    void stop();

    void drawZone();

    void startMonitor();

    void launch(const QStringList& cmdline, const QString& kind);



    // ---- 设置持久化 ----

    void loadSettings();

    void saveSettings();

    QString settingsPath() const;



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

    void log(const QString& text);

    void flushLog();                 // 合并刷新缓冲日志(poll 节流用)

    void tlog(const QString& tag, bool once = false);

};



#endif // DETECT_UI_H