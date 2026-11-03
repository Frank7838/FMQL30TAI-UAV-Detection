// 常驻的NPU推理进程：模型只加载一次，随后按顺序处理图像。
// CPU先做颜色转换和补边，厂商推理图计算后，再由CPU解码并返回检测框。
// 与Python父进程通过本地socket传图像和结果，字段采用小端格式。
// 每帧完整回包后，worker才会把结果交给FPGA告警模块。

#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-xrt/dev/buyi_device.h>
#include <icraft-xrt/dev/zg330_device.h>
#include <icraft-backends/buyibackend/buyibackend.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <icraft-backends/hostbackend/cuda/device.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/hostbackend/utils.h>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include "postprocess_yolo11.hpp"
#include "icraft_utils.hpp"
#include "yaml-cpp/yaml.h"
#include "FusedStreamPicPre.hpp"
#include "LegacyFusedStreamPicPre.hpp"
#include "CenterPacketPicPre.hpp"
#include "DirectTensor.hpp"
#include "ProductGuard.hpp"
#include "RoiSubset.hpp"
#include <memory>

using namespace icraft::xrt;
using namespace icraft::xir;
using Clock = std::chrono::steady_clock;
constexpr uint32_t REQUEST_MAGIC = 0x46504741;
constexpr uint32_t RESULT_MAGIC = 0x50524c39; // M506; strict new protocol

// 流式socket可能短读；读满当前字段才解析，EINTR重试，EOF抛错。
// 所有请求都顺序处理，不依赖一次recv恰好等于一帧。
void receive_exact(int fd, void *data, size_t size) {
    auto *bytes = static_cast<char *>(data);
    while (size) {
        const auto count = recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("stream input closed or failed");
        bytes += count;
        size -= count;
    }
}

