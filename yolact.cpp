#include <fstream>
#include <iostream>
#include <sstream>
#include <numeric>
#include <chrono>
#include <vector>
#include <dirent.h>
#include "NvInfer.h"
#include "cuda_runtime_api.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include "logging.h"


#define CHECK(status) \
    do\
    {\
        auto ret = (status);\
        if (ret != 0)\
        {\
            std::cerr << "Cuda failure: " << ret << std::endl;\
            abort();\
        }\
    } while (0)

#define DEVICE 0  // GPU id


using namespace nvinfer1;

// stuff we know about the network and the input blobs size
static const int INPUT_W = 550;
static const int INPUT_H = 550;


static Logger gLogger;

struct Object
{
    cv::Rect_<float> rect;
    int label;
    float prob;
    std::vector<float> maskdata;
    cv::Mat mask;
};

cv::Mat static_resize(cv::Mat& img, bool keep) {
    // int channel = 3;
    int input_w = INPUT_W;
    int input_h = INPUT_H;
    cv::Mat cropped;
    if (keep)
    {    
        float scale = cv::min(float(input_w)/img.cols, float(input_h)/img.rows);
        auto scaleSize = cv::Size(img.cols * scale, img.rows * scale);

        cv::Mat resized;
        cv::resize(img, resized, scaleSize,0,0);

        cropped = cv::Mat::zeros(input_h, input_w, CV_8UC3);
        cv::Rect rect((input_w - scaleSize.width)/2, (input_h-scaleSize.height)/2, scaleSize.width, scaleSize.height);
        resized.copyTo(cropped(rect));
    }
    else
    {
        auto scaleSize = cv::Size(input_w, input_h);
        cv::resize(img, cropped, scaleSize,0,0);
    }
    

    return cropped;
}

static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& objects, int left, int right)
{
    int i = left;
    int j = right;
    float p = objects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (objects[i].prob > p)
            i++;

        while (objects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(objects[i], objects[j]);

            i++;
            j--;
        }
    }

    // #pragma omp parallel sections
    {
        // #pragma omp section
        {
            if (left < j) qsort_descent_inplace(objects, left, j);
        }
        // #pragma omp section
        {
            if (i < right) qsort_descent_inplace(objects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& objects)
{
    if (objects.empty())
        return;

    qsort_descent_inplace(objects, 0, objects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& objects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = objects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = objects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = objects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = objects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            //             float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

static void draw_objects(const cv::Mat& bgr, const std::vector<Object>& objects)
{
    static const unsigned char colors[19][3] =
    {
        {244, 67, 54},
        {233, 30, 99},
        {156, 39, 176},
        {103, 58, 183},
        {63, 81, 181},
        {33, 150, 243},
        {3, 169, 244},
        {0, 188, 212},
        {0, 150, 136},
        {76, 175, 80},
        {139, 195, 74},
        {205, 220, 57},
        {255, 235, 59},
        {255, 193, 7},
        {255, 152, 0},
        {255, 87, 34},
        {121, 85, 72},
        {158, 158, 158},
        {96, 125, 139}
    };
    static const char* class_names[] = { "background",
            "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
            "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
            "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
            "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
            "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
            "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
            "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
            "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
            "hair drier", "toothbrush"
        };

    cv::Mat image = bgr.clone();

    int color_index = 0;

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object& obj = objects[i];

        if (obj.prob < 0.15)
            continue;

        fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
                obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        const unsigned char* color = colors[color_index++];
        
        cv::rectangle(image, obj.rect, cv::Scalar(color[0], color[1], color[2]));

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);

        int x = obj.rect.x;
        int y = obj.rect.y - label_size.height - baseLine;
        if (y < 0)
            y = 0;
        if (x + label_size.width > image.cols)
            x = image.cols - label_size.width;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                      cv::Scalar(255, 255, 255), -1);

        cv::putText(image, text, cv::Point(x, y + label_size.height),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));

        // draw mask
        for (int y = 0; y < image.rows; y++)
        {
            const uchar* mp = obj.mask.ptr(y);
            uchar* p = image.ptr(y);
            for (int x = 0; x < image.cols; x++)
            {
                if (mp[x] == 255)
                {
                    p[0] = cv::saturate_cast<uchar>(p[0] * 0.5 + color[0] * 0.5);
                    p[1] = cv::saturate_cast<uchar>(p[1] * 0.5 + color[1] * 0.5);
                    p[2] = cv::saturate_cast<uchar>(p[2] * 0.5 + color[2] * 0.5);
                }
                p += 3;
            }
        }
    }

    cv::imwrite("result.png", image);
    // cv::imshow("image", image);
    // cv::waitKey(0);
}

