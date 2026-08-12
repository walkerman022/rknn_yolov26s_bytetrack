#include <iostream>
#include <algorithm>
#include "postprocess.h"
#include "resize_function.h"
#include "set"
// #include <rkYolov8n.h>

static char *labels[OBJ_CLASS_NUM];

namespace multi_det
{

    inline static int clamp(float val, int min, int max)
    {
        return val > min ? (val < max ? val : max) : min;
    }

    int readFloats(const char *fileName, float *result, int max_line, int *valid_number)
    {
        FILE *file = fopen(fileName, "r");
        if (file == NULL)
        {
            printf("failed to open file\n");
            return 1;
        }

        int n = 0;
        while ((n <= max_line) && (fscanf(file, "%f", &result[n++]) != EOF))
            ;

        /* n-1 float values were successfully read */
        // for (int i=0; i<n-1; i++)
        //     printf("fval[%d]=%f\n", i, result[i]);

        fclose(file);
        *valid_number = n - 1;
        return 0;
    }

    static float CalculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0, float xmin1, float ymin1, float xmax1,
                                float ymax1)
    {
        float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
        float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
        float i = w * h;
        float u = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) + (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - i;
        return u <= 0.f ? 0.f : (i / u);
    }


    static int nms(int validCount, std::vector<float> &outputLocations, std::vector<int> classIds, std::vector<int> &order,
               int filterId, float threshold)
{
    for (int i = 0; i < validCount; ++i)
    {
        int n = order[i];
        if (n == -1 || classIds[n] != filterId)
        {
            continue;
        }
        for (int j = i + 1; j < validCount; ++j)
        {
            int m = order[j];
            if (m == -1 || classIds[m] != filterId)
            {
                continue;
            }
            float xmin0 = outputLocations[n * 4 + 0];
            float ymin0 = outputLocations[n * 4 + 1];
            float xmax0 = outputLocations[n * 4 + 0] + outputLocations[n * 4 + 2];
            float ymax0 = outputLocations[n * 4 + 1] + outputLocations[n * 4 + 3];

            float xmin1 = outputLocations[m * 4 + 0];
            float ymin1 = outputLocations[m * 4 + 1];
            float xmax1 = outputLocations[m * 4 + 0] + outputLocations[m * 4 + 2];
            float ymax1 = outputLocations[m * 4 + 1] + outputLocations[m * 4 + 3];

            float iou = CalculateOverlap(xmin0, ymin0, xmax0, ymax0, xmin1, ymin1, xmax1, ymax1);

            if (iou > threshold)
            {
                order[j] = -1;
            }
        }
    }
    return 0;
}

    static int quick_sort_indice_inverse(std::vector<float> &input, int left, int right, std::vector<int> &indices)
    {
        float key;
        int key_index;
        int low = left;
        int high = right;
        if (left < right)
        {
            key_index = indices[left];
            key = input[left];
            while (low < high)
            {
                while (low < high && input[high] <= key)
                {
                    high--;
                }
                input[low] = input[high];
                indices[low] = indices[high];
                while (low < high && input[low] >= key)
                {
                    low++;
                }
                input[high] = input[low];
                indices[high] = indices[low];
            }
            input[low] = key;
            indices[low] = key_index;
            quick_sort_indice_inverse(input, left, low - 1, indices);
            quick_sort_indice_inverse(input, low + 1, right, indices);
        }
        return low;
    }

    static float sigmoid(float x) { return 1.0 / (1.0 + expf(-x)); }

    static float unsigmoid(float y) { return -1.0 * logf((1.0 / y) - 1.0); }



    inline static int32_t __clip(float val, float min, float max)
    {
        float f = val <= min ? min : (val >= max ? max : val);
        return f;
    }

    static int8_t qnt_f32_to_affine(float f32, int32_t zp, float scale)
    {
        float dst_val = (f32 / scale) + zp;
        int8_t res = (int8_t)__clip(dst_val, -128, 127);
        return res;
    }

    static uint8_t qnt_f32_to_affine_u8(float f32, int32_t zp, float scale)
    {
        float dst_val = (f32 / scale) + zp;
        uint8_t res = (uint8_t)__clip(dst_val, 0, 255);
        return res;
    }

    static float deqnt_affine_u8_to_f32(uint8_t qnt, int32_t zp, float scale) 
    { 
        return ((float)qnt - (float)zp) * scale; 
    }

    static float deqnt_affine_to_f32(int8_t qnt, int32_t zp, float scale)
    {
        return ((float)qnt - (float)zp) * scale;
    }

    // void query_dfl_len(MODEL_INFO *m, YOLO_INFO *y_info)
    // {
    //     // set dfl_len
    //     if (m->n_output > 6)
    //     {
    //         y_info->score_sum_available = true;
    //     }
    //     else
    //     {
    //         y_info->score_sum_available = false;
    //     }

    //     // dump_tensor_attr(&m->out_attr[0]);
    //     if (m->out_attr[0].dims[1] != 4)
    //     {
    //         y_info->dfl_len = (int)(m->out_attr[0].dims[1] / 4);
    // #ifdef RKNN_NPU_1
    //         // NCHW reversed: WHCN
    //         int dfl_len = (int)(m->out_attr[0].dims[2] / 4);
    // #else
    //         int dfl_len = (int)(m->out_attr[0].dims[1] / 4);
    // #endif
    //     }
    // }

    static void compute_dfl(float *tensor, int dfl_len, float *box)
    {
        for (int b = 0; b < 4; b++)
        {
            float exp_t[dfl_len];
            float exp_sum = 0;
            float acc_sum = 0;
            for (int i = 0; i < dfl_len; i++)
            {
                exp_t[i] = exp(tensor[i + b * dfl_len]);
                exp_sum += exp_t[i];
            }

            for (int i = 0; i < dfl_len; i++)
            {
                acc_sum += exp_t[i] / exp_sum * i;
            }
            box[b] = acc_sum;
        }
    }
    static int process_u8(uint8_t *box_tensor, int32_t box_zp, float box_scale,
                      uint8_t *score_tensor, int32_t score_zp, float score_scale,
                      uint8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                      int grid_h, int grid_w, int stride, int dfl_len,
                      std::vector<float> &boxes,
                      std::vector<float> &objProbs,
                      std::vector<int> &classId,
                      float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    uint8_t score_thres_u8 = qnt_f32_to_affine_u8(threshold, score_zp, score_scale);
    uint8_t score_sum_thres_u8 = qnt_f32_to_affine_u8(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // Use score sum to quickly filter
            if (score_sum_tensor != nullptr)
            {
                if (score_sum_tensor[offset] < score_sum_thres_u8)
                {
                    continue;
                }
            }

            uint8_t max_score = -score_zp;
            for (int c = 0; c < OBJ_CLASS_NUM; c++)
            {
                if ((score_tensor[offset] > score_thres_u8) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            // compute box
            if (max_score > score_thres_u8)
            {
                offset = i * grid_w + j;
                float box[4];
                float before_dfl[dfl_len * 4];
                for (int k = 0; k < dfl_len * 4; k++)
                {
                    before_dfl[k] = deqnt_affine_u8_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);
            

                objProbs.push_back(deqnt_affine_u8_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount++;
            }
        }
    }
    return validCount;
}

   static int process_i8(int8_t *box_tensor, int32_t box_zp, float box_scale,
                      int8_t *score_tensor, int32_t score_zp, float score_scale,
                      int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                      int grid_h, int grid_w, int stride, int dfl_len,
                      std::vector<float> &boxes, 
                      std::vector<float> &objProbs, 
                      std::vector<int> &classId, 
                      float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i* grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr){
                if (score_sum_tensor[offset] < score_sum_thres_i8){
                    continue;
                }
            }

            int8_t max_score = -score_zp;
            // Only check class 0 (person) — skip the 79-class loop
            {
                int c = 0;  // person
                if ((score_tensor[offset + c * grid_len] > score_thres_i8) && (score_tensor[offset + c * grid_len] > max_score))
                {
                    max_score = score_tensor[offset + c * grid_len];
                    max_class_id = c;
                }
            }

            // compute box
            if (max_score> score_thres_i8){
                offset = i* grid_w + j;
                float box[4];
                float before_dfl[dfl_len*4];
                for (int k=0; k< dfl_len*4; k++){
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset], box_zp, box_scale);
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1,y1,x2,y2,w,h;
                x1 = (-box[0] + j + 0.5)*stride;
                y1 = (-box[1] + i + 0.5)*stride;
                x2 = (box[2] + j + 0.5)*stride;
                y2 = (box[3] + i + 0.5)*stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);
                // std::cout << x1 << std::endl;
                // std::cout << y1 << std::endl;

                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount ++;
            }
        }
    }
    return validCount;
}

    static int process_fp32(float *box_tensor, float *score_tensor, float *score_sum_tensor, 
                        int grid_h, int grid_w, int stride, int dfl_len,
                        std::vector<float> &boxes, 
                        std::vector<float> &objProbs, 
                        std::vector<int> &classId, 
                        float threshold)
{
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    for (int i = 0; i < grid_h; i++)
    {
        for (int j = 0; j < grid_w; j++)
        {
            int offset = i* grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr){
                if (score_sum_tensor[offset] < threshold){
                    continue;
                }
            }

            float max_score = 0;
            for (int c= 0; c< OBJ_CLASS_NUM; c++){
                if ((score_tensor[offset] > threshold) && (score_tensor[offset] > max_score))
                {
                    max_score = score_tensor[offset];
                    max_class_id = c;
                }
                offset += grid_len;
            }

            // compute box
            if (max_score> threshold){
                offset = i* grid_w + j;
                float box[4];
                float before_dfl[dfl_len*4];
                for (int k=0; k< dfl_len*4; k++){
                    before_dfl[k] = box_tensor[offset];
                    offset += grid_len;
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1,y1,x2,y2,w,h;
                x1 = (-box[0] + j + 0.5)*stride;
                y1 = (-box[1] + i + 0.5)*stride;
                x2 = (box[2] + j + 0.5)*stride;
                y2 = (box[3] + i + 0.5)*stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(max_score);
                classId.push_back(max_class_id);
                validCount ++;
            }
        }
    }
    return validCount;
}


