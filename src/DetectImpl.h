#pragma once
#include "Detect.h"
#include "postprocess.h"
#include "resize_function.h"
#include "rknn_utils.h"
#include "timer.h"
#include <sys/time.h>
#include "coreNum.hpp"


using namespace multi_det;

static void dump_tensor_attr(rknn_tensor_attr *attr);
static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz);
static unsigned char *load_model(const char *filename, int *model_size);
static int saveFloat(const char *file_name, float *output, int element_size);

// class ItcMultiDetect::Impl
class Detect::Impl
{
public:
    Impl();
    ~Impl();

    int init_model(const char *model_path, const float &nmsThreshold, const float &boxThreshold, const int &NPUcore, const std::vector<float> &confs);
    int detect(const cv::Mat &image, std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes);
    int preprocess(const cv::Mat &image);
    int infer(std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes);
    rknn_context *get_pctx();

private:
    TIMER timer;

    // MODEL_INFO m_info;
    // YOLO_INFO y_info;
    LETTER_BOX letter_box;

    float nms_threshold = 0.45;
    float conf_threshold = 0.25;
    std::vector<void *> output_buf_list;
    std::vector<float> conf_thresholds;

    
    //20240919
    int ret;
    // std::mutex mtx;
    std::string model_path;   // saved from init_model for post_process semantics
    int force_xyxy = 0;       // 1 = model emits XYXY bbox (v4 family), skip probe
    unsigned char *model_data;
    int channel, width, height;   //模型尺寸
    int img_width, img_height;
    bool is_quant = true;

    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    rknn_input inputs[1];
    // 双缓冲修复(2026-08-07): preprocess 在"主线程"写, infer 在"工作线程"
    // 跑, 原 static cv::Mat 复用导致 NPU 读第 N 帧时被第 N+1 帧覆写
    // (BSL-L 帧间变化小没事, BSL-R/BSO-L 人物动态大 → 全检测不到)。
    // 两块 buffer 交替: preprocess 写完立即 rknn_inputs_set 提交, NPU 读的
    // buffer 直到下一轮同 index 才被覆写(间隔一次推理, 安全)。
    cv::Mat rgb_buf[2];
    int rgb_cur = 0;


// private:
//     int detect_zero_copy(cv::Mat rgb_img, detect_result_group_t *group);
//     int detect_normal(cv::Mat rgb_img, detect_result_group_t *group);
};