const float mean_vals[3] = {123.68f / 255.f, 116.78f / 255.f, 103.94f / 255.f};
const float norm_vals[3] = {58.40f / 255.f, 57.12f / 255.f, 57.38f / 255.f};
float* blobFromImage(cv::Mat& img)
{
    float* blob = new float[img.total()*3];
    int channels = 3;
    int img_h = INPUT_H;
    int img_w = INPUT_W;

    for (int c = 0; c < channels; c++) 
    {
        for (int  h = 0; h < img_h; h++) 
        {
            for (int w = 0; w < img_w; w++) 
            {
                blob[c * img_w * img_h + h * img_w + w] =
                    (((float)img.at<cv::Vec3b>(h, w)[c]) / 255.0f - mean_vals[c]) / norm_vals[c];
            }
        }
    }
    return blob;
}


int main(int argc, char** argv) {
    cudaSetDevice(DEVICE);
    // create a model using the API directly and serialize it to a stream
    char *trtModelStream{nullptr};
    size_t size{0};

    if (argc == 4 && std::string(argv[2]) == "-i") {
        const std::string engine_file_path {argv[1]};
        std::ifstream file(engine_file_path, std::ios::binary);
        if (file.good()) {
            file.seekg(0, file.end);
            size = file.tellg();
            file.seekg(0, file.beg);
            trtModelStream = new char[size];
            assert(trtModelStream);
            file.read(trtModelStream, size);
            file.close();
        }
    } else {
        std::cerr << "arguments not right!" << std::endl;
        std::cerr << "Then use the following command:" << std::endl;
        std::cerr << "./yolact ../../yolact/checkpoint/yolact_fp32.engine -i ../dog.jpg  // deserialize file and run inference" << std::endl;
        return -1;
    }

    const std::string input_image_path {argv[3]};

    IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
    ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream, size);
    assert(engine != nullptr); 
    IExecutionContext* context = engine->createExecutionContext();
    assert(context != nullptr);
    delete[] trtModelStream;

    cudaStream_t stream;
    CHECK(cudaStreamCreate(&stream));


    cv::Mat bgr = cv::imread(input_image_path, 1); // bgr
    int img_w = bgr.cols;
    int img_h = bgr.rows;

    cv::Mat pr_img = static_resize(bgr, false);
    cv::Mat pr_img_rgb;
    cv::cvtColor(pr_img, pr_img_rgb, cv::COLOR_BGR2RGB);
    float* input_raw_data = blobFromImage(pr_img_rgb);

    // GPU pointer
    int nbBindings = 5;
    float* buffers[nbBindings];

    const int batch_size = 1;
    const int i_size = 1 * 3* 550*550;
    const int loc_size = 1 * 19248*4;
    const int mask_size = 1 * 19248*32;
    const int cof_size = 1 * 19248*81;
    const int mask_map_size = 1 * 138* 138*32;

    int num_class = 81;
    int num_priors = 19248;

    const int i_idx = engine->getBindingIndex("input.1"); //1x3x550x550
    const int loc_idx= engine->getBindingIndex("766"); // 1x19248x4
    const int mask_idx = engine->getBindingIndex("768"); //1x19248x32
    const int cof_idx= engine->getBindingIndex("770"); // 1x19248x81
    const int mask_map_idx = engine->getBindingIndex("588"); // 1x32x138x138

    std::chrono::steady_clock::time_point Tbegin, Iend, Tend;

    // Create CPU buffers
    static float* h_input=nullptr, *loca=nullptr, *mask=nullptr, *cof=nullptr, *mmp=nullptr;

    Tbegin = std::chrono::steady_clock::now();

    CHECK(cudaMallocHost((void**)&h_input, i_size*sizeof(float)));
    memcpy(h_input, input_raw_data, i_size*sizeof(float));
    CHECK(cudaMallocHost((void**)&loca, loc_size*sizeof(float)));
    CHECK(cudaMallocHost((void**)&mask, mask_size*sizeof(float)));
    CHECK(cudaMallocHost((void**)&cof, cof_size*sizeof(float)));
    CHECK(cudaMallocHost((void**)&mmp, mask_map_size*sizeof(float)));

    CHECK(cudaMalloc((void**)&buffers[i_idx], i_size*sizeof(float)));
    CHECK(cudaMemcpyAsync(buffers[i_idx], h_input, i_size*sizeof(float), cudaMemcpyHostToDevice, stream));

    // cuda result
    CHECK(cudaMalloc((void**)&buffers[loc_idx], loc_size*sizeof(float)));
    CHECK(cudaMalloc((void**)&buffers[mask_idx], mask_size*sizeof(float)));
    CHECK(cudaMalloc((void**)&buffers[cof_idx], cof_size*sizeof(float)));
    CHECK(cudaMalloc((void**)&buffers[mask_map_idx], mask_map_size*sizeof(float)));
