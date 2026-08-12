#include <iostream>
#include "DetectImpl.h"

using namespace multi_det;

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    std::string shape_str = attr->n_dims < 1 ? "" : std::to_string(attr->dims[0]);
    for (int i = 1; i < attr->n_dims; ++i)
    {
        shape_str += ", " + std::to_string(attr->dims[i]);
    }

    printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, w_stride = %d, size_with_stride=%d, fmt=%s, "
           "type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, shape_str.c_str(), attr->n_elems, attr->size, attr->w_stride,
           attr->size_with_stride, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(const char *filename, int *model_size)
{
    FILE *fp;
    unsigned char *data;

    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

static int saveFloat(const char *file_name, float *output, int element_size)
{
    FILE *fp;
    fp = fopen(file_name, "w");
    for (int i = 0; i < element_size; i++)
    {
        fprintf(fp, "%.6f\n", output[i]);
    }
    fclose(fp);
    return 0;
}


Detect::Impl::Impl()
{
    nms_threshold = NMS_THRESHOLD;      // 默认的NMS阈值
    conf_threshold = CONF_THRESHOLD; // 默认的置信度阈值
}

Detect::Impl::~Impl()
{
    deinitPostProcess();

    ret = rknn_destroy(ctx);

    if (model_data)
        free(model_data);

    if (input_attrs)
        free(input_attrs);
    if (output_attrs)
        free(output_attrs);
}

int Detect::Impl::init_model(const char *model_path, const float &nmsThreshold, const float &boxThreshold, const int &NPUcore, const std::vector<float> &confs)
{
    nms_threshold = nmsThreshold;
    conf_threshold = boxThreshold;

    if (confs.size() != OBJ_CLASS_NUM)
    {
        printf("Error: input confs.size() != OBJ_CLASS_NUM ! \n");
        return -1;
    }
    
    conf_thresholds = confs;

    // Keep model path for post_process field semantics.
    // All yolov26s exports (v2/v3/v4 family) emit XYXY bbox; only the legacy
    // personDetect2 model emits cxcywh. Default to XYXY, probe only for
    // personDetect* models.
    std::string mp = model_path ? model_path : "";
    size_t slash = mp.find_last_of('/');
    size_t back = mp.find_last_of('\\');
    if (back != std::string::npos && (slash == std::string::npos || back > slash)) slash = back;
    std::string name = (slash == std::string::npos) ? mp : mp.substr(slash + 1);
    force_xyxy = (name.find("personDetect") == std::string::npos) ? 1 : 0;
    printf("Detect model: %s (force_xyxy=%d)\n", name.c_str(), force_xyxy);



    /* 加载模型 */
    printf("Loading model...\n");
    int model_data_size = 0;
    model_data = load_model(model_path, &model_data_size);

    //模型参数复用
    // if (share_weight == true){
    //     ret = rknn_dup_context(ctx_in, &ctx);
    // }
    // else{
    ret = rknn_init(&ctx, model_data, model_data_size, 0, NULL);
    // }
   
    if (ret < 0)
    {
        std::cout << "rknn_util_init failed" << std::endl;
        return -1;
    }

    // 设置模型绑定的核心/Set the core of the model that needs to be bound
    rknn_core_mask core_mask;
    switch (NPUcore)
    {
    case -1:
        core_mask = RKNN_NPU_CORE_AUTO;
        break;
    case 0:
        core_mask = RKNN_NPU_CORE_0;
        break;
    case 1:
        core_mask = RKNN_NPU_CORE_1;
        break;
    case 2:
        core_mask = RKNN_NPU_CORE_2;
        break;
    case 3:
        core_mask = RKNN_NPU_CORE_0_1_2;
        break;
    }
    ret = rknn_set_core_mask(ctx, core_mask);
    if (ret < 0)
    {
        printf("rknn_init core error ret=%d\n", ret);
        return -1;
    }
    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version, version.drv_version);

    // 获取模型输入输出参数/Obtain the input and output parameters of the model
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // 设置输入参数/Set the input parameters
    input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    // 设置输出参数/Set the output parameters
    output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        dump_tensor_attr(&(output_attrs[i]));
    }

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        channel = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        width = input_attrs[0].dims[3];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        height = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }
    printf("model input height=%d, width=%d, channel=%d\n", height, width, channel);

    memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = width * height * channel;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].pass_through = 0;
 
    return 0;
}  

rknn_context* Detect::Impl::get_pctx()
{
    return &ctx;
}


int Detect::Impl::detect(const cv::Mat &image, std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes)
{
    if (preprocess(image) != 0) return -1;
    return infer(ids, conf, boxes);
}