#if defined(RV1106_1103)
static int process_i8_rv1106(int8_t *box_tensor, int32_t box_zp, float box_scale,
                             int8_t *score_tensor, int32_t score_zp, float score_scale,
                             int8_t *score_sum_tensor, int32_t score_sum_zp, float score_sum_scale,
                             int grid_h, int grid_w, int stride, int dfl_len,
                             std::vector<float> &boxes,
                             std::vector<float> &objProbs,
                             std::vector<int> &classId,
                             float threshold) {
    int validCount = 0;
    int grid_len = grid_h * grid_w;
    int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t score_sum_thres_i8 = qnt_f32_to_affine(threshold, score_sum_zp, score_sum_scale);

    for (int i = 0; i < grid_h; i++) {
        for (int j = 0; j < grid_w; j++) {
            int offset = i * grid_w + j;
            int max_class_id = -1;

            // 通过 score sum 起到快速过滤的作用
            if (score_sum_tensor != nullptr) {
                //score_sum_tensor [1, 1, 80, 80]
                if (score_sum_tensor[offset] < score_sum_thres_i8) {
                    continue;
                }
            }

            int8_t max_score = -score_zp;
            offset = offset * OBJ_CLASS_NUM;
            for (int c = 0; c < OBJ_CLASS_NUM; c++) {
                if ((score_tensor[offset + c] > score_thres_i8) && (score_tensor[offset + c] > max_score)) {
                    max_score = score_tensor[offset + c]; //80类 [1, 80, 80, 80] 3588NCHW 1106NHWC
                    max_class_id = c;
                }
            }

            // compute box
            if (max_score > score_thres_i8) {
                offset = (i * grid_w + j) * 4 * dfl_len;
                float box[4];
                float before_dfl[dfl_len*4];
                for (int k=0; k< dfl_len*4; k++){
                    before_dfl[k] = deqnt_affine_to_f32(box_tensor[offset + k], box_zp, box_scale);
                }
                compute_dfl(before_dfl, dfl_len, box);

                float x1, y1, x2, y2, w, h;
                x1 = (-box[0] + j + 0.5) * stride;
                y1 = (-box[1] + i + 0.5) * stride;
                x2 = (box[2] + j + 0.5) * stride;
                y2 = (box[3] + i + 0.5) * stride;
                w = x2 - x1;
                h = y2 - y1;
                boxes.push_back(x1);
                boxes.push_back(y1);
                boxes.push_back(w);
                boxes.push_back(h);

                objProbs.push_back(deqnt_affine_to_f32(max_score, score_zp, score_scale));
                classId.push_back(max_class_id);
                validCount ++;
            }
        }
    }
    printf("validCount=%d\n", validCount);
    printf("grid h-%d, w-%d, stride %d\n", grid_h, grid_w, stride);
    return validCount;
}
#endif
    
