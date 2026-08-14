#include <dlfcn.h>



#include <stdio.h>



#include <stdlib.h>



#include <string.h>



#include <sys/time.h>



#include <sys/stat.h>



#include <iostream>



#include <sstream>



#include <iomanip>



#include <cstdarg>



#include <deque>



#include "opencv2/core/core.hpp"



#include "opencv2/imgcodecs.hpp"



#include "opencv2/imgproc.hpp"



#include "opencv2/highgui/highgui.hpp"



#include<fstream>



#include <unordered_set>



#include <set>



#include <map>



#include <thread>

#include <chrono>



#include <queue>  



#include <mutex>  



#include <condition_variable>



#include <atomic>



#include "Detect.h"



#include "postprocess.h"



#include "timer.h"



#include "BYTETracker.h"



#include <arv.h>



using namespace std;



// --- Mouse callback for manual zone drawing ---



// Called while the main window is in draw mode (zone not loaded yet).



// g_draw_scale: display scale factor used while drawing; mouse coords are



// divided by it so zone points are stored in full-frame space.



static std::vector<cv::Point> g_draw_zone;



static std::string g_zone_save_path;



static float g_draw_scale = 1.0f;



static void onMouse(int event, int x, int y, int, void*)



{



    if (event == cv::EVENT_LBUTTONDOWN) {



        cv::Point p((int)(x / g_draw_scale), (int)(y / g_draw_scale));



        g_draw_zone.push_back(p);



        printf("  Zone point %zu: (%d, %d) [frame space]\n", g_draw_zone.size(), p.x, p.y);



    }



}



// IoB: fraction of the box area that lies inside the zone polygon.



// Sampled point-in-polygon (works for concave zones; intersectConvexConvex



// would require convexity). 5x5 grid is accurate enough for zone >> box.



// IoB over the LOWER part of the box: only the bottom `lower_frac` of the box



// height is sampled. A person is considered "in the fence" by their lower body



// (feet/legs), so the upper body sticking out over the fence edge no longer



// triggers an exit. lower_frac=1.0 keeps the old whole-box behaviour.



static float boxIoB(const cv::Rect &r, const std::vector<cv::Point> &zone, float lower_frac = 1.0f)



{



    if (r.width <= 0 || r.height <= 0) return 0.0f;



    const int N = 5;



    int y0 = (int)(r.y + r.height * (1.0f - lower_frac));



    if (y0 < r.y) y0 = r.y;



    int y1 = r.y + r.height;



    if (y0 >= y1) return 0.0f;



    float sub_h = (float)(y1 - y0);



    int inside = 0;



    for (int gy = 0; gy < N; gy++) {



        float y = y0 + (gy + 0.5f) * sub_h / N;



        for (int gx = 0; gx < N; gx++) {



            float x = r.x + (gx + 0.5f) * r.width / N;



            if (cv::pointPolygonTest(zone, cv::Point2f(x, y), false) >= 0)



                inside++;



        }



    }



    return (float)inside / (N * N);



}



// --- Simple on-screen info panel (OSD) ---



// Draws console-style info (video info, detection counts, zone enter/exit,



// FP/miss stats and the latest console events) onto the displayed frame, so



// the operator can read everything without the console window.



static std::deque<std::string> g_event_log;   // recent console events



static const size_t g_event_log_max = 6;



static bool g_show_panel = true;              // toggle with 'i' on the window



// printf + keep the line in the on-screen event log.



static void push_event(const char *fmt, ...)



{



    char buf[512];



    va_list ap;



    va_start(ap, fmt);



    vsnprintf(buf, sizeof(buf), fmt, ap);



    va_end(ap);



    printf("%s", buf);



    std::string s(buf);



    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();



    g_event_log.push_back(s);



    while (g_event_log.size() > g_event_log_max) g_event_log.pop_front();



}



// Build a dashboard-style SIDE panel (video is not covered). The video display



// is shown on the left; this panel is concatenated to its right, like a small



// software UI with a separate info area.



static void buildInfoPanel(cv::Mat &panel, int panel_h,



                           const std::string &vlabel, int vw, int vh, int vfps,



                           int frames, double fps, double det_fps,



                           size_t raw_dets, size_t tracked, int persons,



                           int zone_in, int zone_out, int in_now,



                           float iob_in, float iob_out,



                           int fp_confirmed, int fp_zone, int miss,



                           bool no_fp, bool fp_only, int fp_frames, int miss_frames,



                           const std::deque<std::string> &ev)



{



    const int W = 380;



    panel = cv::Mat::zeros(panel_h, W, CV_8UC3);



    const int FONT = cv::FONT_HERSHEY_SIMPLEX;



    const double FS = 0.42;



    const int TH = 1;



    int y = 0;



    // --- title bar ---



    cv::rectangle(panel, cv::Rect(0, y, W, 26), cv::Scalar(45, 45, 45), cv::FILLED);



    cv::rectangle(panel, cv::Rect(0, y, 6, 26), cv::Scalar(0, 200, 255), cv::FILLED);



    cv::putText(panel, "RK3588 FENCE DETECT", cv::Point(14, 18), FONT, 0.5, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);



    y += 30;



    auto section = [&](const char *name) {



        cv::putText(panel, name, cv::Point(8, y + 12), FONT, 0.42, cv::Scalar(0, 200, 255), 1, cv::LINE_AA);



        y += 16;



    };



    auto row = [&](const char *label, const char *value, const cv::Scalar &vc = cv::Scalar(255, 255, 255)) {



        cv::putText(panel, label, cv::Point(8, y + 12), FONT, FS, cv::Scalar(150, 150, 150), TH, cv::LINE_AA);



        cv::putText(panel, value, cv::Point(118, y + 12), FONT, FS, vc, TH, cv::LINE_AA);



        y += 16;



    };



    char tmp[256];



    section("MODE");



    snprintf(tmp, sizeof(tmp), "%s   (fp>=%d  miss>=%d)",



             no_fp ? "fp OFF" : (fp_only ? "fp only" : "fp+miss"), fp_frames, miss_frames);



    row("mode", tmp, cv::Scalar(0, 255, 255));



    section("VIDEO");



    row("file", vlabel.c_str());



    snprintf(tmp, sizeof(tmp), "%dx%d @ %dfps", vw, vh, vfps);



    row("size", tmp);



    snprintf(tmp, sizeof(tmp), "det %d   %.1f fps", frames, det_fps);



    row("处理", tmp, cv::Scalar(0, 255, 255));

    snprintf(tmp, sizeof(tmp), "real %.1f fps", fps);

    row("实时", tmp, cv::Scalar(0, 255, 255));



    section("DETECT");



    snprintf(tmp, sizeof(tmp), "raw %zu  tracked %zu  persons %d", raw_dets, tracked, persons);



    row("det", tmp);



    section("FENCE");



    snprintf(tmp, sizeof(tmp), "enter %d   exit %d   now %d", zone_in, zone_out, in_now);



    row("zone", tmp, cv::Scalar(0, 255, 0));



    snprintf(tmp, sizeof(tmp), "IoB in %.2f / out %.2f", iob_in, iob_out);



    row("iob", tmp);



    section("FP / MISS");



    double fpr = frames > 0 ? 100.0 * fp_confirmed / frames : 0.0;



    double mr  = frames > 0 ? 100.0 * miss / frames : 0.0;



    snprintf(tmp, sizeof(tmp), "%d (zone %d)   %.2f%%", fp_confirmed, fp_zone, fpr);



    row("false pos", tmp, cv::Scalar(0, 255, 255));



    snprintf(tmp, sizeof(tmp), "%d   %.2f%%", miss, mr);



    row("miss", tmp, cv::Scalar(0, 255, 255));



    section("LOG");



    for (size_t i = 0; i < ev.size() && y < panel_h - 16; i++) {



        std::string ln = ev[i];



        if (ln.size() > 46) ln = ln.substr(0, 46);



        cv::putText(panel, ln, cv::Point(8, y + 12), FONT, FS, cv::Scalar(230, 230, 230), TH, cv::LINE_AA);



        y += 16;



    }



}



static bool drawZoneInteractively(cv::Mat &frame, const std::string &savePath, float scale)



