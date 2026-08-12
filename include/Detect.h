#pragma once
#include <opencv2/opencv.hpp>
#include <rknn_api.h>

class Detect
{
public:
    Detect();
    ~Detect();

    int init_model(const char *model_path, const float &nmsThreshold, const float &boxThreshold, const int &NPUcore,const std::vector<float> &confs);
    int detect(const cv::Mat &image, std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes);

    // Pipeline split: preprocess (letterbox+RGB, ~63ms) and infer (NPU, ~61ms)
    // can overlap across frames when called from a consumer thread.
    int preprocess(const cv::Mat &image);   // returns 0 on success
    int infer(std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes);

public:
    class Impl;
    Impl *pImpl;
};