printf(">>>>>>>>>>>>>>\n");
    context->enqueue(batch_size, (void**)buffers, stream, nullptr);
printf(">>>>>>>>>>>>>>\n");

    CHECK(cudaMemcpyAsync(loca, buffers[loc_idx], loc_size*sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(mask, buffers[mask_idx], mask_size*sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(cof, buffers[cof_idx], cof_size*sizeof(float), cudaMemcpyDeviceToHost, stream));
    CHECK(cudaMemcpyAsync(mmp, buffers[mask_map_idx], mask_map_size*sizeof(float), cudaMemcpyDeviceToHost, stream));

    cudaStreamSynchronize(stream);
    Iend = std::chrono::steady_clock::now();
    float infer_time = std::chrono::duration_cast <std::chrono::milliseconds> (Iend - Tbegin).count();

    std::cout << "time : " << infer_time/1000.0 << " Sec" << std::endl;

    std::vector<Object> objects;

    // make priorbox
    float priorbox[4*num_priors];
    {
        const int conv_ws[5] = {69, 35, 18, 9, 5};
        const int conv_hs[5] = {69, 35, 18, 9, 5};

        const float aspect_ratios[3] = {1.f, 0.5f, 2.f};
        const float scales[5] = {24.f, 48.f, 96.f, 192.f, 384.f};

        float* pb = priorbox;

        for (int p = 0; p < 5; p++)
        {
            int conv_w = conv_ws[p];
            int conv_h = conv_hs[p];

            float scale = scales[p];

            for (int i = 0; i < conv_h; i++)
            {
                for (int j = 0; j < conv_w; j++)
                {
                    // +0.5 because priors are in center-size notation
                    float cx = (j + 0.5f) / conv_w;
                    float cy = (i + 0.5f) / conv_h;

                    for (int k = 0; k < 3; k++)
                    {
                        float ar = aspect_ratios[k];

                        ar = sqrt(ar);

                        float w = scale * ar / 550;
                        float h = scale / ar / 550;

                        // This is for backward compatability with a bug where I made everything square by accident
                        // cfg.backbone.use_square_anchors:
                        h = w;

                        pb[0] = cx;
                        pb[1] = cy;
                        pb[2] = w;
                        pb[3] = h;

                        pb += 4;
                    }
                }
            }
        }
    }

    const float confidence_thresh = 0.05f;
    const float nms_threshold = 0.5f;
    const int keep_top_k = 200;

    std::vector<std::vector<Object>> class_candidates;
    class_candidates.resize(num_class);

    for (int i = 0; i < num_priors; i++)
    {
        float* conf = cof + 81*i;
        float* loc = loca + 4*i;
        float* pb = priorbox + 4*i;
        float* maskdata = mask + 32*i;

        // find class id with highest score
        // start from 1 to skip background
        int label = 0;
        float score = 0.f;
        for (int j = 1; j < num_class; j++)
        {
            float class_score = conf[j];
            if (class_score > score)
            {
                label = j;
                score = class_score;
            }
        }

        // ignore background or low score
        if (label == 0 || score <= confidence_thresh)
            continue;

        // CENTER_SIZE
        float var[4] = {0.1f, 0.1f, 0.2f, 0.2f};

        float pb_cx = pb[0];
        float pb_cy = pb[1];
        float pb_w = pb[2];
        float pb_h = pb[3];

        float bbox_cx = var[0] * loc[0] * pb_w + pb_cx;
        float bbox_cy = var[1] * loc[1] * pb_h + pb_cy;
        float bbox_w = (float)(exp(var[2] * loc[2]) * pb_w);
        float bbox_h = (float)(exp(var[3] * loc[3]) * pb_h);

        float obj_x1 = bbox_cx - bbox_w * 0.5f;
        float obj_y1 = bbox_cy - bbox_h * 0.5f;
        float obj_x2 = bbox_cx + bbox_w * 0.5f;
        float obj_y2 = bbox_cy + bbox_h * 0.5f;

        // clip
        obj_x1 = std::max(std::min(obj_x1 * bgr.cols, (float)(bgr.cols - 1)), 0.f);
        obj_y1 = std::max(std::min(obj_y1 * bgr.rows, (float)(bgr.rows - 1)), 0.f);
        obj_x2 = std::max(std::min(obj_x2 * bgr.cols, (float)(bgr.cols - 1)), 0.f);
        obj_y2 = std::max(std::min(obj_y2 * bgr.rows, (float)(bgr.rows - 1)), 0.f);

        // append object
        Object obj;
        obj.rect = cv::Rect_<float>(obj_x1, obj_y1, obj_x2 - obj_x1 + 1, obj_y2 - obj_y1 + 1);
        obj.label = label;
        obj.prob = score;
        obj.maskdata = std::vector<float>(maskdata, maskdata + 32);

        class_candidates[label].push_back(obj);
    }

    objects.clear();
    for (int i = 0; i < (int)class_candidates.size(); i++)
    {
        std::vector<Object>& candidates = class_candidates[i];

        qsort_descent_inplace(candidates);

        std::vector<int> picked;
        nms_sorted_bboxes(candidates, picked, nms_threshold);

        for (int j = 0; j < (int)picked.size(); j++)
        {
            int z = picked[j];
            objects.push_back(candidates[z]);
        }
    }

    qsort_descent_inplace(objects);

    // keep_top_k
    if (keep_top_k < (int)objects.size())
    {
        objects.resize(keep_top_k);
    }

    // generate mask
    for (unsigned int i = 0; i < objects.size(); i++)
    {
        Object& obj = objects[i];

        cv::Mat mask(138, 138, CV_32FC1);
        {
            mask = cv::Scalar(0.f);

            for (int p = 0; p < 32; p++)
            {
                const float* maskmap = mmp + p*138*138;
                float coeff = obj.maskdata[p];
                float* mp = (float*)mask.data;

                // mask += m * coeff
                for (int j = 0; j < 138 * 138; j++)
                {
                    mp[j] += maskmap[j] * coeff;
                }
            }
        }

        cv::Mat mask2;
        cv::resize(mask, mask2, cv::Size(img_w, img_h));

        // crop obj box and binarize
        obj.mask = cv::Mat(img_h, img_w, CV_8UC1);
        {
            obj.mask = cv::Scalar(0);

            for (int y = 0; y < img_h; y++)
            {
                if (y < obj.rect.y || y > obj.rect.y + obj.rect.height)
                    continue;

                const float* mp2 = mask2.ptr<const float>(y);
                uchar* bmp = obj.mask.ptr<uchar>(y);

                for (int x = 0; x < img_w; x++)
                {
                    if (x < obj.rect.x || x > obj.rect.x + obj.rect.width)
                        continue;

                    bmp[x] = mp2[x] > 0.5f ? 255 : 0;
                }
            }
        }
    }

    Tend = std::chrono::steady_clock::now();
    float f = std::chrono::duration_cast <std::chrono::milliseconds> (Tend - Tbegin).count();

    std::cout << "time : " << f/1000.0 << " Sec" << std::endl;

    draw_objects(bgr, objects);

    cudaStreamDestroy(stream);
    cudaFreeHost(context);
    cudaFreeHost(engine);
    cudaFreeHost(runtime);

    CHECK(cudaFree(buffers[i_idx]));
    CHECK(cudaFree(buffers[loc_idx]));
    CHECK(cudaFree(buffers[mask_idx]));
    CHECK(cudaFree(buffers[cof_idx]));
    CHECK(cudaFree(buffers[mask_map_idx]));

    return 0;
}