{



    g_draw_zone.clear();



    g_zone_save_path = savePath;



    g_draw_scale = scale;   // mouse coords will be divided by this



    cv::setMouseCallback("Zone Detection", onMouse);



    cv::Mat canvas = frame.clone();



    cv::Mat show_small;



    cv::resize(canvas, show_small, cv::Size((int)(canvas.cols * scale), (int)(canvas.rows * scale)), 0, 0, cv::INTER_LINEAR);



    // Keep the drawing window the same size as the main-loop display window



    // (disp_scale). Drawing at a different size than subsequent imshow leaves



    // the X11 HighGUI window size locked to the drawing size -> window can be



    // enlarged but never shrunk (the old two-window bug resurfaces).



    cv::resizeWindow("Zone Detection", show_small.cols, show_small.rows);



    printf("\n=== Manual Zone Drawing ===\n");



    printf("Click 4+ points to define polygon, then:\n");



    printf("  ENTER = save & continue\n");



    printf("  ESC   = skip (no zone)\n");



    printf("  c     = clear all points\n");



    while (true) {



        cv::Mat show = show_small.clone();



        // Draw existing points and lines (scale them to the small display)



        for (size_t i = 0; i < g_draw_zone.size(); i++) {



            cv::Point p((int)(g_draw_zone[i].x * scale), (int)(g_draw_zone[i].y * scale));



            cv::circle(show, p, 5, cv::Scalar(0, 255, 255), -1);



            if (i > 0) cv::line(show, p, cv::Point((int)(g_draw_zone[i-1].x * scale), (int)(g_draw_zone[i-1].y * scale)), cv::Scalar(0, 255, 0), 2);



            if (i == g_draw_zone.size() - 1 && g_draw_zone.size() >= 3)



                cv::line(show, p, cv::Point((int)(g_draw_zone[0].x * scale), (int)(g_draw_zone[0].y * scale)), cv::Scalar(0, 255, 0), 2);



        }



        cv::putText(show, "Click points | ENTER=save | ESC=skip | c=clear",



                    cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);



        cv::imshow("Zone Detection", show);



        int key = cv::waitKey(50);



        if (key == 13 || key == 10) { // ENTER



            if (g_draw_zone.size() >= 3) {



                // Save to file (points already in frame space)



                std::ofstream zf(g_zone_save_path);



                for (auto &pt : g_draw_zone)



                    zf << pt.x << "," << pt.y << "\n";



                zf.close();



                printf("Zone saved: %zu points -> %s\n", g_draw_zone.size(), g_zone_save_path.c_str());



                return true;



            }



        } else if (key == 27) { // ESC



            printf("Zone drawing skipped\n");



            return false;



        } else if (key == 'c' || key == 'C') {



            g_draw_zone.clear();



            printf("  Cleared\n");



        }



    }



}



// Draw fence zone overlay + tracking boxes on the image. Shared by the



// display window and the --gui-frame export (external GUI video panel).



static void drawOverlay(cv::Mat &img, const std::vector<cv::Point> &zone,



                        const std::vector<STrack> &stracks,



                        const std::map<int, bool> &in_zone)



{



    if (!zone.empty()) {



        static cv::Mat zone_overlay;



        if (zone_overlay.empty() || zone_overlay.size() != img.size()) {



            zone_overlay = cv::Mat::zeros(img.size(), img.type());



            cv::fillPoly(zone_overlay, std::vector<std::vector<cv::Point>>{zone}, cv::Scalar(0, 255, 0));



        }



        cv::addWeighted(zone_overlay, 0.3, img, 0.7, 0, img);



    }



    for (size_t i = 0; i < stracks.size(); i++) {



        vector<float> tlwh = stracks[i].tlwh;



        cv::Rect r((int)tlwh[0], (int)tlwh[1], (int)tlwh[2], (int)tlwh[3]);



        int tid = stracks[i].track_id;



        auto it = in_zone.find(tid);



        bool inside = (it != in_zone.end()) ? it->second : false;



        cv::Scalar color = inside ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);



        cv::rectangle(img, r, color, 1);



        cv::putText(img, std::to_string(tid) + " " + std::to_string((int)(stracks[i].score*100)) + "%",



                    cv::Point((int)tlwh[0], (int)tlwh[1] - 3), cv::FONT_HERSHEY_SIMPLEX, 0.35, color, 1);



    }



}



// Open a video file for decoding. For H.264 containers (.mp4/.mov/.mkv) use a



// GStreamer software-decode pipeline (avdec_h264) first: the board's patched



// OpenCV/ffmpeg routes H.264 through MPP+RGA which fails (RGA_BLIT spam +



// garbage frames). MJPG/other files keep the FFMPEG backend (pure CPU).



static bool openVideoFile(cv::VideoCapture &cap, const std::string &path, bool force_ffmpeg = false)



{



    std::string ext;



    size_t dot = path.find_last_of('.');



    if (dot != std::string::npos) {



        ext = path.substr(dot);



        for (size_t i = 0; i < ext.size(); i++)



            if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] = (char)(ext[i] + 32);



    }



    // H.264 容器: GStreamer 硬解优先(avdec_h264_rkmpp + appsink sync=false)。



    // 注意: 系统 OpenCV 4.2 的 videoio 被厂商 patch 过, FFMPEG 后端读帧



    // 内部走 RGA 转换且持续失败(RGA_BLIT fail 刷屏, 5.8fps); GStreamer



    // 硬解路径虽也有 RGA 报错但不致命(720p 实测稳定 30fps)。



    // FFMPEG 作为最后回退。



    if (ext == ".mp4" || ext == ".mov" || ext == ".mkv") {



        const char *demux = (ext == ".mkv") ? "matroskademux" : "qtdemux";



        // NOTE: do NOT quote the path (OpenCV 4.2 GStreamer backend fails to



        // open a quoted filesrc location); paths with spaces are not supported.



        // appsink sync=false drop=false max-buffers=4: 关闭视频时钟同步,



        // 让流水线全速跑(默认 sync=true 会卡死并行, 4K 只有 15fps)。



        std::string app_sink = "appsink sync=false drop=false max-buffers=4";



        std::string pipeline_hw1 = "filesrc location=" + path + " ! " + demux + " ! h264parse ! avdec_h264 "



                                   "! video/x-raw,format=I420 ! " + app_sink;



        std::string pipeline_hw2 = "filesrc location=" + path + " ! " + demux + " ! h264parse ! mppvideodec "



                                   "! video/x-raw,format=I420 ! " + app_sink;



        const char *hw_pipelines[] = { pipeline_hw1.c_str(), pipeline_hw2.c_str() };



        const char *hw_names[] = { "avdec_h264", "mppvideodec" };



        for (size_t pi = 0; pi < 2; pi++) {



            if (cap.open(hw_pipelines[pi], cv::CAP_GSTREAMER)) {



                printf("Open video via GStreamer HW decode [%s]: %s\n", hw_names[pi], path.c_str());



                return true;



            }



            cap.release();



            printf("GStreamer HW pipeline %zu failed, trying next\n", pi);



        }



        printf("GStreamer MPP H.264 open failed (%s), fallback to FFMPEG\n", path.c_str());



        cap.open(path, cv::CAP_FFMPEG);



        if (cap.isOpened()) {



            printf("Open video via FFMPEG backend (retry): %s\n", path.c_str());



            return true;



        }



        cap.release();



        printf("FFMPEG open failed too: %s\n", path.c_str());



    }



    cap.open(path, cv::CAP_FFMPEG);



    if (cap.isOpened())



        printf("Open video via FFMPEG backend: %s\n", path.c_str());



    return cap.isOpened();



}



double __get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }



const std::vector<std::string> names = {"person", "head_helmet","head","reflective_clothes","smoking","calling","falling","face_mask","car","bicycle","motorcycle",



                    "fumes","fire","head_hat","normal_clothes","face","play_phone","other","knife"};



const std::vector<cv::Scalar> colors = {



        cv::Scalar(255, 0, 0),    // 蓝色



        cv::Scalar(0, 255, 0),    // 绿色



        cv::Scalar(0, 0, 255),    // 红色



        cv::Scalar(255, 255, 0),  // 青色



        cv::Scalar(255, 0, 255),  // 品红色



        cv::Scalar(0, 255, 255),  // 黄色



        cv::Scalar(192, 192, 192), // 浅灰色



        cv::Scalar(128, 0, 0),    // 深红色



        cv::Scalar(128, 128, 0),  // 橄榄色



        cv::Scalar(0, 128, 0),    // 深绿色



        cv::Scalar(128, 0, 128),  // 紫色



        cv::Scalar(0, 128, 128),  // 深青色



        cv::Scalar(0, 0, 128),    // 深蓝色



        cv::Scalar(255, 128, 0),  // 橙色



        cv::Scalar(255, 0, 128),  // 红紫色



        cv::Scalar(128, 255, 0),  // 黄绿色



        cv::Scalar(0, 255, 128),  // 青绿色



        cv::Scalar(128, 0, 255),  // 蓝紫色



        cv::Scalar(255, 255, 255) // 白色



    };



const float person_thres = 0.1;      //0



const float head_helmet_thres = 0.15;        //1



const float head_thres = 0.15;                //2



const float reflective_clothes_thres = 0.5;   //3



const float smoking_thres = 0.15;    //4



const float calling_thres = 0.15;     //5



const float falling_thres = 0.5;       //6



const float face_mask_thres = 0.5;     //7



const float car_thres = 0.5;           //8



const float bicycle_thres = 0.5;        //9



const float motorcycle_thres = 0.5;     //10



const float fumes_thres = 0.5;          //11



const float fire_thres = 0.5;          //12



const float head_hat_thres = 0.15;      //13



const float normal_clothes_thres = 0.5; //14



const float face_thres = 0.5;           //15



const float play_phone_thres = 0.15;    //16



const float other_thres = 0.5;         //17



const float knife_thres = 0.15;         //18



/*-------------------------------------------



                  Main Functions



-------------------------------------------*/



int main(int argc, char** argv)