// 发送完整字段，MSG_NOSIGNAL让断开的父进程表现为异常而非异步SIGPIPE。
void send_exact(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    while (size) {
        const auto count = send(fd, bytes, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::runtime_error("stream output closed or failed");
        bytes += count;
        size -= count;
    }
}

// steady_clock只用于阶段耗时，返回毫秒；这些分段不能单独当作整机FPS。
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 用旧CPU实现作为字节和几何参考，检查缩放比、补边及像素完全一致。
// 此函数不执行NPU，不以两次不同模型输出代替输入一致性验证。
void compare_picture(const cv::Mat &body, std::pair<int, int> target) {
    FusedStreamPicPre current(body, target);
    LegacyFusedStreamPicPre reference(body, target);
    require(current.dst_img.size() == reference.dst_img.size() &&
                current.dst_img.type() == reference.dst_img.type() &&
                current.getPadInfo() == reference.getPadInfo() &&
                current.getResizedHW() == reference.getResizedHW() &&
                current.getResizedRatio() == reference.getResizedRatio(),
            "Preprocess geometry mismatch");
    require(cv::norm(current.dst_img, reference.dst_img, cv::NORM_INF) == 0,
            "Preprocess pixel mismatch");
}
// 正常入口参数是配置文件和继承socket fd；--cpu-preprocess-check是无SDK的离线自检。
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--cpu-preprocess-check") {
        cv::setNumThreads(1);
        for (int width : {17, 31, 37, 640}) {
            cv::Mat storage(480, width + 7, CV_8UC3);
            cv::randu(storage, 0, 256);
            auto body = storage(cv::Rect(0, 0, width, 480));
            compare_picture(body, {640, width});
        }
        for (auto size : {cv::Size(640, 480), cv::Size(640, 640), cv::Size(800, 600)}) {
            cv::Mat body(size, CV_8UC3);
            cv::randu(body, 0, 256);
            compare_picture(body, {640, 640});
        }
        std::cout << "CPU_PREPROCESS_BYTES_GEOMETRY_MATCH cases=7 SDK_open=0\n";
        return 0;
    }
    if (argc != 3) {
        std::cerr << "Usage: drone_yolo11_stream_arm64 <config.yaml> <socket-fd>\n";
        return 2;
    }
    try {
        cv::setNumThreads(1); // Avoid per-frame parallel color/resize scheduling on the small CPU.
        product_guard();
        const int fd = std::stoi(argv[2]);
        // 0表示持续；非0才检查前向上限。不能仅去掉Python的1800秒限制而遗留C++次数上限。
        const uint64_t max_forwards = std::stoull(std::getenv("PRODUCT_MAX_FORWARDS"));
        if (max_forwards > 2000000)
            throw std::runtime_error(
                "Invalid finite forward limit"); // zero explicitly means continuous
        auto config = YAML::LoadFile(argv[1]);
        roi_m705::Roi roi = roi_m705::DEFAULT;
        if (config["business_roi"]) {
            auto values = config["business_roi"].as<std::vector<int64_t>>();
            if (values.size() != 4)
                throw std::runtime_error("Four ROI coordinates required");
            for (int i = 0; i < 4; i++) {
                if (values[i] < 0 || values[i] > 65536)
                    throw std::runtime_error("Invalid ROI integer");
                roi[i] = uint32_t(values[i]);
            }
        }
        roi_m705::validate(roi); // Before opening any SDK device.
        auto model = config["imodel"];
        const auto backend = model["run_backend"].as<std::string>();
        if (backend != "zg330")
            throw std::runtime_error("stream worker requires real zg330 backend");
        auto stage = model["stage"].as<std::string>();
        // 按zg330后端加载同一份JSON/raw；不是在板端重新训练、量化或导出模型。
        auto paths = getJrPath(backend, model["dir"].as<std::string>(), stage);
        auto network = loadNetwork(paths.first, paths.second);
        NetInfo info(network);
        auto param = config["param"];
        const float confidence = param["conf"].as<float>();
        const float nms_iou = param["iou_thresh"].as<float>();
        bool multilabel = param["multilabel"].as<bool>();
        bool fpga_nms = param["fpga_nms"].as<bool>();
        const int classes = param["number_of_class"].as<int>();
        const int heads = param["number_of_head"].as<int>();
        auto anchors = param["anchors"].as<std::vector<std::vector<std::vector<float>>>>();
        auto labels = toVector(config["dataset"]["names"].as<std::string>());
        // 现用图包含厂商DetPost硬件处理，后面的CPU解码/NMS仍存在。
        // 这部分厂商PL能力与自研告警RTL分别说明，不能把整个推理IP算作自研。
        if (info.DetPost_on)
            updateDetpost(info, confidence);
        auto device = openDevice(backend, model["ip"].as<std::string>(), true, false);
        auto session = initSession(backend, network, device, model["ocm_option"].as<int>(), true,
                                   model["speedmode"].as<bool>(), model["compressFtmp"].as<bool>());
        session.enableTimeProfile(true);
        session.apply();
        std::vector<std::vector<float>> norms;
        std::vector<int> real_channels;
        auto strides = get_stride(info);
        if (info.DetPost_on) {
            real_channels = _getReal_out_channles({classes, 64}, info.detpost_bit, classes);
            auto scales = info.o_scale;
            norms = set_norm_by_head(heads, 2, scales);
        }
        bool show = false, save = false;
        std::string result_root, name;
        // PL decisions belong to the guarded parent after frame-age/validity checks.
        // 模型及SDK会话apply完成才发就绪标记。父进程从此开始获取新鲜相机帧，避免回放加载前积压。
        send_exact(fd, "RGB1", 4);
        std::cout << "NPU_STREAM_READY\n" << std::flush;
        CenterPacketStorage center_storage;
        size_t processed = 0;
        // 单请求串行执行，响应顺序为3个uint32、11个double、每框6个float，均小端。
        for (;;) {
            // 请求头16字节：[magic, 序号, 原宽, 原高]，小端uint32。
            // 普通请求随后是width×height×3 BGR；1920×1080紧凑请求只传640×360采样BGR。
            uint32_t request[4];
            receive_exact(fd, request, sizeof(request));
            if (request[0] != REQUEST_MAGIC && request[0] != CENTER_REQUEST_MAGIC)
                throw std::runtime_error("bad stream request magic");
            // 零宽/零高仅对普通magic表示正常结束；不会把一次坏图像当成退出后继续运行。
            if (request[0] == REQUEST_MAGIC && !request[2] && !request[3])
                break;
            if (max_forwards && processed >= max_forwards)
                throw std::runtime_error("Forward budget exhausted");
            const auto guard_begin = Clock::now();
            product_guard();
            const auto guard_end = Clock::now();
            const uint32_t width = request[2], height = request[3];
            if (!width || !height || width > 8192 || height > 8192 ||
                uint64_t(width) * height > 8388608)
                throw std::runtime_error("invalid or excessive frame size");
            const std::pair<int, int> target{info.i_cubic[0].h, info.i_cubic[0].w};
            const bool compact = validate_center_header(request, processed, target);
            cv::Mat body(compact ? CENTER_SMALL_HEIGHT : int(height),
                         compact ? CENTER_SMALL_WIDTH : int(width), CV_8UC3);
            receive_exact(fd, body.data, body.total() * body.elemSize());
            const auto begin = Clock::now();
            // Vendor PicPre has no virtual destructor. Own each concrete type;
            // never delete a derived preprocessor through a PicPre pointer.
            // 保持具体预处理对象活到后处理结束；PicPre没有虚析构，不能经基类指针delete。
            std::unique_ptr<CenterPacketPicPre> center_owner;
            std::unique_ptr<FusedStreamPicPre> raw_owner;
            PicPre *image_pointer = nullptr;
            if (compact) {
                if (show || save)
                    throw std::logic_error("center worker cannot visualize original colors");
                center_owner = std::make_unique<CenterPacketPicPre>(body, width, height, target,
                                                                    center_storage);
                image_pointer = center_owner.get();
            } else {
                raw_owner = std::make_unique<FusedStreamPicPre>(body, target);
                image_pointer = raw_owner.get();
            }
            PicPre &image = *image_pointer;
            const auto picture_end = Clock::now();
            // CPU RGB图复制一次到SDK拥有的Host Tensor，再由dmaInit交接设备。
            // 这是减少重复复制的路径，并非相机、PL、NPU之间的零拷贝通路。
            auto tensor = DirectUInt8Tensor(image.dst_img, network);
            // Development adoption gate: compare actual tensor bytes against the
            // previous helper on four real inputs, with only one NPU forward each.
            // 前四个真实输入逐字节比对Tensor与旧预处理；每个输入只执行一次forward。
            // 对四种输入比较新旧预处理结果。
            if (processed < 4) {
                auto reference = CvMat2Tensor(image.dst_img, network);
                if (!compact)
                    compare_picture(body, target);
                const auto bytes =
                    reference.dtype().numElements() * reference.dtype()->element_dtype.bits() / 8;
                require(tensor.dtype().numElements() == reference.dtype().numElements() &&
                            tensor.dtype()->element_dtype.bits() ==
                                reference.dtype()->element_dtype.bits(),
                        "Input tensor layout mismatch");
                require(std::memcmp(tensor.data().cptr(), reference.data().cptr(), bytes) == 0,
                        "Input tensor bytes mismatch");
                if (processed == 3)
                    std::cout << "INPUT_TENSOR_AND_PREPROCESS_BYTES_MATCH samples=4\n"
                              << std::flush;
            }
            const auto tensor_end = Clock::now();
            dmaInit(backend, info.ImageMake_on, tensor, device);
            const auto pre_end = Clock::now();
            // No business register writes in this inference-only child.
            // 这里只提交厂商NPU；推理完成、后处理结束后才向父进程回包。
            // 同步forward返回后才进入复位/后处理；父进程此时仍没有本帧完成回执。
            auto outputs = session.forward({tensor});
            const auto forward_end = Clock::now();
            // 沿用厂商SDK的推理传输复位。业务告警的reset单独同步，不由此清除。
            device.reset(1);
            const auto infer_end = Clock::now();
            std::vector<std::vector<float>> detections;
            if (info.DetPost_on) {
                detections =
                    post_detpost_hard(outputs, image, info, confidence, nms_iou, multilabel,
                                      fpga_nms, classes, anchors, labels, show, save, result_root,
                                      name, device, backend, norms, real_channels, strides, 64);
            } else {
                detections = post_detpost_soft(outputs, image, labels, anchors, info, classes,
                                               confidence, nms_iou, fpga_nms, device, backend,
                                               multilabel, show, save, result_root, name);
            }
            const auto post_end = Clock::now();
            // 300是通信协议的最大返回框数；父进程的PL原始框容量是16，超限会显式报错而非直接截断。
            if (detections.size() > 300)
                throw std::runtime_error("Response detection limit");
            // 回包头12字节：[RESULT_MAGIC, 原请求序号, 框数]。
            // 再发11个double：3段汇总耗时与8段细项；每框6个float为类别、x、y、宽、高、分数。
            uint32_t response[3] = {RESULT_MAGIC, request[1],
                                    static_cast<uint32_t>(detections.size())};
            const double times[11] = {
                milliseconds(begin, pre_end),          milliseconds(pre_end, infer_end),
                milliseconds(infer_end, post_end),     milliseconds(guard_begin, guard_end),
                milliseconds(guard_end, begin),        milliseconds(begin, picture_end),
                milliseconds(picture_end, tensor_end), milliseconds(tensor_end, pre_end),
                milliseconds(pre_end, forward_end),    milliseconds(forward_end, infer_end),
                milliseconds(infer_end, post_end)};
            send_exact(fd, response, sizeof(response));
            send_exact(fd, times, sizeof(times));
            for (const auto &row : detections) {
                if (row.size() != 6)
                    throw std::runtime_error("unexpected detection row shape");
                send_exact(fd, row.data(), 6 * sizeof(float));
            }
            // Raw detections returned; parent validates and commits to M512 separately.
            if (!processed)
                calctime_detail(backend, session);
            ++processed;
        }
        // 正常结束必须走SDK关闭；父进程确认退出码后才释放会话设备锁。
        // 只有正常退出路径显式关闭SDK并返回0；异常路径返回1，由父进程保留未知设备所有权。
        Device::Close(device);
        std::cout << "NPU_STREAM_COMPLETE frames=" << processed << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "NPU_STREAM_FAILED: " << error.what() << '\n';
        return 1;
    }
}