int post_process(rknn_context ctx, void *outputs, LETTER_BOX *letter_box, float conf_threshold, float nms_threshold, detect_result_group_t *od_results,int width, int height,
rknn_input_output_num io_num, rknn_tensor_attr *output_attrs, bool is_quant, int force_xyxy)
{

#if defined(RV1106_1103)
    rknn_tensor_mem **_outputs = (rknn_tensor_mem **)outputs;
#else
    rknn_output *_outputs = (rknn_output *)outputs;
#endif
    std::vector<float> filterBoxes;
    std::vector<float> objProbs;
    std::vector<int> classId;
    int validCount = 0;
    int stride = 0;
    int grid_h = 0;
    int grid_w = 0;
    int model_in_w = width;
    int model_in_h = height;
    memset(od_results, 0, sizeof(detect_result_group_t));

    // Handle single output
    if (io_num.n_output == 1) {
        bool is_fp = (output_attrs[0].type == RKNN_TENSOR_FLOAT16 || output_attrs[0].type == RKNN_TENSOR_FLOAT32);
        // RKNN reports output dims either as [1,N,6] (3-D) or [N,6] (2-D).
        // Read dims safely according to n_dims; never touch dims[n_dims..].
        int n_ch = (output_attrs[0].n_dims >= 2) ? (int)output_attrs[0].dims[1] : -1;
        int total_det = -1;
        if (output_attrs[0].n_dims == 3) total_det = (int)output_attrs[0].dims[2];
        else if (output_attrs[0].n_dims == 2) total_det = (int)output_attrs[0].dims[0];

        // YOLOv26 format: decoded detections [x1,y1,x2,y2,conf,cls].
        // Matches [1,N,6] (3-D) or [N,6] (2-D), N <= 300 (TopK limit).
        bool is_v26 = (total_det > 0 && total_det <= 300) &&
                      ((output_attrs[0].n_dims == 3 && n_ch == 6) ||
                       (output_attrs[0].n_dims == 2 && n_ch == 6));
        if (is_v26) {
            float *data = (float *)_outputs[0].buf;
            // Layout: row-major [N,6] (stride 6, verified on simulator:
            //   det k lives at data[k*6 .. k*6+5], fields x1,y1,x2,y2,conf,cls)
            // or column-major [6,N] (stride N). Probe: in row-major, data[5] is
            // the first det's cls (integer 0 for single-class); in column-major
            // data[4] is the first conf (0..1) and data[5] is a coordinate.
            // Default row-major; fall back to column-major only when the probe
            // clearly indicates it (data[4] in 0..1, data[5] not 0/1).
            int ch_stride = 6;
            if (total_det > 0) {
                float d4 = data[4], d5 = data[5];
                bool row_major = (d5 >= -0.01f && d5 <= 1.01f && d4 >= -0.01f && d4 <= 1.01f);
                if (!row_major) {
                    // column-major probe: d4 = first conf, d5 = first x2
                    bool col_major = (d4 >= 0.0f && d4 <= 1.0f) && (d5 > 1.0f || d5 < 0.0f);
                    if (col_major) ch_stride = total_det;
                }
            }
            static bool once26 = false;
            if (!once26) {
                printf("Using YOLOv26 decoded format [%d,%d,%d] (%s) fmt=%s ch_stride=%d "
                       "(probe d4=%.3f d5=%.3f)\n",
                       (int)output_attrs[0].dims[0], n_ch, total_det, is_fp ? "FP" : "INT8",
                       get_format_string(output_attrs[0].fmt), ch_stride, data[4], data[5]);
                once26 = true;
            }
            for (int k = 0; k < total_det; k++) {
                float x1, y1, x2, y2, score;
                if (ch_stride == 6) {
                    // row-major [N,6]: det k at data[k*6 .. k*6+5]
                    int off = k * 6;
                    x1 = data[off + 0]; y1 = data[off + 1];
                    x2 = data[off + 2]; y2 = data[off + 3];
                    score = data[off + 4];
                } else {
                    // column-major [6,N]: field c at data[k + c*N]
                    x1 = data[k + 0*ch_stride]; y1 = data[k + 1*ch_stride];
                    x2 = data[k + 2*ch_stride]; y2 = data[k + 3*ch_stride];
                    score = data[k + 4*ch_stride];
                }
                if (score < conf_threshold) continue;
                if (x2 <= x1 || y2 <= y1) continue;
                filterBoxes.push_back(x1); filterBoxes.push_back(y1);
                filterBoxes.push_back(x2 - x1); filterBoxes.push_back(y2 - y1);
                objProbs.push_back(score); classId.push_back(0); validCount++;
            }
        } else if ((n_ch == 5 && total_det > 300) || (n_ch > 300 && total_det == 5)) {
            // New v26 bare export (one2one, no TopK): [1, 8400, 5] row-major
            // cx,cy,bw,bh,score in pixel coords. Two dim layouts are seen in the
            // wild: simulator reports [1,8400,5] but the board may report
            // [1,5,8400] with the same row-major buffer. Either way 8400 vs 5
            // distinguishes it from the old v11 [1,5,8400] column-major format.
            int anchors = (total_det > 300) ? total_det : n_ch;  // anchor count = 8400
            if (!is_fp) {
                int8_t *data = (int8_t *)_outputs[0].buf;
                int32_t zp = output_attrs[0].zp;
                float scale = output_attrs[0].scale;
                for (int k = 0; k < anchors; k++) {
                    float cx = deqnt_affine_to_f32(data[k*5 + 0], zp, scale);
                    float cy = deqnt_affine_to_f32(data[k*5 + 1], zp, scale);
                    float bw = deqnt_affine_to_f32(data[k*5 + 2], zp, scale);
                    float bh = deqnt_affine_to_f32(data[k*5 + 3], zp, scale);
                    float score = deqnt_affine_to_f32(data[k*5 + 4], zp, scale);
                    if (score < conf_threshold) continue;
                    float x1 = cx - bw * 0.5f, y1 = cy - bh * 0.5f;
                    float x2 = cx + bw * 0.5f, y2 = cy + bh * 0.5f;
                    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
                    if (x2 > model_in_w) x2 = model_in_w;
                    if (y2 > model_in_h) y2 = model_in_h;
                    if (x1 >= x2 || y1 >= y2) continue;
                    filterBoxes.push_back(x1); filterBoxes.push_back(y1);
                    filterBoxes.push_back(x2 - x1); filterBoxes.push_back(y2 - y1);
                    objProbs.push_back(score); classId.push_back(0); validCount++;
                }
            } else {
                float *data = (float *)_outputs[0].buf;
                for (int k = 0; k < anchors; k++) {
                    float cx = data[k*5 + 0], cy = data[k*5 + 1];
                    float bw = data[k*5 + 2], bh = data[k*5 + 3];
                    float score = data[k*5 + 4];
                    if (score < conf_threshold) continue;
                    float x1 = cx - bw * 0.5f, y1 = cy - bh * 0.5f;
                    float x2 = cx + bw * 0.5f, y2 = cy + bh * 0.5f;
                    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
                    if (x2 > model_in_w) x2 = model_in_w;
                    if (y2 > model_in_h) y2 = model_in_h;
                    if (x1 >= x2 || y1 >= y2) continue;
                    filterBoxes.push_back(x1); filterBoxes.push_back(y1);
                    filterBoxes.push_back(x2 - x1); filterBoxes.push_back(y2 - y1);
                    objProbs.push_back(score); classId.push_back(0); validCount++;
                }
            }
        } else {
            // Original YOLOv8/v11 format: [1, 5, 8400]
            static bool once = false;
            if (!once) {
                printf("Using single-output decode path [1,%d,%d] (%s)\n", n_ch, total_det, is_fp ? "FP" : "INT8");
                once = true;
            }
            if (!is_fp) {
                int8_t *data = (int8_t *)_outputs[0].buf;
                int32_t zp = output_attrs[0].zp;
                float scale = output_attrs[0].scale;
                for (int k = 0; k < total_det; k++) {
                    float cx  = deqnt_affine_to_f32(data[k + 0*total_det], zp, scale);
                    float cy  = deqnt_affine_to_f32(data[k + 1*total_det], zp, scale);
                    float bw  = deqnt_affine_to_f32(data[k + 2*total_det], zp, scale);
                    float bh  = deqnt_affine_to_f32(data[k + 3*total_det], zp, scale);
                    float score = deqnt_affine_to_f32(data[k + 4*total_det], zp, scale);
                    if (score < conf_threshold) continue;
                    cx *= (float)model_in_w; cy *= (float)model_in_h;
                    bw *= (float)model_in_w; bh *= (float)model_in_h;
                    float x1 = cx - bw * 0.5f;
                    float y1 = cy - bh * 0.5f;
                    float x2 = cx + bw * 0.5f;
                    float y2 = cy + bh * 0.5f;
                    if (x1 >= x2 || y1 >= y2) continue;
                    filterBoxes.push_back(x1);
                    filterBoxes.push_back(y1);
                    filterBoxes.push_back(x2 - x1);
                    filterBoxes.push_back(y2 - y1);
                    objProbs.push_back(score);
                    classId.push_back(0);
                    validCount++;
                }
            } else {
                float *data = (float *)_outputs[0].buf;
                int ch_stride = total_det;  // [1,5,8400] FP32: field c at data[k + c*N]
                for (int k = 0; k < ch_stride; k++) {
                    float score = data[k + 4*ch_stride];
                    if (score < conf_threshold) continue;
                    float cx = data[k + 0*ch_stride], cy = data[k + 1*ch_stride];
                    float bw = data[k + 2*ch_stride], bh = data[k + 3*ch_stride];
                    float x1 = cx - bw * 0.5f, y1 = cy - bh * 0.5f;
                    float x2 = cx + bw * 0.5f, y2 = cy + bh * 0.5f;
                    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
                    if (x2 > model_in_w) x2 = model_in_w;
                    if (y2 > model_in_h) y2 = model_in_h;
                    if (x1 >= x2 || y1 >= y2) continue;
                    filterBoxes.push_back(x1); filterBoxes.push_back(y1);
                    filterBoxes.push_back(x2 - x1); filterBoxes.push_back(y2 - y1);
                    objProbs.push_back(score); classId.push_back(0); validCount++;
                }
            }
        }
    } else if (io_num.n_output == 2) {
        // Split outputs: bbox [1,4,N] + score [1,1,N] (personDetect2-style).
        // Read N safely per n_dims: 3-D -> dims[2], 2-D -> dims[0].
        int total_det = -1;
        if (output_attrs[0].n_dims >= 3) total_det = (int)output_attrs[0].dims[2];
        else if (output_attrs[0].n_dims == 2) total_det = (int)output_attrs[0].dims[0];
        else if (output_attrs[0].n_dims == 1) total_det = (int)output_attrs[0].dims[0];
        if (total_det <= 0) total_det = 8400;  // last-resort fallback

        // Identify which output is bbox (ch=4) and which is score (ch=1);
        // export order may vary between models (v4: bbox first, v3x: score first).
        int ch0 = (output_attrs[0].n_dims >= 2) ? (int)output_attrs[0].dims[1] : -1;
        int ch1 = (output_attrs[1].n_dims >= 2) ? (int)output_attrs[1].dims[1] : -1;
        int bbox_idx = (ch0 == 4) ? 0 : 1;
        int score_idx = (ch1 == 4) ? 1 : 0;
        if (bbox_idx == score_idx) { bbox_idx = 0; score_idx = 1; }  // fallback
        float *bbox  = (float *)_outputs[bbox_idx].buf;
        float *score = (float *)_outputs[score_idx].buf;
        float eff_thres = conf_threshold;
        static bool once2 = false;
        if (!once2) {
            printf("Using 2-output decode path [%d,%d,%d] / [%d,%d,%d] total_det=%d\n",
                   output_attrs[0].n_dims >= 1 ? (int)output_attrs[0].dims[0] : -1,
                   output_attrs[0].n_dims >= 2 ? (int)output_attrs[0].dims[1] : -1,
                   output_attrs[0].n_dims >= 3 ? (int)output_attrs[0].dims[2] : -1,
                   output_attrs[1].n_dims >= 1 ? (int)output_attrs[1].dims[0] : -1,
                   output_attrs[1].n_dims >= 2 ? (int)output_attrs[1].dims[1] : -1,
                   output_attrs[1].n_dims >= 3 ? (int)output_attrs[1].dims[2] : -1,
                   total_det);
            once2 = true;
        }
        // Layout + field probes. Locked ONCE on a confident detection
        // (score >= 0.5): video-startup frames are noisy and locking on them
        // produces full-frame boxes. Frames before the lock emit no detections.
        // NOTE: the decided semantics must be static too — a local default
        // (use_xyxy=false) would decode every frame as cxcywh after locking.
        // force_xyxy=1 skips the field-order probe entirely (known model
        // semantics, e.g. yolov26s_v4_* which emits XYXY), BUT the data
        // layout (col_major) is a separate property: the RKNN converter may
        // emit bbox as [4,N] column-major or [N,4] row-major depending on the
        // ONNX graph structure. v3 (concat_xyxy + Identity) came out row-major
        // and reading it column-major produced full-frame garbage boxes
        // (y=0, w>640). Probe col_major here too, before any decode.
        static bool use_xyxy = false;
        static int col_major = 1;    // column-major [4,N]: field c at data[k + c*N]
        static bool probe_locked = false;
        if (force_xyxy) {
            // force_xyxy=1 仅表示"已知不是 personDetect2 老模型", 不代表
            // bbox 一定是 XYXY: v3 cxcywh 版输出 [cx,cy,bw,bh](纯 Slice 无
            // 算术链, NPU 不塌缩), 必须探测语义 + 布局。
            // Probe layout on the highest-score anchor: col-major reads
            // [k, k+N, k+2N, k+3N]; row-major reads [4k..4k+3]. The correct
            // one yields a plausible box (XYXY: x2>x1 in-frame; CXCYWH:
            // center in-frame with sane size) — probe BOTH semantics.
            // NOTE: probe once on the first frame only (probe_locked gates
            // the printout too, otherwise [layout] spam every frame).
            if (!probe_locked) {
                int k0 = 0;
                float s0 = score[0];
                for (int i = 1; i < total_det; i++)
                    if (score[i] > s0) { s0 = score[i]; k0 = i; }
                // 关键: 无人帧(所有 score≈0)时 k0=0 是垃圾数据, 探测会锁错
                // 语义/布局(BSL-R 首帧因此锁成 use_xyxy=1, 实际是 cxcywh,
                // 之后有人帧全部解码错误 → 检测不到)。必须有高分 anchor 才探测。
                if (s0 < 0.5f) {
                    return 0;  // 无置信检测, 跳过本帧, 下帧再试
                }
                // collect both reads
                float c0 = bbox[k0 + 0*total_det], c1 = bbox[k0 + 1*total_det],
                      c2 = bbox[k0 + 2*total_det], c3 = bbox[k0 + 3*total_det];
                float r0 = bbox[k0*4 + 0], r1 = bbox[k0*4 + 1],
                      r2 = bbox[k0*4 + 2], r3 = bbox[k0*4 + 3];
                auto plausible = [&](float a, float b, float c, float d) {
                    // XYXY: x2>x1, y2>y1, inside, not full-frame
                    bool xyxy = (c > a && d > b && a >= -1 && b >= -1 &&
                                 c <= model_in_w + 1 && d <= model_in_h + 1 &&
                                 (c - a) < 0.95f * model_in_w && (d - b) < 0.95f * model_in_h);
                    // CXCYWH: center inside, size 1..frame
                    bool cxcywh = (a > -1 && b > -1 && a <= model_in_w + 1 && b <= model_in_h + 1 &&
                                   c > 1 && d > 1 && c < model_in_w && d < model_in_h);
                    return xyxy || cxcywh;
                };
                bool col_ok = plausible(c0, c1, c2, c3);
                bool row_ok = plausible(r0, r1, r2, r3);
                if (row_ok && !col_ok) col_major = 0;
                // 语义判定: 用正确布局的 4 值, XYXY vs CXCYWH 计数
                float a0, a1, a2, a3;
                if (col_major) { a0 = c0; a1 = c1; a2 = c2; a3 = c3; }
                else           { a0 = r0; a1 = r1; a2 = r2; a3 = r3; }
                bool is_xyxy = (a2 > a0 && a3 > a1);
                use_xyxy = is_xyxy;
                printf("[layout] force_xyxy: k0=%d score=%.3f col_ok=%d row_ok=%d col_major=%d use_xyxy=%d "
                       "(col=[%.1f %.1f %.1f %.1f] row=[%.1f %.1f %.1f %.1f])\n",
                       k0, s0, (int)col_ok, (int)row_ok, col_major, (int)use_xyxy,
                       c0, c1, c2, c3, r0, r1, r2, r3);
            }
            probe_locked = true;
        } else if (!probe_locked) {
            int k0 = 0;
            float s0 = score[0];
            for (int i = 1; i < total_det; i++)
                if (score[i] > s0) { s0 = score[i]; k0 = i; }
            if (s0 < 0.5f) {
                return 0;  // no confident detection yet; skip this frame
            }
            float w_c = bbox[k0 + 2*total_det], h_c = bbox[k0 + 3*total_det];
            float w_r = bbox[k0*4 + 2],       h_r = bbox[k0*4 + 3];
            float cx_c = bbox[k0 + 0*total_det], cy_c = bbox[k0 + 1*total_det];
            float cx_r = bbox[k0*4 + 0],         cy_r = bbox[k0*4 + 1];
            bool col_ok = cx_c >= 0 && cx_c <= model_in_w && cy_c >= 0 && cy_c <= model_in_h &&
                          w_c > 1 && w_c <= model_in_w && h_c > 1 && h_c <= model_in_h;
            bool row_ok = cx_r >= 0 && cx_r <= model_in_w && cy_r >= 0 && cy_r <= model_in_h &&
                          w_r > 1 && w_r <= model_in_w && h_r > 1 && h_r <= model_in_h;
            if (row_ok && !col_ok) col_major = 0;
            // Field order (xyxy vs cxcywh): a single anchor is ambiguous
            // (e.g. a right-side xyxy box can look like cxcywh: y2 < x1),
            // so count how many confident anchors give a plausible box
            // under each hypothesis — the correct one yields far more.
            int n_xyxy = 0, n_cxc = 0;
            float a0 = 0, a1 = 0, a2 = 0, a3 = 0;  // top anchor, for printout
            for (int i = 0; i < total_det; i++) {
                float s = score[i];
                if (s < eff_thres) continue;
                float b0, b1, b2, b3;
                if (col_major) {
                    b0 = bbox[i + 0*total_det]; b1 = bbox[i + 1*total_det];
                    b2 = bbox[i + 2*total_det]; b3 = bbox[i + 3*total_det];
                } else {
                    b0 = bbox[i*4 + 0]; b1 = bbox[i*4 + 1];
                    b2 = bbox[i*4 + 2]; b3 = bbox[i*4 + 3];
                }
                // xyxy hypothesis: x2>x1, y2>y1, in frame, not full-frame
                if (b2 > b0 && b3 > b1 && b0 >= 0 && b1 >= 0 &&
                    b2 <= model_in_w && b3 <= model_in_h &&
                    (b2 - b0) < 0.95f * model_in_w && (b3 - b1) < 0.95f * model_in_h)
                    n_xyxy++;
                // cxcywh hypothesis: positive size, center inside frame
                if (b2 > 1 && b3 > 1 && b0 >= b2/2 && b0 <= model_in_w - b2/2 &&
                    b1 >= b3/2 && b1 <= model_in_h - b3/2)
                    n_cxc++;
                if (i == k0) { a0 = b0; a1 = b1; a2 = b2; a3 = b3; }
            }
            use_xyxy = (n_xyxy > n_cxc);
            (void)s0; (void)a0; (void)a1; (void)a2; (void)a3;  // probe result used above
            probe_locked = true;
        }
        // Sparse decode: pass 1 scans only the score row (cheap) and records
        // indices above threshold; pass 2 does the full bbox decode for those
        // few anchors only. Avoids touching the bbox buffer for ~99.9% of
        // anchors where score < conf.
        static std::vector<int> cand;  // reused across frames
        cand.clear();
        for (int k = 0; k < total_det; k++) {
            if (score[k] >= eff_thres) cand.push_back(k);
        }
        for (size_t ci = 0; ci < cand.size(); ci++) {
            int k = cand[ci];
            float s = score[k];
            float x1, y1, x2, y2;
            if (use_xyxy) {
                if (col_major) {
                    x1 = bbox[k + 0*total_det]; y1 = bbox[k + 1*total_det];
                    x2 = bbox[k + 2*total_det]; y2 = bbox[k + 3*total_det];
                } else {
                    x1 = bbox[k*4 + 0]; y1 = bbox[k*4 + 1];
                    x2 = bbox[k*4 + 2]; y2 = bbox[k*4 + 3];
                }
            } else {
                float cx, cy, bw, bh;
                if (col_major) {
                    cx = bbox[k + 0*total_det]; cy = bbox[k + 1*total_det];
                    bw = bbox[k + 2*total_det]; bh = bbox[k + 3*total_det];
                } else {
                    cx = bbox[k*4 + 0]; cy = bbox[k*4 + 1];
                    bw = bbox[k*4 + 2]; bh = bbox[k*4 + 3];
                }
                x1 = cx - bw*0.5f; y1 = cy - bh*0.5f;
                x2 = cx + bw*0.5f; y2 = cy + bh*0.5f;
            }
            if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
            if (x2 > model_in_w) x2 = model_in_w;
            if (y2 > model_in_h) y2 = model_in_h;
            if (x1 >= x2 || y1 >= y2) continue;
            filterBoxes.push_back(x1); filterBoxes.push_back(y1);
            filterBoxes.push_back(x2 - x1); filterBoxes.push_back(y2 - y1);
            objProbs.push_back(s); classId.push_back(0); validCount++;
        }
    } else {

// default 3 branch
#ifdef RKNPU1
    int dfl_len = app_ctx->output_attrs[0].dims[2] / 4;
#else
    int dfl_len = output_attrs[0].dims[1] /4;     //RKNPU2

#endif
    int output_per_branch = io_num.n_output / 3 ;     //尺度分支个数 
    for (int i = 0; i < output_per_branch; i++) 
    {
#if defined(RV1106_1103)
        dfl_len = app_ctx->output_attrs[0].dims[3] /4;
        void *score_sum = nullptr;
        int32_t score_sum_zp = 0;
        float score_sum_scale = 1.0;
        if (output_per_branch == 3) {
            score_sum = _outputs[i * output_per_branch + 2]->virt_addr;
            score_sum_zp = app_ctx->output_attrs[i * output_per_branch + 2].zp;
            score_sum_scale = app_ctx->output_attrs[i * output_per_branch + 2].scale;
        }
        int box_idx = i * output_per_branch;
        int score_idx = i * output_per_branch + 1;
        grid_h = app_ctx->output_attrs[box_idx].dims[1];
        grid_w = app_ctx->output_attrs[box_idx].dims[2];
        stride = model_in_h / grid_h;
        
        if (app_ctx->is_quant) {
            validCount += process_i8_rv1106((int8_t *)_outputs[box_idx]->virt_addr, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                (int8_t *)_outputs[score_idx]->virt_addr, app_ctx->output_attrs[score_idx].zp,
                                app_ctx->output_attrs[score_idx].scale, (int8_t *)score_sum, score_sum_zp, score_sum_scale,
                                grid_h, grid_w, stride, dfl_len, filterBoxes, objProbs, classId, conf_threshold);
        }
        else
        {
            printf("RV1106/1103 only support quantization mode\n", LABEL_NALE_TXT_PATH);
            return -1;
        }

#else
        void *score_sum = nullptr;
        int32_t score_sum_zp = 0;
        float score_sum_scale = 1.0;
        int nt_per_pair = 3;     //每个尺度分支的3个输出头。 1.回归系数 2.类别分数 3.总分数
        if (output_per_branch == 4){    //原始模型为3，p2模型则为4
            score_sum = _outputs[i * nt_per_pair + 2].buf;
            score_sum_zp = output_attrs[i * nt_per_pair + 2].zp;
            score_sum_scale = output_attrs[i * nt_per_pair + 2].scale;
        }
        int box_idx = i*nt_per_pair;
        int score_idx = i*nt_per_pair + 1;

#ifdef RKNPU1
        grid_h = app_ctx->output_attrs[box_idx].dims[1];
        grid_w = app_ctx->output_attrs[box_idx].dims[0];
#else
        grid_h = output_attrs[box_idx].dims[2];
        grid_w = output_attrs[box_idx].dims[3];
    
#endif
        stride = model_in_h / grid_h;

        if (is_quant)
        {
#ifdef RKNPU1
            validCount += process_u8((uint8_t *)_outputs[box_idx].buf, app_ctx->output_attrs[box_idx].zp, app_ctx->output_attrs[box_idx].scale,
                                     (uint8_t *)_outputs[score_idx].buf, app_ctx->output_attrs[score_idx].zp, app_ctx->output_attrs[score_idx].scale,
                                     (uint8_t *)score_sum, score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len,
                                     filterBoxes, objProbs, classId, conf_threshold);
#else
            validCount += process_i8((int8_t *)_outputs[box_idx].buf, output_attrs[box_idx].zp, output_attrs[box_idx].scale,
                                     (int8_t *)_outputs[score_idx].buf, output_attrs[score_idx].zp, output_attrs[score_idx].scale,
                                     (int8_t *)score_sum, score_sum_zp, score_sum_scale,
                                     grid_h, grid_w, stride, dfl_len, 
                                     filterBoxes, objProbs, classId, conf_threshold);
            
#endif
        }
        else
        {
            validCount += process_fp32((float *)_outputs[box_idx].buf, (float *)_outputs[score_idx].buf, (float *)score_sum,
                                       grid_h, grid_w, stride, dfl_len, 
                                       filterBoxes, objProbs, classId, conf_threshold);
        }
#endif
    }
    } // end else (multi-branch decode)

    // no object detect
    if (validCount <= 0)
    {
        return 0;
    }
    std::vector<int> indexArray;
    for (int i = 0; i < validCount; ++i)
    {
        indexArray.push_back(i);
    }
    quick_sort_indice_inverse(objProbs, 0, validCount - 1, indexArray);

    std::set<int> class_set(std::begin(classId), std::end(classId));

    for (auto c : class_set)
    {
        nms(validCount, filterBoxes, classId, indexArray, c, nms_threshold);
    }

    // Center-distance dedup: merge nearby detections of same class
    // fixes fragmented body-part detections on standing persons
    {
        const float dedup_dist = 80.0f;  // pixels — any two centers within this = same person
        for (int i = 0; i < validCount; ++i) {
            if (indexArray[i] == -1) continue;
            int ni = indexArray[i];
            float cxi = filterBoxes[ni*4+0] + filterBoxes[ni*4+2]*0.5f;
            float cyi = filterBoxes[ni*4+1] + filterBoxes[ni*4+3]*0.5f;
            for (int j = i+1; j < validCount; ++j) {
                if (indexArray[j] == -1) continue;
                int nj = indexArray[j];
                if (classId[ni] != classId[nj]) continue;
                float cxj = filterBoxes[nj*4+0] + filterBoxes[nj*4+2]*0.5f;
                float cyj = filterBoxes[nj*4+1] + filterBoxes[nj*4+3]*0.5f;
                float dx = cxi - cxj, dy = cyi - cyj;
                if (dx*dx + dy*dy < dedup_dist*dedup_dist) {
                    // keep the one with higher score
                    if (objProbs[ni] < objProbs[nj]) { indexArray[i] = -1; break; }
                    else { indexArray[j] = -1; }
                }
            }
        }
    }

    int last_count = 0;
    od_results->count = 0;

    /* box valid detect target */
    for (int i = 0; i < validCount; ++i)
    {
        if (indexArray[i] == -1 || last_count >= OBJ_NUMB_MAX_SIZE)
        {
            continue;
        }
        int n = indexArray[i];

        float x1 = filterBoxes[n * 4 + 0] - letter_box->w_pad_right;
        float y1 = filterBoxes[n * 4 + 1] - letter_box->h_pad_bottom;
        float x2 = x1 + filterBoxes[n * 4 + 2];
        float y2 = y1 + filterBoxes[n * 4 + 3];
        int id = classId[n];
        float obj_conf = objProbs[i];

        od_results->results[last_count].box.left = (int)(clamp(x1, 0, model_in_w) / letter_box->resize_scale_w);
        od_results->results[last_count].box.top = (int)(clamp(y1, 0, model_in_h) / letter_box->resize_scale_h);
        od_results->results[last_count].box.right = (int)(clamp(x2, 0, model_in_w) / letter_box->resize_scale_w);
        od_results->results[last_count].box.bottom = (int)(clamp(y2, 0, model_in_h) / letter_box->resize_scale_h);
        od_results->results[last_count].prop = obj_conf;
        od_results->results[last_count].class_index = id;
        last_count++;
    }
    od_results->count = last_count;
    return 0;
}

}



void deinitPostProcess()
{
  for (int i = 0; i < OBJ_CLASS_NUM; i++)
  {
    if (labels[i] != nullptr)
    {
      free(labels[i]);
      labels[i] = nullptr;
    }
  }
}