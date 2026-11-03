#pragma once
#include <opencv2/opencv.hpp>
#include <icraft-xrt/core/tensor.h>
#include <icraft-xrt/dev/host_device.h>

#include "modelzoo_utils.hpp"

#include "yolo11_utils.hpp"
#include <cstring>
#include <stdexcept>

using namespace icraft::xrt;
struct Grid {
    uint16_t location_x = 0;
    uint16_t location_y = 0;
    uint16_t anchor_index = 0;
};


std::vector<float> get_stride(NetInfo& netinfo) {
    std::vector<float> stride;
    for (auto i : netinfo.o_cubic) {
        stride.emplace_back(netinfo.i_cubic[0].h / i.h);
    }
    return stride;
};

template <typename T>
Grid get_grid(int bits, T* tensor_data, int base_addr, int anchor_length) {
    Grid grid;
    uint16_t anchor_index;
    uint16_t location_y;
    uint16_t location_x;
    if (bits == 8)
    {
        anchor_index = (((uint16_t)tensor_data[base_addr + anchor_length - 1]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 2];
        location_y = (((uint16_t)tensor_data[base_addr + anchor_length - 3]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 4];
        location_x = (((uint16_t)tensor_data[base_addr + anchor_length - 5]) << 8) + (uint8_t)tensor_data[base_addr + anchor_length - 6];

    }
    else if (bits == 16)
    {
        anchor_index = (uint16_t)tensor_data[base_addr + anchor_length - 1];
        location_y = (uint16_t)tensor_data[base_addr + anchor_length - 2];
        location_x = (uint16_t)tensor_data[base_addr + anchor_length - 3];
    }
    grid.location_x = location_x;
    grid.location_y = location_y;
    grid.anchor_index = anchor_index;
    return grid;
}

template <typename T>
void post_process(std::vector<int> &id_list, std::vector<float> &socre_list, std::vector<cv::Rect2f> &box_list, T* tensor_data, int obj_ptr_start,
    Grid& grid, std::vector<int>& real_out_channles,int bbox_info_channel, std::vector<float> norm, int stride,
    std::vector<float> anchor,int NOC, float CONF,bool MULTILABEL) {
    if (!MULTILABEL) {
        //getMaxRealScore;
        T* class_ptr_start = tensor_data + obj_ptr_start;
        T* max_prob_ptr = std::max_element(class_ptr_start, class_ptr_start + NOC);
        int max_index = std::distance(class_ptr_start, max_prob_ptr);
        auto _prob_ = sigmoid(*max_prob_ptr * norm[0]);
        auto realscore =  _prob_;

        if (realscore > CONF) {
            //getBbox
            std::vector<float> ltrb = dfl(tensor_data,
                norm[1], obj_ptr_start + real_out_channles[0], bbox_info_channel);
            float x1 = grid.location_x + 0.5 - ltrb[0];
            float y1 = grid.location_y + 0.5 - ltrb[1];
            float x2 = grid.location_x + 0.5 + ltrb[2];
            float y2 = grid.location_y + 0.5 + ltrb[3];

            float x = ((x2 + x1) / 2.f) * stride;
            float y = ((y2 + y1) / 2.f) * stride;
            float w = (x2 - x1) * stride;
            float h = (y2 - y1) * stride;
            std::vector<float> xywh = { x,y,w,h };


            id_list.emplace_back(max_index);
            socre_list.emplace_back(realscore);
            box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
                (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
        }
    }
    else {
        for (size_t i = 0; i < NOC; i++) {
            //getRealScore
            auto _prob_ = sigmoid(tensor_data[obj_ptr_start + i] * norm[0]);
            auto realscore =_prob_;
            if (realscore > CONF) {
                //getBbox
                std::vector<float> ltrb = dfl(tensor_data,
                    norm[1], obj_ptr_start + real_out_channles[0], bbox_info_channel);
                float x1 = grid.location_x + 0.5 - ltrb[0];
                float y1 = grid.location_y + 0.5 - ltrb[1];
                float x2 = grid.location_x + 0.5 + ltrb[2];
                float y2 = grid.location_y + 0.5 + ltrb[3];

                float x = ((x2 + x1) / 2.f) * stride;
                float y = ((y2 + y1) / 2.f) * stride;
                float w = (x2 - x1) * stride;
                float h = (y2 - y1) * stride;
                std::vector<float> xywh = { x,y,w,h };


                id_list.emplace_back(i);
                socre_list.emplace_back(realscore);
                box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),
                    (xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
            }
        }
    }

}

std::vector<std::vector<float>> post_detpost_hard(const std::vector<Tensor>& output_tensors, PicPre& img, NetInfo& netinfo,
    float conf, float iou_thresh, bool MULTILABEL, bool fpga_nms, int N_CLASS, 
    std::vector<std::vector<std::vector<float>>> &ANCHORS,std::vector<std::string>& LABELS, 
    bool & show, bool & save , std::string &resRoot, std::string & name,icraft::xrt::Device device,const std::string& runBackend,std::vector<std::vector<float>> & _norm,std::vector<int> & real_out_channles,std::vector<float> & _stride,int bbox_info_channel) {
        std::vector<int> id_list;
        std::vector<float> socre_list;
        std::vector<cv::Rect2f> box_list;
		for (int head_index = 0; head_index < output_tensors.size(); head_index++) {
			auto host_tensor = output_tensors[head_index].to(HostDevice::MemRegion());
			int output_tensors_bits = output_tensors[head_index].dtype()->element_dtype.getStorageType().bits();
			int obj_num = output_tensors[head_index].dtype()->shape[2];
			int anchor_length = output_tensors[head_index].dtype()->shape[3];

			auto norm = _norm[head_index];
			auto stride = _stride[head_index];
			std::vector<float> _anchor_ = {};
			switch(output_tensors_bits){
				case 8:{
					auto tensor_data = (int8_t*)host_tensor.data().cptr();
					for(size_t obj = 0; obj < obj_num; obj++){
                        int base_addr = obj * anchor_length;
                        Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
						post_process(id_list, socre_list, box_list, (int8_t*)tensor_data, base_addr, grid, real_out_channles, bbox_info_channel, norm, stride, _anchor_, N_CLASS, conf, MULTILABEL);
					}
					break;
				}
				//case 16
                case 16:{
                    auto tensor_data = (int16_t*)host_tensor.data().cptr();
                    auto storage = output_tensors[head_index].dtype()->element_dtype.getStorageType();
                    const bool floating = storage.is<icraft::xir::FloatType>();
                    if (floating && !storage.isFP16() && !storage.isBF16())
                        throw std::runtime_error("Unsupported floating DetPost format");
                    std::vector<float> decoded(anchor_length);
                    for(size_t obj = 0; obj < obj_num; obj++){
                        int base_addr = obj * anchor_length;
                        Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
                        if (floating) {
                            // Final three words are integer grid metadata, even
                            // when the logits and DFL values are FP16 or BF16.
                            auto words = reinterpret_cast<const uint16_t*>(tensor_data + base_addr);
                            for (int k = 0; k < anchor_length - 3; ++k) {
                                if (storage.isBF16()) {
                                    uint32_t bits = uint32_t(words[k]) << 16;
                                    std::memcpy(&decoded[k], &bits, sizeof(float));
                                } else { decoded[k] = float(cv::float16_t::fromBits(words[k])); }
                            }
                            post_process(id_list, socre_list, box_list, decoded.data(), 0, grid, real_out_channles, bbox_info_channel, norm, stride, _anchor_, N_CLASS, conf, MULTILABEL);
                        } else {
                            post_process(id_list, socre_list, box_list, tensor_data, base_addr, grid, real_out_channles, bbox_info_channel, norm, stride, _anchor_, N_CLASS, conf, MULTILABEL);
                        }
					}
					break;
                }
				default: {
                	throw "wrong bits num!";
                	exit(EXIT_FAILURE);
            	}
			}
		}
		//后处理之NMS
        std::vector<std::tuple<int, float, cv::Rect2f>> nms_res;
        if (fpga_nms&& runBackend.compare("buyi") == 0) {
            nms_res = nms_hard(box_list, socre_list, id_list, iou_thresh, device);
        }
        else {
            nms_res = nms_soft(id_list, socre_list, box_list, iou_thresh);   

        }
		std::vector<std::vector<float>> output_res = coordTrans(nms_res, img);

        #ifdef _WIN32
        if (show) {
            visualize(output_res, img.ori_img, resRoot, name, LABELS);
        }
        #endif
        if (save) {
            saveRes(output_res, resRoot, name);
            #ifdef __linux__
                visualize(output_res, img.ori_img, resRoot, name, LABELS);
            #endif
        }
        return output_res;
}

std::vector<std::vector<float>> post_detpost_soft(const std::vector<Tensor>& output_tensors, PicPre& img, std::vector<std::string>& LABELS,
    std::vector<std::vector<std::vector<float>>>& ANCHORS, NetInfo& netinfo, int N_CLASS, float conf, float iou_thresh,bool& fpga_nms, icraft::xrt::Device device, const std::string& runBackend,
    bool& MULTILABEL,bool& show, bool& save, std::string& resRoot, std::string& name) {

    std::vector<int> id_list;
    std::vector<float> socre_list;
    std::vector<cv::Rect2f> box_list;
    std::vector<float> stride = get_stride(netinfo);
     for (int yolo = 0; yolo < output_tensors.size()-1; yolo = yolo + 2) {
        int _H = output_tensors[yolo].dtype()->shape[1];
        int _W = output_tensors[yolo].dtype()->shape[2];
        auto host_tensor_class = output_tensors[yolo+0].to(HostDevice::MemRegion());
        auto host_tensor_box = output_tensors[yolo+1].to(HostDevice::MemRegion());
        auto tensor_data_class = (float*)host_tensor_class.data().cptr();
        auto tensor_data_box = (float*)host_tensor_box.data().cptr();
         for (size_t h = 0; h < _H; h++) {
            for (size_t w = 0; w < _W; w++) {
                auto one_head_stride = stride[yolo];
                auto classPtr = tensor_data_class + h * _W * N_CLASS + w * N_CLASS;
                auto boxPtr = tensor_data_box + h * _W * 64 + w * 64;
                if (!MULTILABEL) {
                    float* max_prob_ptr = std::max_element(classPtr, classPtr + N_CLASS);
                    int max_index = std::distance(classPtr, max_prob_ptr);
                    auto _prob_ = sigmoid(*max_prob_ptr);
                    auto realscore = _prob_;
                    
                    if (realscore > conf) {
                        //getBbox
                        std::vector<float> ltrb = dfl(boxPtr,1.0, 0, 64);
                        float x1 = w + 0.5 - ltrb[0];
                        float y1 = h + 0.5 - ltrb[1];
                        float x2 = w + 0.5 + ltrb[2];
                        float y2 = h + 0.5 + ltrb[3];

                        float x_ = ((x2 + x1) / 2.f) * one_head_stride;
                        float y_ = ((y2 + y1) / 2.f) * one_head_stride;
                        float w_ = (x2 - x1) * one_head_stride;
                        float h_ = (y2 - y1) * one_head_stride;
                        std::vector<float> xywh = { x_,y_,w_,h_ };
                        id_list.emplace_back(max_index);
                        socre_list.emplace_back(realscore);
                        box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),(xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
                        
                    }
                }
                else{
                    for (size_t cls_idx = 0; cls_idx < N_CLASS; cls_idx++) {
                        auto _prob_ = sigmoid(*(classPtr + cls_idx));
                        auto realscore =  _prob_;
                        if (realscore > conf) {
                            
                            //getBbox
                            std::vector<float> ltrb = dfl(boxPtr,1.0, 0, 64);
                            float x1 = w + 0.5 - ltrb[0];
                            float y1 = h + 0.5 - ltrb[1];
                            float x2 = w + 0.5 + ltrb[2];
                            float y2 = h + 0.5 + ltrb[3];

                            float x_ = ((x2 + x1) / 2.f) * one_head_stride;
                            float y_ = ((y2 + y1) / 2.f) * one_head_stride;
                            float w_ = (x2 - x1) * one_head_stride;
                            float h_ = (y2 - y1) * one_head_stride;
                            std::vector<float> xywh = { x_,y_,w_,h_ };

                            id_list.emplace_back(cls_idx);
                            socre_list.emplace_back(realscore);
                            box_list.emplace_back(cv::Rect2f((xywh[0] - xywh[2] / 2),(xywh[1] - xywh[3] / 2), xywh[2], xywh[3]));
                            
                        }
                    }
                }
            }
        }
    }
    // std::cout << "number of results before nms = " << id_list.size() << '\n';
    //后处理之NMS
    std::vector<std::tuple<int, float, cv::Rect2f>> nms_res;
    //auto post_start = std::chrono::system_clock::now();
    
    if (fpga_nms&& runBackend.compare("buyi") == 0) {
        nms_res = nms_hard(box_list, socre_list, id_list, iou_thresh, device);
    }
    else {
        nms_res = nms_soft(id_list, socre_list, box_list, iou_thresh);   
    }
    std::vector<std::vector<float>> output_res = coordTrans(nms_res, img);
    #ifdef _WIN32
    if (show) {
        visualize(output_res, img.ori_img, resRoot, name, LABELS);
    }
    #endif
    if (save) {
        saveRes(output_res, resRoot, name);
        #ifdef __linux__
            visualize(output_res, img.ori_img, resRoot, name, LABELS);
        #endif
    }
    return output_res;
}

// smooth
const float ALPHA = 0.5f;
const float SMOOTH_IOU = 0.80f;

using YoloPostResult = std::tuple<std::vector<int>, std::vector<float>, std::vector<cv::Rect2f>  >; // box_list, id_list, score_list

YoloPostResult post_detpost_plin(const std::vector<Tensor>& output_tensors, YoloPostResult& last_frame_result,NetInfo& netinfo,
    float conf, float iou_thresh, bool MULTILABEL, bool fpga_nms, int N_CLASS,
    std::vector<std::vector<std::vector<float>>>& ANCHORS, icraft::xrt::Device device,
    const std::string& runBackend,std::vector<std::vector<float>> & _norm,std::vector<int> & real_out_channles,std::vector<float> & _stride,int bbox_info_channel) {

    std::vector<int> id_list;
    std::vector<float> socre_list;
    std::vector<cv::Rect2f> box_list;
    std::vector<float> stride = get_stride(netinfo);
    for (size_t i = 0; i < output_tensors.size(); i++) {

        auto host_tensor = output_tensors[i].to(HostDevice::MemRegion());
        int output_tensors_bits = output_tensors[i].dtype()->element_dtype.getStorageType().bits();
        int obj_num = output_tensors[i].dtype()->shape[2];
        int anchor_length = output_tensors[i].dtype()->shape[3];

        auto norm = _norm[i];
		auto stride = _stride[i];
		std::vector<float> _anchor_ = {};
        if (output_tensors_bits == 16) {
            auto tensor_data = (int16_t*)host_tensor.data().cptr();
            for (size_t obj = 0; obj < obj_num; obj++) {
                int base_addr = obj * anchor_length;
                Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
                post_process(id_list, socre_list, box_list, (int16_t*)tensor_data, base_addr, grid, real_out_channles, bbox_info_channel, norm, stride, _anchor_, N_CLASS, conf, MULTILABEL);
            }
        }
        else {
            auto tensor_data = (int8_t*)host_tensor.data().cptr();
            for (size_t obj = 0; obj < obj_num; obj++) {
                int base_addr = obj * anchor_length;
                Grid grid = get_grid(output_tensors_bits, tensor_data, base_addr, anchor_length);
                post_process(id_list, socre_list, box_list, (int8_t*)tensor_data, base_addr, grid, real_out_channles, bbox_info_channel, norm, stride, _anchor_, N_CLASS, conf, MULTILABEL);
            }
        }
    }
    std::vector<std::tuple<int, float, cv::Rect2f>> nms_res;
    if (fpga_nms) {
        nms_res = nms_hard(box_list, socre_list, id_list, iou_thresh, device);
    }
    else {
        nms_res = nms_soft(id_list, socre_list, box_list, iou_thresh);   // 后处理 之 NMS

    }


    // // 对前后帧的结果求平均
    auto id_list_last_frame = std::get<0>(last_frame_result);
    auto score_list_last_frame = std::get<1>(last_frame_result);
    auto box_list_last_frame = std::get<2>(last_frame_result);

    for (auto  idx_score_bbox : nms_res) {
        for (size_t i = 0; i < box_list_last_frame.size(); ++i) {
            if ((1.f - cv::jaccardDistance(std::get<2>(idx_score_bbox), box_list_last_frame[i])) > SMOOTH_IOU && (std::get<0>(idx_score_bbox) == id_list_last_frame[i])) {

                std::get<2>(idx_score_bbox).x = box_list_last_frame[i].x * ALPHA + std::get<2>(idx_score_bbox).x * (1.0f - ALPHA);
                std::get<2>(idx_score_bbox).y = box_list_last_frame[i].y * ALPHA + std::get<2>(idx_score_bbox).y * (1.0f - ALPHA);
                std::get<2>(idx_score_bbox).width = box_list_last_frame[i].width * ALPHA + std::get<2>(idx_score_bbox).width * (1.0f - ALPHA);
                std::get<2>(idx_score_bbox).height = box_list_last_frame[i].height * ALPHA + std::get<2>(idx_score_bbox).height * (1.0f - ALPHA);
                std::get<1>(idx_score_bbox) = score_list_last_frame[i] * ALPHA + std::get<1>(idx_score_bbox) * (1.0f - ALPHA);
                break;
            }
        }
    }
    std::vector<int> id_list_ret;   //id_list_ret.reserve(nms_indices.size());
    std::vector<float> score_list_ret; //score_list_ret.reserve(nms_indices.size());
    std::vector<cv::Rect2f> box_list_ret; //box_list_ret.reserve(nms_indices.size());
    for (auto  idx_score_bbox : nms_res) {
        // store 
        id_list_ret.emplace_back(std::get<0>(idx_score_bbox));
        score_list_ret.emplace_back(std::get<1>(idx_score_bbox));
        box_list_ret.emplace_back(std::get<2>(idx_score_bbox));
    }
    return YoloPostResult { id_list_ret, score_list_ret, box_list_ret};

}