{



    // Line-buffer stdout: when output goes to a pipe or file (GUI / detect.log)



    // the default is full buffering (~4KB chunks), which makes the console look



    // frozen for hundreds of frames. Line buffering flushes every line.



    setvbuf(stdout, NULL, _IOLBF, 0);



    printf("start test... \n");



    // Parse args: --file video.mp4 --model personDetect2_fp16.rknn (default: GigE camera)



    std::string video_file;



    std::string model_name = "yolov26s_fp16_bare.rknn";



    int fp_frames = 5;  // 误检确认阈值:目标持续被跟踪 >= N 次检测才算一次误报



    int miss_frames = 2;  // 漏检确认阈值:目标消失 >= N 次检测才算一次漏检



    // 注意: fp/miss/zone_confirm 阈值都以"检测次数"计,不是视频帧数。



    // 抽帧检测(--detect-interval 5)下每次检测覆盖 5 帧视频,所以同一实际



    // 时间窗对应的阈值更小: miss_frames=2 (~10帧视频 ≈ 0.33s) 与逐帧时代的



    // 10 帧保持相同灵敏度; fp_frames=5 (~25帧 ≈ 0.83s) 比逐帧时代更严。



    bool no_fp = false;    // --no-fp:  禁用误检统计(真人视频测漏检时用)



    bool fp_only = false;  // --fp-only:禁用漏检统计(机械臂视频测误检时用)



    float iob_in = 0.5f;   // 进围栏阈值:IoB >= 此值



    float iob_out = 0.4f;  // 出围栏阈值:IoB < 此值(低于 iob_in,形成迟滞带)



    float disp_scale = 0.5f;  // 显示缩放(相对检测帧):0.5 => 640x360 显示,降 X11 带宽;可调 0.25~1.0



    bool no_display = false;  // --no-display: 不创建窗口/不显示,供外部GUI无头运行



    std::string gui_frame_path;  // --gui-frame <path>: 每2帧存一帧JPEG,供外部GUI显示画面



    std::string fp_save_dir;  // --fp-save <dir>: 误检确认时保存当前帧到该目录(fp_001.jpg...),不覆盖



    bool draw_zone_only = false;  // --draw-zone-only: 只画围栏并保存zone.txt后退出(供GUI首次启动)



    float iob_lower = 0.5f;  // --iob-lower: 只对框的下半部分算IoB(默认0.5),上半身探出围栏不判出



    int zone_mode = 2;  // --zone-mode hybrid|foot|area: 0=area面积IoB, 1=foot脚点, 2=hybrid混合(默认,脚点或面积任一在内即算在内)



    float zone_hyst = 6.0f;  // --zone-hyst: 脚点判定的迟滞(像素,在减半帧坐标系)

    int zone_debounce = 3;   // --zone-debounce N: 进出状态连续 N 次检测一致才翻转计数(防框抖动反复进出;抽帧下3次≈0.5s视频)



    float conf_person = 0.65f;  // --conf: 人形检测最终置信度阈值(默认0.65),调高可减少机械臂等误检



    int detect_interval = 5;  // --detect-interval N: 每 N 帧视频帧检测 1 次(抽帧检测,默认5)



    bool force_ffmpeg = false;  // --ffmpeg: 强制用 FFMPEG 后端打开视频(性能对比调试用)



    bool allow_born_inside = false;  // --allow-born-inside: 允许"首次出现即在围栏内"计为进入(默认否,机械臂在围栏内场景不计)



    int zone_confirm_frames = 2;  // --zone-confirm N: 轨迹存活>=N次检测才参与围栏进出判定/计数(过滤瞬时误检;抽帧下2次≈0.33s视频)



    for (int i = 1; i < argc; i++) {



        if (std::strcmp(argv[i], "--file") == 0 && i + 1 < argc) {



            video_file = argv[++i];



        } else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) {



            model_name = argv[++i];



        } else if (std::strcmp(argv[i], "--fp-frames") == 0 && i + 1 < argc) {



            fp_frames = std::atoi(argv[++i]);



            if (fp_frames < 1) fp_frames = 1;



        } else if (std::strcmp(argv[i], "--miss-frames") == 0 && i + 1 < argc) {



            miss_frames = std::atoi(argv[++i]);



            if (miss_frames < 1) miss_frames = 1;



        } else if (std::strcmp(argv[i], "--no-fp") == 0) {



            no_fp = true;



        } else if (std::strcmp(argv[i], "--fp-only") == 0) {



            fp_only = true;



        } else if (std::strcmp(argv[i], "--iob-in") == 0 && i + 1 < argc) {



            iob_in = (float)std::atof(argv[++i]);



            if (iob_in < 0.0f) iob_in = 0.0f;



            if (iob_in > 1.0f) iob_in = 1.0f;



        } else if (std::strcmp(argv[i], "--iob-out") == 0 && i + 1 < argc) {



            iob_out = (float)std::atof(argv[++i]);



            if (iob_out < 0.0f) iob_out = 0.0f;



            if (iob_out > 1.0f) iob_out = 1.0f;



        } else if (std::strcmp(argv[i], "--disp-scale") == 0 && i + 1 < argc) {



            disp_scale = (float)std::atof(argv[++i]);



            if (disp_scale < 0.25f) disp_scale = 0.25f;



            if (disp_scale > 1.0f) disp_scale = 1.0f;



        } else if (std::strcmp(argv[i], "--no-display") == 0) {



            no_display = true;



        } else if (std::strcmp(argv[i], "--gui-frame") == 0 && i + 1 < argc) {



            gui_frame_path = argv[++i];



        } else if (std::strcmp(argv[i], "--fp-save") == 0 && i + 1 < argc) {



            fp_save_dir = argv[++i];



        } else if (std::strcmp(argv[i], "--draw-zone-only") == 0) {



            draw_zone_only = true;



        } else if (std::strcmp(argv[i], "--iob-lower") == 0 && i + 1 < argc) {



            iob_lower = (float)std::atof(argv[++i]);



            if (iob_lower < 0.1f) iob_lower = 0.1f;



            if (iob_lower > 1.0f) iob_lower = 1.0f;



        } else if (std::strcmp(argv[i], "--zone-mode") == 0 && i + 1 < argc) {



            if (std::strcmp(argv[i + 1], "area") == 0) zone_mode = 0;



            else if (std::strcmp(argv[i + 1], "foot") == 0) zone_mode = 1;



            else zone_mode = 2;   // hybrid (default)



            i++;



        } else if (std::strcmp(argv[i], "--zone-hyst") == 0 && i + 1 < argc) {



            zone_hyst = (float)std::atof(argv[++i]);



            if (zone_hyst < 0.0f) zone_hyst = 0.0f;



            if (zone_hyst > 50.0f) zone_hyst = 50.0f;



        } else if (std::strcmp(argv[i], "--zone-debounce") == 0 && i + 1 < argc) {



            zone_debounce = std::atoi(argv[++i]);



            if (zone_debounce < 1) zone_debounce = 1;



            if (zone_debounce > 20) zone_debounce = 20;



        } else if (std::strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {



            conf_person = (float)std::atof(argv[++i]);



            if (conf_person < 0.05f) conf_person = 0.05f;



            if (conf_person > 0.99f) conf_person = 0.99f;



            allow_born_inside = true;



        } else if (std::strcmp(argv[i], "--detect-interval") == 0 && i + 1 < argc) {



            detect_interval = std::atoi(argv[++i]);



            if (detect_interval < 1) detect_interval = 1;



        } else if (std::strcmp(argv[i], "--ffmpeg") == 0) {



            force_ffmpeg = true;



        } else if (std::strcmp(argv[i], "--zone-confirm") == 0 && i + 1 < argc) {



            zone_confirm_frames = std::atoi(argv[++i]);



            if (zone_confirm_frames < 1) zone_confirm_frames = 1;



        }



    }



    if (!video_file.empty()) {



        printf("Video file mode: %s\n", video_file.c_str());



    }



    printf("Model: %s\n", model_name.c_str());



    printf("False-positive confirm threshold: %d detections (~%d video frames)\n", fp_frames, fp_frames * detect_interval);



    printf("Miss (lost) confirm threshold: %d detections (~%d video frames)\n", miss_frames, miss_frames * detect_interval);



    printf("Detect interval: every %d video frames (effective detection rate ~%dfps for 30fps input)\n",



           detect_interval, detect_interval > 0 ? 30 / detect_interval : 30);



    printf("Stats mode: fp=%s miss=%s\n", no_fp ? "OFF" : "ON", fp_only ? "OFF" : "ON");



    printf("Zone judge=%s (hyst=%.1f) confirm=%d debounce=%d | IoB in=%.2f out=%.2f lower=%.2f\n",



           zone_mode == 0 ? "area" : (zone_mode == 1 ? "foot" : "hybrid"),



           zone_hyst, zone_confirm_frames, zone_debounce, iob_in, iob_out, iob_lower);



    printf("Display scale: %.2f (display ~%dx%d for a 2560x1440 source)\n",



           disp_scale, (int)(1280*disp_scale), (int)(720*disp_scale));



    printf("Person conf threshold: %.2f\n", conf_person);



    // --- Draw-zone-only mode (used by the external GUI on first run): show the



    // first frame of the video, let the user click the fence polygon, save



    // ../models/zone.txt, then exit. No model/NPU needed, opens fast. ---



    if (draw_zone_only) {



        if (video_file.empty()) {



            printf("--draw-zone-only requires --file <video>\n");



            return -1;



        }



        cv::VideoCapture zcap;



        if (!openVideoFile(zcap, video_file, force_ffmpeg)) {



            printf("Cannot open video %s for zone drawing\n", video_file.c_str());



            return -1;



        }



        cv::Mat zraw;



        if (!zcap.read(zraw) || zraw.empty()) {



            printf("Cannot read first frame of %s\n", video_file.c_str());



            return -1;



        }



        // 统一到 1280x720 处理空间画围栏(与主循环检测/叠加坐标一致):



        // GStreamer 已输出 720, FFMPEG 4K 需要缩半。



        cv::Mat zframe;



        if (zraw.type() != CV_8UC3) {



            // I420 管线输出(软解): 先转 BGR 再缩放(直接 resize 堆叠布局会显示灰图)



            cv::cvtColor(zraw, zframe, cv::COLOR_YUV2BGR_I420);



            if (zframe.cols > 1280 || zframe.rows > 720)



                cv::resize(zframe, zframe, cv::Size(zframe.cols / 2, zframe.rows / 2), 0, 0, cv::INTER_NEAREST);



        } else {



            if (zraw.cols > 1280 || zraw.rows > 720)



                cv::resize(zraw, zframe, cv::Size(zraw.cols / 2, zraw.rows / 2), 0, 0, cv::INTER_NEAREST);



            else



                zframe = zraw;



        }



        std::string zpath = std::string("../models") + "/zone.txt";



        cv::namedWindow("Zone Detection", cv::WINDOW_NORMAL);



        printf("=== Zone drawing mode (frame %dx%d) ===\n", zframe.cols, zframe.rows);



        printf("Click 4+ points for the fence polygon, then ENTER to save / ESC to skip.\n");



        if (drawZoneInteractively(zframe, zpath, disp_scale)) {



            printf("Zone saved to %s\n", zpath.c_str());



        } else {



            printf("Zone drawing skipped, %s unchanged\n", zpath.c_str());



        }



        return 0;



    }



    // 误检帧保存目录: 存在则自动创建(忽略已存在)



    if (!fp_save_dir.empty()) {



        mkdir(fp_save_dir.c_str(), 0755);



    }



    int ret = 0;



    Detect MultiDetecter;



    // 单类检测阈值 (personDetect2 FP16: scores ~0.75-0.85; INT8 slightly lower; YOLOv26s 裸输出: ~0.86)



    // 通过 --conf 调整: 调高可减少机械臂/遮挡导致的低置信度误检



    std::vector<float> conf_thresholds(1, conf_person);



    const char* models_path = "../models"; //所有模型所在的目录



    std::string Multidetecter_model_path = models_path + std::string("/") + model_name;



    ret = MultiDetecter.init_model(Multidetecter_model_path.c_str(), NMS_THRESHOLD, CONF_THRESHOLD, 3, conf_thresholds);



    if (ret != 0) {



        printf("Model init failed, exiting\n");



        return -1;



    }



    int video_width, video_height, video_fps;



    ArvCamera* camera = NULL;



    ArvStream* stream = NULL;



    GError* error = NULL;



    cv::VideoCapture video_cap;



    if (video_file.empty()) {



        // --- GigE Vision camera via Aravis ---



        arv_update_device_list();



        int n_devices = arv_get_n_devices();



        if (n_devices == 0) {



            printf("No GigE Vision camera found!\n");



            return -1;



        }



        printf("Found %d GigE camera(s)\n", n_devices);



        const char* camera_id = arv_get_device_id(0);



        printf("Using camera: %s\n", camera_id);



        camera = arv_camera_new(camera_id, &error);



        if (!camera) {



            printf("Failed to open camera: %s\n", error ? error->message : "unknown");



            return -1;



        }



        arv_camera_set_acquisition_mode(camera, ARV_ACQUISITION_MODE_CONTINUOUS, &error);



        // Auto exposure + auto gain



        arv_camera_set_exposure_time_auto(camera, ARV_AUTO_CONTINUOUS, &error);



        g_clear_error(&error);



        arv_camera_set_gain_auto(camera, ARV_AUTO_CONTINUOUS, &error);



        g_clear_error(&error);



        int cam_width, cam_height;



        arv_camera_get_region(camera, NULL, NULL, &cam_width, &cam_height, &error);



        printf("Camera resolution: %dx%d\n", cam_width, cam_height);



        // Hardware 2x2 binning: camera outputs half resolution directly



        arv_camera_set_binning(camera, 2, 2, &error);



        if (error) {



            printf("Binning not supported: %s, keeping full resolution\n", error->message);



            g_clear_error(&error);



        } else {



            printf("Binning 2x2 enabled, effective output: %dx%d\n", cam_width/2, cam_height/2);



        }



        arv_camera_set_pixel_format_from_string(camera, "BayerRG8", &error);



        if (error) {



            g_clear_error(&error);



            printf("BayerRG8 not supported, keeping default format\n");



        }



        printf("Pixel format: %s\n", arv_camera_get_pixel_format_as_string(camera, &error));



        stream = arv_camera_create_stream(camera, NULL, NULL, &error);



        if (!stream) {



            printf("Failed to create stream: %s\n", error ? error->message : "unknown");



            return -1;



        }



        size_t payload = arv_camera_get_payload(camera, &error);



        for (int i = 0; i < 10; i++) {



            arv_stream_push_buffer(stream, arv_buffer_new(payload, NULL));



        }



        arv_camera_start_acquisition(camera, &error);



        printf("Camera acquisition started\n");



        video_width = cam_width / 2;



        video_height = cam_height / 2;



        video_fps = 30;



    } else {



        // --- Video file via OpenCV ---



        // .mp4/.mkv/.mov (H.264) use a GStreamer software pipeline (avdec_h264)



        // because the board's MPP/RGA path is broken for H.264 (RGA_BLIT fail



        // spam + garbage frames). MJPG/AVI keep the FFMPEG backend (pure CPU).



        openVideoFile(video_cap, video_file, force_ffmpeg);



        if (!video_cap.isOpened()) {



            printf("Failed to open video file: %s\n", video_file.c_str());



            return -1;



        }



        // FFMPEG 后端输出原始分辨率(4K), GStreamer 硬解已输出 1280x720。



        // video_width/height 统一反映处理尺寸(围栏坐标空间): 超过 720p



        // 才减半, 否则直接用。



        int src_w = (int)video_cap.get(cv::CAP_PROP_FRAME_WIDTH);



        int src_h = (int)video_cap.get(cv::CAP_PROP_FRAME_HEIGHT);



        if (src_w > 1280 || src_h > 720) {



            video_width  = src_w / 2;



            video_height = src_h / 2;



        } else {



            video_width  = src_w;



            video_height = src_h;



        }



        video_fps    = (int)video_cap.get(cv::CAP_PROP_FPS);



        if (video_fps <= 0) video_fps = 30;



        printf("Video: %dx%d @ %d fps (process %dx%d)\n",



               src_w, src_h, video_fps, video_width, video_height);



    }



    printf("video info: width=%d, height=%d, fps=%d\n", video_width, video_height, video_fps);



    // --- Load electronic fence zone ---



    std::vector<cv::Point> zone_polygon;



    std::string zone_path = models_path + std::string("/zone.txt");



    std::ifstream zone_file(zone_path);



    if (zone_file.is_open()) {



        std::string line;



        while (std::getline(zone_file, line)) {



            if (line.empty() || line[0] == '#') continue;



            int x, y;



            if (sscanf(line.c_str(), "%d,%d", &x, &y) == 2) {



                zone_polygon.push_back(cv::Point(x, y));



            }



        }



        zone_file.close();



        printf("Loaded zone with %zu vertices from %s\n", zone_polygon.size(), zone_path.c_str());



        // Scale zone coords: camera has hardware binning (/2), video file does not



        if (video_file.empty()) {



            for (auto& pt : zone_polygon) { pt.x /= 2; pt.y /= 2; }



        }



    } else {



        printf("No zone file at %s\n", zone_path.c_str());



    }



    int zone_enter_count = 0, zone_exit_count = 0;



    std::map<int, bool> person_in_zone;  // track_id -> currently inside?



    std::set<int> track_ever_outside;    // 曾观察到在围栏外的 track_id



    std::set<int> zone_counted_ids;      // 已计数"进入"的 track_id(只有它才能计"出")

    std::map<int, int> zone_side_streak; // track_id -> 当前"疑似翻转侧"连续一致次数(防抖, 累计到 zone_debounce 才翻转)



    // --- False-positive statistics (no real persons in these test videos) ---



    // A false positive is counted once per track_id: a confirmed (tracklet_len



    // >= fp_frames) fake target. track_id is a global static counter in



    // STrack::next_id() and is never reused, so set dedup is safe.



    std::set<int> false_pos_counted;   // track_ids already counted as false positives



    std::set<int> fp_zone_entered;     // of those, which triggered a zone ENTERED



    int confirmed_false_pos = 0;



    int zone_false_pos = 0;



    int total_frames = 0;  // 已处理帧数(主循环外声明,供结束汇总使用)



    std::map<int,int> miss_absent;   // track_id -> 连续消失(不在 output)的帧数



    std::set<int> miss_counted;      // 已计漏检的 track_id



    int miss_count = 0;



    std::set<int> prev_tids;         // track_ids present in the previous frame



    // Create the main window BEFORE zone drawing: draw mode reuses this



    // window (single-window design), so the X11 HighGUI bug where a destroyed



    // drawing window leaves the main window unable to shrink never triggers.



    if (!no_display) {



        cv::namedWindow("Zone Detection", cv::WINDOW_NORMAL);



        cv::resizeWindow("Zone Detection", 1280, 720);



    }



    // ByteTrack 帧率参数按"有效检测率"传:抽帧检测时 tracker 只被调用



    // detect_interval 分之一的频率,若不折算,轨迹丢失容忍时间(max_time_lost)



    // 会变成实际视频的 detect_interval 倍,人走开很久轨迹还"活着",污染



    // 误检/漏检统计。30fps / 5 => 6 次检测/s, max_time_lost ≈ 30s 视频时间。



    int eff_fps = video_fps / detect_interval;



    if (eff_fps < 1) eff_fps = 1;



    BYTETracker tracker(eff_fps, 50);



    // --- Pipeline: preprocess on main thread, NPU infer on worker thread ---



    // The worker consumes preprocessed frames and publishes results; the main



    // loop overlaps frame N+1 preprocessing with frame N NPU inference.



    std::queue<cv::Mat> frame_queue;        // preprocessed (orig) frames



    std::queue<std::vector<int>> q_ids;



    std::queue<std::vector<float>> q_conf;



    std::queue<std::vector<cv::Rect>> q_boxes;



    std::mutex q_mtx;



    std::condition_variable q_cv_frame, q_cv_res;



    bool infer_stop = false;



    bool infer_done = false;



    // --- 视频解码线程(仅视频文件模式): 持续 cap.read() 拉帧, 主循环



    // 从 dec_queue 无阻塞取最新帧。OpenCV 4.2 的 GStreamer appsink 是



    // 同步拉帧模型, read() 阻塞时把解码/检测串行化, 4K 源被卡到 15fps;



    // 独立线程拉帧 + 队列解耦后, 主循环帧率 = min(拉帧能力, 检测能力),



    // 720p 源可达 30fps。队列满丢旧帧(只保留最新, 实时性优先)。



    std::queue<cv::Mat> dec_queue;      // 解码线程 -> 主循环



    std::mutex dec_mtx;



    std::condition_variable dec_cv;



    bool dec_stop = false;



    bool dec_done = false;



    // --- GUI 帧导出后台线程(主循环 -> worker): 编码+写盘挪出主循环 ---
    // 误检帧多时, GUI 帧每帧都在变(画框/文字), 主循环每帧 imwrite 会被
    // eMMC/编码拖慢, 偶发超时 -> 解码线程堆积 -> 节流时间线重对齐 -> 画面快进。
    // 这里只在入队端做 100ms 节流, worker 只保留最新任务(丢弃旧), 主循环永不阻塞。
    struct GuiFrameTask {
        cv::Mat frame;               // 已克隆的帧(worker 只读)
        std::vector<cv::Point> zone;
        std::vector<STrack> stracks;
        std::map<int, bool> in_zone;
        std::string path;            // 最终文件路径
        int type = 0;                // 0=GUI显示帧, 1=误检帧
    };
    std::queue<GuiFrameTask> gui_queue;    // 唯一写盘队列(GUI帧 + 误检帧)
    std::mutex gui_mtx;
    std::condition_variable gui_cv;
    bool gui_stop = false;
    std::thread gui_worker([&]() {
        while (true) {
            GuiFrameTask task;
            {
                std::unique_lock<std::mutex> lk(gui_mtx);
                gui_cv.wait(lk, [&]() { return gui_stop || !gui_queue.empty(); });
                if (gui_queue.empty()) { if (gui_stop) break; else continue; }
                // 先进先出, 不丢任何任务: 帧时间线单调, 杜绝内容跳变
                // (历史坑: "只取最新丢旧"在 CPU 竞争时丢帧 -> 画面加速)
                task = std::move(gui_queue.front());
                gui_queue.pop();
            }
            // 后台画图+编码+原子写盘(全部不占主循环)
            drawOverlay(task.frame, task.zone, task.stracks, task.in_zone);
            std::string tmp = task.path + ".tmp.jpg";
            if (task.type == 0) {
                // GUI 显示帧: 720p(用户要求清晰度, 360p 观感差)
                cv::Mat full;
                cv::resize(task.frame, full, cv::Size(1280, 720), 0, 0, cv::INTER_NEAREST);
                if (cv::imwrite(tmp, full)) {
                    rename(tmp.c_str(), task.path.c_str());
                }
            } else {
                // 误检帧: 原尺寸写盘
                if (cv::imwrite(tmp, task.frame)) {
                    rename(tmp.c_str(), task.path.c_str());
                }
            }
        }
    });

    std::thread dec_thread([&]() {

        if (video_file.empty()) { dec_done = true; return; }   // 相机模式不走



        // 解码线程节流(关键!): 控制"视频时间轴推进速度"为 1x。



        // 若解码全速拉帧(720p 硬解 100+fps), 视频时间轴 3x+ 推进,



        // 主循环虽然按 30fps 消费, 但取到的是时间轴上"跑快"的帧,



        // 画面内容加速播放。解码线程按视频帧率节流产出后, 视频时间轴



        // 严格 1x; 主循环再按同节奏消费, 播放 1x 无加速。



        // (720p 解码能力远高于 30fps, 节流不会造成饥饿)



        double frame_us = 1000000.0 / video_fps;



        long dec_cnt = 0;



        auto t_diag = std::chrono::steady_clock::now();



        while (true) {



            cv::Mat f;



            if (!video_cap.read(f) || f.empty()) {



                // 视频结束: 通知主循环退出



                std::lock_guard<std::mutex> lk(dec_mtx);



                dec_stop = true;



                dec_cv.notify_all();



                break;



            }



            {



                // 背压: 队列满(>=3)时等待主循环消费, 不丢旧帧。
                // 主循环偶发卡顿 -> 解码暂停 -> 恢复后从停下的帧继续播
                // (宁可短暂停顿, 不跳帧——丢帧会导致内容前跳/"回溯"错觉)。

                std::unique_lock<std::mutex> lk(dec_mtx);

                dec_cv.wait(lk, [&]() { return dec_stop || dec_queue.size() < 3; });

                if (dec_stop) break;



                                // 丢弃 GStreamer 软解启动期灰帧(warm-up 未就绪输出): 前 ~8 帧全灰



                // (mean~128 std~0)或半灰, 显示为花屏。std<12 且中灰均值视为无效帧,



                // 直接丢弃(内容帧 std 通常 >20)。



                bool gray_frame = false;



                if (!f.empty()) {



                    cv::Mat gray;



                    if (f.channels() == 3) cv::cvtColor(f, gray, cv::COLOR_BGR2GRAY);



                    else gray = f;



                    cv::Scalar m, s;



                    cv::meanStdDev(gray, m, s);



                    if (s[0] < 12.0 && m[0] > 90.0 && m[0] < 165.0) gray_frame = true;



                }



                if (!gray_frame) {



                dec_queue.push(f);



                }



            }



            dec_cv.notify_all();



            dec_cnt++;



            if (dec_cnt % 300 == 0) {



                double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_diag).count();



                double expect = 300.0 * frame_us / 1e6;   // 期望耗时



                printf("[DEC] %ld frames wall=%.1fs expect=%.1fs ratio=%.2f (1.0=正常, >1=慢放, <1=加速)\n",



                       dec_cnt, dt, expect, dt / expect);



                t_diag = std::chrono::steady_clock::now();



                dec_cnt = 0;



            }



            if (dec_stop) break;



            // 节流(绝对时间线): 期望时间点 = 上一期望 + frame_us, 不随



            // read/sleep 实际耗时漂移。之前用"实际结束时刻"节流, sleep 只



            // 补 read 的耗时(0.7ms)而非整帧间隔 → 产出 ~50fps 加速播放。



            // 绝对时间线: sleep = 期望时间 - now, 严格 1/fps 一帧。



                        // adaptive throttle: 解码能力不足(实际 <30fps)时, 固定 33.3ms 时间线会被



            // read 拖累后持续"追赶-重置", 时间线失效变成全速读帧(内容加速)。



            // 用解码耗时滑动平均作为每帧最小时长: 每帧耗 = max(1/fps, 解码平均耗时)



            // -> 解码慢时按解码速度走(内容 1x, 播放慢一点), 解码快时依旧严格 1/fps。



            static auto t_exp = std::chrono::steady_clock::now();



            static auto t_dec_avg = std::chrono::duration<double>(frame_us / 1e6);



            static bool t_dec_init = false;



            static std::chrono::steady_clock::time_point t_prev;



            if (t_dec_init) {



                auto read_dt = std::chrono::steady_clock::now() - t_prev;



                t_dec_avg = t_dec_avg * 0.8 + read_dt * 0.2;   // 滑动平均



            }



            t_dec_init = true;



            t_prev = std::chrono::steady_clock::now();



            double eff_us = frame_us;



            if (t_dec_avg.count() * 1e6 > eff_us) eff_us = t_dec_avg.count() * 1e6;



            t_exp += std::chrono::microseconds((long)eff_us);



            auto now = std::chrono::steady_clock::now();



            if (now < t_exp) {



                std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::microseconds>(t_exp - now));



            } else if (now - t_exp > std::chrono::microseconds((long)frame_us * 30)) {



                // 落后太多(卡顿后追赶): 重新对齐, 避免长时间补帧。
                // 阈值从 2 帧提到 30 帧(1s@30fps): 偶发 CPU 竞争(误检保存/GUI 帧
                // 编码写盘)不再触发时间线跳变 -> 偶发卡顿表现为短暂慢放而非突然加速。



                t_exp = now;



            }



        }



        dec_done = true;



    });



    std::thread infer_thread([&]() {



        while (true) {



            cv::Mat frame;



            {



                std::unique_lock<std::mutex> lk(q_mtx);



                q_cv_frame.wait(lk, [&]() { return infer_stop || !frame_queue.empty(); });



                if (infer_stop && frame_queue.empty()) break;



                frame = frame_queue.front();



                frame_queue.pop();



            }



            std::vector<int> ids;



            std::vector<float> conf;



            std::vector<cv::Rect> boxes;



            MultiDetecter.infer(&ids, &conf, &boxes);



            {



                std::lock_guard<std::mutex> lk(q_mtx);



                q_ids.push(std::move(ids));



                q_conf.push(std::move(conf));



                q_boxes.push(std::move(boxes));



            }



            q_cv_res.notify_all();



        }



        infer_done = true;



    });



    // --- Interactive zone drawing for video mode ---



    if (!no_display && zone_polygon.empty() && !video_file.empty()) {



        printf("No zone loaded. Entering manual draw mode on first frame...\n");



        cv::Mat raw_frame;



        if (video_cap.read(raw_frame) && !raw_frame.empty()) {



            // 统一到 1280x720 处理空间画围栏(与主循环检测/叠加同坐标)



            cv::Mat view_frame;



            if (raw_frame.cols > 1280 || raw_frame.rows > 720)



                cv::resize(raw_frame, view_frame, cv::Size(raw_frame.cols / 2, raw_frame.rows / 2), 0, 0, cv::INTER_NEAREST);



            else



                view_frame = raw_frame;



            // draw at the same display scale as the main loop (disp_scale) so



            // the window size stays consistent and stays shrinkable



            if (drawZoneInteractively(view_frame, zone_path, disp_scale)) {



                std::ifstream zf(zone_path);



                std::string line;



                while (std::getline(zf, line)) {



                    if (line.empty() || line[0] == '#') continue;



                    int x, y;



                    if (sscanf(line.c_str(), "%d,%d", &x, &y) == 2)



                        zone_polygon.push_back(cv::Point(x, y));



                }



                printf("Reloaded zone with %zu vertices\n", zone_polygon.size());



            }



        }



        printf("Starting detection...\n");



    }



    // Result buffers, kept across loop iterations: the latest completed



    // inference is consumed at the top of each iteration (may lag one frame).



    std::vector<int> ids;



    std::vector<float> conf;



    std::vector<cv::Rect> boxes;

    // 轨迹结果跨循环保留: 检测帧由 tracker.update() 刷新, 非检测帧沿用

    // 上一次检测结果(框持续显示, 不再闪烁)。视频播放/解码/抽帧调度逻辑不受影响。

    std::vector<STrack> output_stracks;



    while (true)



    {



        cv::Mat orig_img;



        // process_time 循环级计时(检测帧才有 tik/tok)。



        TIMER process_time;



        // --- Grab frame FIRST: this overlaps with the previous frame's NPU



        // inference (worker thread). Video read + resize can take ~40ms; doing



        // it before the wait below hides it under the inference.



        if (video_file.empty()) {



            // --- Grab frame from GigE camera ---



            ArvBuffer* buffer = arv_stream_timeout_pop_buffer(stream, 500000);



            if (!buffer || arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS) {



                if (buffer) arv_stream_push_buffer(stream, buffer);



                continue;



            }



            gint w, h;



            arv_buffer_get_image_region(buffer, NULL, NULL, &w, &h);



            size_t data_size;



            const void* data = arv_buffer_get_data(buffer, &data_size);



            guint32 pf = arv_buffer_get_image_pixel_format(buffer);



            if (pf == 0x01080001) {       // Mono8



                cv::Mat raw(h, w, CV_8UC1, const_cast<void*>(data));



                cv::cvtColor(raw, orig_img, cv::COLOR_GRAY2BGR);



            } else if (pf == 0x01080009) { // BayerRG8



                cv::Mat raw(h, w, CV_8UC1, const_cast<void*>(data));



                cv::cvtColor(raw, orig_img, cv::COLOR_BayerRG2BGR);



            } else if (pf == 0x02180014) { // RGB8



                cv::Mat raw(h, w, CV_8UC3, const_cast<void*>(data));



                cv::cvtColor(raw, orig_img, cv::COLOR_RGB2BGR);



            } else if (pf == 0x02180015) { // BGR8



                cv::Mat raw(h, w, CV_8UC3, const_cast<void*>(data));



                orig_img = raw.clone();



            } else {



                cv::Mat raw(h, w, CV_8UC1, const_cast<void*>(data));



                cv::cvtColor(raw, orig_img, cv::COLOR_GRAY2BGR);



            }



            arv_stream_push_buffer(stream, buffer);



        } else {



            // Video mode: 从解码线程的 dec_queue 取最新帧(无阻塞)。



            // 解码线程持续 cap.read(), 主循环不被 read() 阻塞——帧率由



            // min(拉帧能力, 检测能力) 决定, 720p 源可达 30fps。



            // 队列空(解码慢于消费): 说明视频还没读出下一帧, 稍等。



            cv::Mat raw_frame;



            {



                // 消费跟随解码产出: 解码线程已按视频帧率(30fps)节流,



                // 主循环只需"等下一帧产出"——队列空就等(最多 50ms),



                // 不自己跑时间线, 杜绝加速。解码线程队列满时背压等待, 不丢帧。



                std::unique_lock<std::mutex> lk(dec_mtx);



                dec_cv.wait_for(lk, std::chrono::milliseconds(50),



                                [&]() { return dec_stop || !dec_queue.empty(); });



                if (!dec_queue.empty()) {



                    raw_frame = dec_queue.front();



                    dec_queue.pop();



                    dec_cv.notify_all();   // 唤醒可能被背压阻塞的解码线程



                } else if (dec_stop) {



                    printf("Video ended or read error\n");



                    orig_img.release();



                } else {



                    // 解码线程仍在拉帧(首帧/慢速), 队列暂时空: 跳过本轮,



                    // 下一轮再取。绝不能在此退出——否则首次迭代竞态误判结束。



                    continue;



                }



            }



            if (!raw_frame.empty()) {



                // 统一处理分辨率: NV12(硬解) 先转 BGR 再缩半(先 resize 会搅乱



                // NV12 堆叠布局导致花屏); FFMPEG 后端已 BGR 直接缩半。



                if (raw_frame.type() == CV_8UC3) {



                    if (raw_frame.cols > 1280 || raw_frame.rows > 720) {



                        cv::resize(raw_frame, orig_img,



                                   cv::Size(raw_frame.cols / 2, raw_frame.rows / 2),



                                   0, 0, cv::INTER_NEAREST);



                    } else {



                        orig_img = raw_frame;



                    }



                } else {



                    cv::Mat bgr;



                    cv::cvtColor(raw_frame, bgr, cv::COLOR_YUV2BGR_I420);



                    if (bgr.cols > 1280 || bgr.rows > 720) {



                        cv::resize(bgr, orig_img,



                                   cv::Size(bgr.cols / 2, bgr.rows / 2),



                                   0, 0, cv::INTER_NEAREST);



                    } else {



                        orig_img = bgr;



                    }



                }



            }



        }



        if (orig_img.empty()) {



            // Video ended or read error: stop the worker and exit the loop.



            {



                std::lock_guard<std::mutex> lk(q_mtx);



                infer_stop = true;



            }



            q_cv_frame.notify_all();



            q_cv_res.notify_all();



            {



                std::lock_guard<std::mutex> lk(dec_mtx);



                dec_stop = true;



            }



            dec_cv.notify_all();



            break;



        }



        // --- 抽帧检测(interval>1): 主循环从不阻塞等 NPU 结果。



        // 检测帧: 消费最新推理结果(队列空则沿用上一帧), 提交新帧推理;



        // 非检测帧: 直接用上一帧结果。NPU 推理(~45ms)远快于检测间隔



        // (5帧=~165ms), 结果在下一检测帧前必然就绪; 检测帧消费的是



        // 上一轮的结果, 滞后 interval 帧属预期。



        static int frame_since_detect = detect_interval;  // 下一帧即检测帧



        bool is_detect_frame = (++frame_since_detect >= detect_interval);



        if (is_detect_frame) {



            frame_since_detect = 0;



            {



                std::lock_guard<std::mutex> lk(q_mtx);



                while (!q_ids.empty()) {



                    ids = std::move(q_ids.front()); q_ids.pop();



                    conf = std::move(q_conf.front()); q_conf.pop();



                    boxes = std::move(q_boxes.front()); q_boxes.pop();



                }



            }



            // Pipeline: preprocess on main thread, infer on worker thread.



            // Push frame N; results are drained at the NEXT detect frame



            // (non-blocking), so the NPU stays saturated across frames.



            process_time.tik();



            ret = MultiDetecter.preprocess(orig_img);



            {



                // Hand frame to infer thread; drop oldest if worker is backed up



                // (keeps latency bounded: at most 2 frames in flight).



                std::lock_guard<std::mutex> lk(q_mtx);



                while (frame_queue.size() >= 2) frame_queue.pop();



                frame_queue.push(orig_img);



            }



            q_cv_frame.notify_all();



        }



        /*19-class model: person = class 0*/



        // output_stracks 循环级声明: 检测帧由 tracker.update() 刷新,



        // 非检测帧沿用上一帧结果(显示/围栏判定用最新已知位置)。



        vector<Object> objects;





        // 检测帧: 用最新推理结果构建检测框, 并调用 tracker.update() 推进轨迹;



        // 非检测帧: 不调用 tracker(调用空检测会让所有轨迹 mark_lost,



        // tracklet_len 永远累积不到阈值, 误检/漏检统计全废)。间隔帧内轨迹



        // 状态冻结, 下个检测帧继续匹配——生命周期以"次检测"推进。



        if (is_detect_frame) {



            for(int i = 0; i < ids.size(); i++){



                if(ids[i] == 0){



                    Object obj;



                    obj.label = ids[i];



                    obj.prob = conf[i];



                    obj.rect = boxes[i];



                    objects.push_back(obj);



                }



                else{



                    continue;;



                }



            }



            output_stracks = tracker.update(objects);



        }



        // Print per-frame detection info



        // 抽帧模式下: 非检测帧显示的是上一次检测结果(落后 <= interval 帧),



        // 属预期; 显示帧率(fps)是视频播放速率, 检测率约为 fps/interval。



        static int info_cnt = 0;



        if (++info_cnt % 10 == 1) {



            printf("[INFO f%d] raw_dets=%zu tracked=%zu | confs: ",



                   info_cnt, objects.size(), output_stracks.size());



            for (size_t i = 0; i < objects.size() && i < 5; i++)



                printf("%.2f ", objects[i].prob);



            if (objects.empty()) printf("(none)");



            printf("\n");



        }



        total_frames++;  // 已处理帧数



        // 误检/漏检统计只在检测帧执行(非检测帧沿用上一帧结果, 重复统计



        // 会虚增"消失帧数", 如轨迹刚确认就误计 MISS)。



        if (!is_detect_frame) goto stats_done;



        // False-positive accounting: any confirmed target is a false positive



        // in these no-person test videos. Each track_id counts once.



        if (!no_fp) {



            for (size_t i = 0; i < output_stracks.size(); i++) {



                int tid = output_stracks[i].track_id;



                if (output_stracks[i].label != 0) continue;      // persons only



                if (output_stracks[i].tracklet_len < fp_frames) continue;  // not confirmed yet



                if (false_pos_counted.count(tid)) continue;      // already counted



                false_pos_counted.insert(tid);



                confirmed_false_pos++;



                push_event("[FALSE-POS] track=%d confirmed (total: %d)\n", tid, confirmed_false_pos);



                push_event("[FP-BOX] track=%d box=(%.0f,%.0f %.0fx%.0f) aspect=%.2f\n", tid,



                           output_stracks[i].tlwh[0], output_stracks[i].tlwh[1],



                           output_stracks[i].tlwh[2], output_stracks[i].tlwh[3],



                           (float)output_stracks[i].tlwh[3] / (output_stracks[i].tlwh[2] > 0 ? output_stracks[i].tlwh[2] : 1.0f));



                // 保存误检帧: 带围栏/检测框的当前帧。

                // 文件名: 序号 + 主线程递增计数。
                // 注意: 板子上 std::chrono::steady_clock 连续调用会返回相同纳秒值(实测 c-b=0),
                // 用时间戳做文件名在连续误检时会重复 -> 并发写同一文件 -> JPEG 损坏(灰色区域/打不开)。
                // 主线程 static 递增序号必然唯一, 无时钟依赖。

                // 误检保存完全异步: clone + drawOverlay + imwrite 全在后台线程,
                // 主循环只做轻量数据拷贝(orig_img 是引用+深拷贝, 数据量小), 误检瞬间不阻塞
                // (同步 clone 7MB + 画图 5-7ms 会让主循环停顿, 解码线程积压 -> 恢复后画面"突然加速")。

                if (!fp_save_dir.empty()) {

                    static unsigned long save_seq = 0;

                    char fname[512];

                    snprintf(fname, sizeof(fname), "%s/fp_%03lu_%lu.jpg", fp_save_dir.c_str(), confirmed_false_pos, ++save_seq);

                    push_event("[FP-SAVE] %s box=(%.0f,%.0f %.0fx%.0f) aspect=%.2f\n", fname,

                               output_stracks[i].tlwh[0], output_stracks[i].tlwh[1],

                               output_stracks[i].tlwh[2], output_stracks[i].tlwh[3],

                               (float)output_stracks[i].tlwh[3] / (output_stracks[i].tlwh[2] > 0 ? output_stracks[i].tlwh[2] : 1.0f));

                    // 误检帧保存与 GUI 帧共用同一个写盘队列(FIFO, 不丢帧):
                    // 之前另起 detach 线程并发编码 720p, 与 gui_worker 抢 CPU,
                    // worker 卡顿积压 -> "只取最新丢旧" -> 画面内容前跳(加速/回溯)。
                    // 现在串行 FIFO: 帧时间线单调, 卡顿时表现为短暂延迟而非跳变。
                    GuiFrameTask t;

                    t.frame = orig_img.clone();

                    t.zone = zone_polygon;

                    t.stracks = output_stracks;

                    t.in_zone = person_in_zone;

                    t.path = fname;

                    t.type = 1;   // 误检帧: 原尺寸写盘

                    {

                        std::lock_guard<std::mutex> lk(gui_mtx);

                        gui_queue.push(std::move(t));

                    }

                    gui_cv.notify_one();

                }



            }



        }



        // Miss (lost) accounting: a track that disappeared for >= miss_frames



        // frames counts as one miss episode. Tracks absent briefly (association



        // jitter) do not count.



        if (!fp_only) {



            std::set<int> cur_tids;   // track_ids present in this frame



            for (size_t i = 0; i < output_stracks.size(); i++) {



                cur_tids.insert(output_stracks[i].track_id);



            }



            // Tracks present last frame but absent now: start/continue absence.



            for (int tid : prev_tids) {



                if (!cur_tids.count(tid)) {



                    miss_absent[tid] = miss_absent.count(tid) ? miss_absent[tid] + 1 : 1;



                }



            }



            // Tracks already marked missing and still absent: continue absence.



            // (prev_tids only holds tracks present last frame, so a track absent



            // for 2+ consecutive frames drops out of prev_tids; count it here.)



            for (auto &kv : miss_absent) {



                if (!cur_tids.count(kv.first) && !prev_tids.count(kv.first)) kv.second++;



            }



            // Recovered tracks: if they were gone long enough, count a miss.



            for (int tid : cur_tids) {



                auto it = miss_absent.find(tid);



                if (it != miss_absent.end()) {



                    int gone = it->second;



                    miss_absent.erase(it);



                    if (gone >= miss_frames && !miss_counted.count(tid)) {



                        miss_counted.insert(tid);



                        miss_count++;



                        push_event("[MISS] track=%d lost %d frames (total: %d)\n", tid, gone, miss_count);



                    }



                }



            }



            prev_tids = cur_tids;



        }



        stats_done:



        process_time.tok();  //设置结束时间



        // process_time.print_time("infer ");



        // --- Zone checking & Visualization ---



        // IoB hysteresis: a track already inside stays inside until IoB drops



        // below iob_out (0.1 band below the enter threshold), and vice versa,



        // preventing enter/exit flapping when the box wobbles on the boundary.



        if (!zone_polygon.empty()) {



            for(size_t i = 0; i < output_stracks.size(); i++) {



                vector<float> tlwh = output_stracks[i].tlwh;



                cv::Rect r((int)tlwh[0], (int)tlwh[1], (int)tlwh[2], (int)tlwh[3]);



                int tid = output_stracks[i].track_id;



                // 轨迹确认窗口: 存活太短的瞬时误检不参与围栏进出判定/计数



                if (output_stracks[i].tracklet_len < zone_confirm_frames) continue;



                // 事件: 人物首次出现在画面(未进围栏也算) -> "进入"事件, 一个 id 只记一次
                // person_in_zone 在循环末尾才写入, find 失败即首次出现
                if (person_in_zone.find(tid) == person_in_zone.end())
                    push_event("[ENTER] track=%d\n", tid);



                auto it = person_in_zone.find(tid);



                bool was_inside = (it != person_in_zone.end()) ? it->second : false;



                bool inside;



                float iob = boxIoB(r, zone_polygon, iob_lower);



                bool area_in = was_inside ? (iob > iob_out) : (iob >= iob_in);



                if (zone_mode == 0) {



                    inside = area_in;   // area-only: IoB on the lower part



                } else {



                    // Feet point: bottom-center of the box (near the feet).



                    cv::Point2f foot((float)(r.x + r.width * 0.5f),



                                     (float)(r.y + r.height * 0.90f));



                    double dist = cv::pointPolygonTest(zone_polygon, foot, true);



                    bool foot_in = was_inside ? (dist > -zone_hyst) : (dist > zone_hyst);



                    // hybrid (default): in if feet in OR lower-body area in, so



                    // occluded feet cannot cause a false "exited".



                    inside = (zone_mode == 1) ? foot_in : (foot_in || area_in);



                }



                // 防抖: 只有连续 zone_debounce 次检测同一侧才允许状态翻转,
                // 单帧框抖动(边界 wobble)不再触发反复 ENTERED/EXITED。
                // streak 记录"与当前记录状态相反的一侧"连续出现次数:
                //  - 与 was_inside 相同 -> 清零; 与 was_inside 不同 -> +1
                //  - 达到 zone_debounce -> 翻转 inside 为观察值并计数



                int& streak = zone_side_streak[tid];



                if (inside == was_inside) {



                    streak = 0;



                } else {



                    streak++;



                    if (streak < zone_debounce) {



                        inside = was_inside;   // 未达防抖阈值: 保持原状态, 不翻转不计数



                    }



                }



                // 观察到在围栏外 -> 记录, 之后才允许计"进入"



                if (!inside) track_ever_outside.insert(tid);



                if (inside && !was_inside) {



                    if (allow_born_inside || track_ever_outside.count(tid)) {



                        zone_enter_count++;



                        zone_counted_ids.insert(tid);



                        push_event("[Zone] Person id=%d ENTERED (total in: %d)\n", tid, zone_enter_count);



                        // False-positive zone alert: this track is a confirmed fake



                        // target and it just triggered the fence alarm.



                        if (!no_fp && false_pos_counted.count(tid) && !fp_zone_entered.count(tid)) {



                            fp_zone_entered.insert(tid);



                            zone_false_pos++;



                            push_event("[FALSE-POS] track=%d ZONE ENTERED (zone total: %d)\n", tid, zone_false_pos);



                        }



                    } else {



                        // 首次出现即在围栏内(如机械臂): 视为原本在场, 不计进入/报警



                        push_event("[Zone] Person id=%d born-inside, not counted\n", tid);



                    }



                } else if (!inside && was_inside) {



                    if (zone_counted_ids.count(tid)) {



                        zone_exit_count++;



                        push_event("[Zone] Person id=%d EXITED (total out: %d)\n", tid, zone_exit_count);



                    }



                }



                person_in_zone[tid] = inside;



            }



        }



        // Count persons in current frame



        int person_count = 0;



        for(size_t i = 0; i < output_stracks.size(); i++)



            if(output_stracks[i].label == 0) person_count++;



        // Count persons currently inside the fence zone (used by console + GUI)



        int in_zone_now = 0;



        if (!zone_polygon.empty()) {



            for (size_t i = 0; i < output_stracks.size(); i++) {



                if (output_stracks[i].label != 0) continue;



                auto zit = person_in_zone.find(output_stracks[i].track_id);



                if (zit != person_in_zone.end() && zit->second) in_zone_now++;



            }



        }



        // FPS tracking (before display so g_fps is available for overlay text)



        static int frame_count = 0;



        static double g_fps = 0.0;

        static double g_det_fps = 0.0;

        static int det_count = 0;

        static struct timeval det_tv = {0, 0};



        static struct timeval last_tv = {0, 0};



        frame_count++;

        if (is_detect_frame) det_count++;



        struct timeval now;



        gettimeofday(&now, NULL);



        if(frame_count % 30 == 0) {



            if(last_tv.tv_sec > 0) {



                double elapsed = (now.tv_sec - last_tv.tv_sec) + (now.tv_usec - last_tv.tv_usec) / 1000000.0;



                g_fps = 30.0 / elapsed;



                printf("[Frame %d] %zu tracked, %d persons | zone: %d in / %d out (now %d) | %.1f fps\n",



                       frame_count, output_stracks.size(), person_count,



                       zone_enter_count, zone_exit_count, in_zone_now, g_fps);



            }



            last_tv = now;



        }



            // Display every 3rd frame



            static int disp_skip = 2;  // show first frame immediately



            if (!no_display && ++disp_skip >= 3) {



                disp_skip = 0;



                // Draw fence zone + tracking boxes



                drawOverlay(orig_img, zone_polygon, output_stracks, person_in_zone);



                // Shrink before imshow: X11 forwarding bandwidth is the display



                // bottleneck, not the video. 1280x720@30fps (~83MB/s raw) blows



                // up the SSH channel; 640x360 is what BSL-L era used and is



                // watchable. --disp-scale lets the user trade clarity vs fluidity.



                cv::Mat disp;



                cv::resize(orig_img, disp, cv::Size((int)(orig_img.cols * disp_scale), (int)(orig_img.rows * disp_scale)),



                           0, 0, cv::INTER_NEAREST);



                // --- Dashboard: video left, info panel right (separate side



                // area, video is never covered). Press 'i' to toggle panel.



                cv::Mat combined;



                if (g_show_panel) {



                    std::string vlabel = video_file.empty() ? "GigE" : video_file;



                    size_t p = vlabel.find_last_of("/\\");



                    if (p != std::string::npos) vlabel = vlabel.substr(p + 1);



                    cv::Mat panel;



                    buildInfoPanel(panel, disp.rows, vlabel, video_width * 2, video_height * 2, video_fps,



                                   total_frames, g_fps, g_det_fps, objects.size(), output_stracks.size(), person_count,



                                   zone_enter_count, zone_exit_count, in_zone_now,



                                   iob_in, iob_out, confirmed_false_pos, zone_false_pos, miss_count,



                                   no_fp, fp_only, fp_frames, miss_frames, g_event_log);



                    cv::hconcat(disp, panel, combined);



                } else {



                    combined = disp;



                }



                cv::imshow("Zone Detection", combined);



                int key = cv::waitKey(1);



                if (key == 'i' || key == 'I') g_show_panel = !g_show_panel;



            }



            // GUI frame export: draw overlay on a copy and save JPEG



            // GUI 读取 frame.jpg 是 250ms 一次, 逐帧写盘(30/s)纯浪费,

            // 且长时间持续写 eMMC 会触发写放大/回收, 主循环每帧被 imwrite

            // 拖住 -> 长时间运行后 FPS 掉一半(7000帧后 29->16.8 的根因)。

            // 节流: 距上次写入 >=100ms 才写(约 8-10/s, GUI 观感无差异)。



            if (!gui_frame_path.empty()) {

                // 入队给 worker 线程画图+编码+原子写盘(不再同步阻塞主循环):
                // 每帧都入队(不节流): clone 7MB 深拷贝是主循环唯一成本(~1ms),
                // worker 只取最新任务(丢弃积压旧帧), 写盘最坏落后一帧, 画面刷新率
                // 回到 ~29fps(GUI 显示帧率 = 处理帧率)。
                // (历史坑: 这里曾残留旧同步版 clone+drawOverlay+resize 死代码,
                //  每帧在主循环多花 ~10-15ms -> 偶发卡顿 -> 解码追赶 -> 画面跳变。)
                GuiFrameTask t;

                t.frame = orig_img.clone();   // 7MB 深拷贝, worker 独占可画可写

                t.zone = zone_polygon;

                t.stracks = output_stracks;

                t.in_zone = person_in_zone;

                t.path = gui_frame_path;

                t.type = 0;   // GUI 显示帧

                {

                    std::lock_guard<std::mutex> lk(gui_mtx);

                    gui_queue.push(std::move(t));

                }

                gui_cv.notify_one();

            }



    }



    // Stop infer thread and drain any remaining frames



    {



        std::lock_guard<std::mutex> lk(q_mtx);



        infer_stop = true;



    }



    q_cv_frame.notify_all();



    if (infer_thread.joinable()) infer_thread.join();



    // Stop decode thread (video mode)



    {



        std::lock_guard<std::mutex> lk(dec_mtx);



        dec_stop = true;



    }



    dec_cv.notify_all();



    if (dec_thread.joinable()) dec_thread.join();

    // Stop GUI-frame worker: 让 worker 把队列里最后一张写完再退出(不丢最后一帧)

    {

        std::lock_guard<std::mutex> lk(gui_mtx);

        gui_stop = true;

    }

    gui_cv.notify_all();

    if (gui_worker.joinable()) gui_worker.join();



    // Tracks still missing at video end: count them if absent long enough.



    if (!fp_only) {



        for (auto &kv : miss_absent) {



            if (kv.second >= miss_frames && !miss_counted.count(kv.first)) {



                miss_counted.insert(kv.first);



                miss_count++;



                push_event("[MISS] track=%d lost %d frames (total: %d, video end)\n",



                       kv.first, kv.second, miss_count);



            }



        }



    }



    // --- False-positive stats summary ---



    if (!no_fp) {



        printf("[STATS] frames=%d  confirmed_false_pos=%d  zone_false_pos=%d\n",



               total_frames, confirmed_false_pos, zone_false_pos);



    }



    if (!fp_only) {



        printf("[STATS] frames=%d  miss_count=%d\n",



               total_frames, miss_count);



    }



    if (!video_file.empty()) {



        std::ofstream sf("stats.txt", std::ios::app);



        if (sf.is_open()) {



            sf << video_file << " frames=" << total_frames;



            if (!no_fp)



                sf << " confirmed_false_pos=" << confirmed_false_pos



                   << " zone_false_pos=" << zone_false_pos;



            if (!fp_only)



                sf << " miss_count=" << miss_count;



            sf << "\n";



            sf.close();



            printf("[STATS] appended to stats.txt\n");



        } else {



            printf("[STATS] WARNING: cannot open stats.txt\n");



        }



    }



    if (video_file.empty()) {



        arv_camera_stop_acquisition(camera, NULL);



        g_object_unref(stream);



        g_object_unref(camera);



    } else {



        video_cap.release();



    }



    printf("end test... \n");



    return 0;



}