int Detect::Impl::preprocess(const cv::Mat &image)
{
    if (image.empty())
    {
        std::cout << "cannot open image..." << std::endl;
        return -1;
    }

    img_width = image.cols;
    img_height = image.rows;

    letter_box.in_height = image.rows;
    letter_box.in_width = image.cols;
    letter_box.channel = image.channels();
    letter_box.target_width = width;
    letter_box.target_height = height;

    // 图像缩放/Image scaling
    // 双缓冲(2026-08-07): 不要用 static 复用 —— 主线程 preprocess 与工作
    // 线程 infer 并行, static 共享会让 NPU 读第 N 帧时被第 N+1 帧覆写。
    // rgb_buf[2] 交替使用, 写完立即 rknn_inputs_set 提交。
    cv::Mat &s_resized_img = rgb_buf[rgb_cur];
    cv::Mat &s_rgb_img = rgb_buf[rgb_cur];
    rgb_cur ^= 1;  // 下一帧用另一块
    if (img_width != width || img_height != height)
    {
        compute_letter_box(&letter_box);
        letter_box.reverse_available = true;
        if (image.rows != letter_box.resize_height || image.cols != letter_box.resize_width)
        {
            s_resized_img = cv::Mat(letter_box.resize_height, letter_box.resize_width, CV_8UC3);
            cv::resize(image, s_resized_img, cv::Size(letter_box.resize_width, letter_box.resize_height), 0, 0, cv::INTER_NEAREST);
            cv::copyMakeBorder(s_resized_img, s_resized_img, letter_box.h_pad_top, letter_box.h_pad_bottom, letter_box.w_pad_left,
                               letter_box.w_pad_right, cv::BORDER_CONSTANT, cv::Scalar(114, 114, 114));
        }
        else
        {
            cv::copyMakeBorder(image, s_resized_img, letter_box.h_pad_top,
                               letter_box.h_pad_bottom, letter_box.w_pad_left,
                               letter_box.w_pad_right, cv::BORDER_CONSTANT,
                               cv::Scalar(114, 114, 114));
        }
        // v3 model trained on RGB; RKNN inputs are BGR from cv::VideoCapture
        cv::cvtColor(s_resized_img, s_rgb_img, cv::COLOR_BGR2RGB);
        inputs[0].buf = s_rgb_img.data;
    }
    else
    {
        // No resize needed; convert directly
        cv::cvtColor(image, s_rgb_img, cv::COLOR_BGR2RGB);
        inputs[0].buf = s_rgb_img.data;
    }

    // 立即提交输入(NPU 开始拷贝/读取时 buffer 还是本帧完整内容)
    rknn_inputs_set(ctx, io_num.n_input, inputs);

    return 0;
}

int Detect::Impl::infer(std::vector<int> *ids, std::vector<float> *conf, std::vector<cv::Rect> *boxes)
{
    ids->clear();
    conf->clear();
    boxes->clear();

    // 注意: rknn_inputs_set 已前移到 preprocess 末尾(双缓冲提交),
    // 保证 NPU 读取的输入是完整帧, 不被主线程下一帧覆写。
    // rknn_inputs_set(ctx, io_num.n_input, inputs);

    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 1;  // Request FP32 to handle both INT8 and FP16 models
    }

    int ret = 0;
    // 模型推理/Model inference
    {
        static int infer_count = 0;
        static double infer_total = 0.0;
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        ret = rknn_run(ctx, NULL);
        ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
        gettimeofday(&t1, NULL);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
        infer_total += ms;
        infer_count++;
        if(infer_count % 30 == 0) {
            printf("[NPU] %d inferences, avg %.1f ms (%.0f FPS)\n",
                   infer_count, infer_total / infer_count, 1000.0 / (infer_total / infer_count));
        }
    }

    // 后处理/Post-processing
    detect_result_group_t detect_result_group;

    post_process(ctx, outputs, &letter_box, conf_threshold, nms_threshold, &detect_result_group, width, height, io_num, output_attrs, true, force_xyxy);

    ret = rknn_outputs_release(ctx, io_num.n_output, outputs);

    for (int i = 0; i < detect_result_group.count; i++)      //解码后的预测框存放在detect_result_group
    {
        detect_result_t *det_result = &(detect_result_group.results[i]);

        // filter by conf_threshold
        if (det_result->prop < conf_thresholds[det_result->class_index])  //可对不同类别设置置信度阈值，再次进行过滤
        {
            continue;
        }

        ids->push_back(det_result->class_index);
        conf->push_back(det_result->prop);
        boxes->push_back(cv::Rect(det_result->box.left, det_result->box.top, det_result->box.right - det_result->box.left, det_result->box.bottom - det_result->box.top));
    }

    return ret;
}