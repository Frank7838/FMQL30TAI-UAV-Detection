#pragma once

#include <icraft-xir/core/network.h>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <unordered_set>
#include <spdlog/spdlog.h>
#include <stdexcept>

struct Cubic
{
    int h = 0;
    int w = 0;
    int c = 0;
};

class NetInfo
{
public:
    icraft::xir::Network network;
    std::vector<std::vector<int>> i_shape;
    std::vector<std::vector<int>> o_shape;
    std::vector<std::vector<int>> head_hardop_i_shape;
    std::vector<float> o_scale;
    std::unordered_set<std::string> fpga_op;
    std::vector<Cubic> i_cubic;
    std::vector<Cubic> o_cubic;
    std::vector<Cubic> head_hardop_i_shape_cubic;
    int inp_shape_opid = 0;
    std::vector<int> Imk_op_indices;
    int Imk_op_id = -1;
    int WarpAffine_op_idx = -1;
    int First_cast_op_idx = -1;
    int bit = 8;
    int detpost_bit = 8;
    bool resize_on = false;
    bool swaporder_on = false;
    bool ImageMake_on = false;
    bool DetPost_on = false;
    bool WarpAffine_on = false;
    std::vector<icraft::xir::Operation> ImageMakes_;
    icraft::xir::Operation DetPost_;
    icraft::xir::Operation WarpAffine_;
    bool mmu = true;
    float thr_f;
    std::vector<int64_t> data_thr;
    NetInfo() = default;

    NetInfo(const icraft::xir::Network &network)
    {
        this->network = network;
        this->fpga_op = fpgaOPlist(this->network);
        if (network.getTag("speedmode").has_value())
        {
            auto _speedmode = icraft::xir::Downcast<icraft::xir::Bool>(network.getTag("speedmode").value())->value;
            auto _compressFtmp = icraft::xir::Downcast<icraft::xir::Bool>(network.getTag("compressFtmp").value())->value;
            this->mmu = _speedmode || _compressFtmp;
        }
        auto oplist = network->ops;

        for (auto i : oplist[this->inp_shape_opid]->outputs)
        {
            this->i_shape.push_back(i.tensorType()->shape);
        }
        // 选择距离第一个hardop最近的
        if (this->WarpAffine_on)
        {
            for (auto i : oplist[this->WarpAffine_op_idx]->outputs)
            {
                this->head_hardop_i_shape.push_back(i.tensorType()->shape);
            }
        }
        else if (this->ImageMake_on)
        {
            for (auto imk_op_id : this->Imk_op_indices)
            {
                for (auto i : oplist[imk_op_id]->outputs)
                {
                    this->head_hardop_i_shape.push_back(i.tensorType()->shape);
                }
            }
        }
        else if (this->First_cast_op_idx != -1)
        {
            for (auto i : oplist[this->First_cast_op_idx]->outputs)
            {
                this->head_hardop_i_shape.push_back(i.tensorType()->shape);
            }
        }

        for (auto i : oplist[-1]->inputs)
        {
            this->o_shape.push_back(i.tensorType()->shape);
        }
        if (this->DetPost_on)
        {
            this->detpost_bit = this->DetPost_->outputs[0]->dtype.getStorageType().bits();
            // get original thr_f & thr_q(data_thr)
            thr_f = this->DetPost_->getAttr("thr_f").cast<icraft::xir::FloatImm>();
            auto data_thr_bool = this->DetPost_->getAttr("data_thr").has_value();
            if (data_thr_bool)
            {
                data_thr = this->DetPost_->getAttr("data_thr").cast<icraft::xir::Array<int64_t>>();
            }
            for (auto i : this->DetPost_->inputs)
            {
                auto storage = i->dtype.getStorageType();
                if (storage.is<icraft::xir::FloatType>())
                {
                    // Floating DetPost payloads are already expressed values.
                    // The compiler legitimately emits an empty scale array.
                    this->o_scale.emplace_back(1.0f);
                }
                else if (i->dtype.getNormratio().has_value() && i->dtype.getNormratio().value()->data.size() > 0)
                {
                    // std::cout << "Buyi DetPost input has normratio." << std::endl;
                    this->o_scale.emplace_back(i->dtype.getNormratio().value()[0]);
                }
                else if (i->dtype.getScale().has_value() && i->dtype.getScale().value().size() > 0)
                {
                    // std::cout << "Zhuge DetPost input has scale." << std::endl;
                    //  getScale() 返回的是 Optional<QuantizedScaleArray>
                    icraft::xir::QuantizedScaleArray scale_array = i->dtype.getScale().value();
                    icraft::xir::QuantizedScale q_scale = scale_array[0];
                    this->o_scale.emplace_back(q_scale->origin_scale);
                }
                else { throw std::runtime_error("Integer DetPost input lacks a scale"); }
            }

            // for (auto v : this->o_scale)
            // {
            //     std::cout << "DetPost input o_scale value: " << v << std::endl;
            // }
        }

        for (int op_idx = 0; op_idx < oplist.size(); op_idx++)
        {
            if (oplist[op_idx]->compile_target->typeKey().find("BuyiTargetNode") != std::string::npos || oplist[op_idx]->compile_target->typeKey().find("FPGATargetNode") != std::string::npos || oplist[op_idx]->compile_target->typeKey().find("ZhugeTargetNode") != std::string::npos)
            {
                this->bit = oplist[op_idx]->outputs[0]->dtype.getStorageType().bits();
                break;
            }
        }
        Cubic temp_cubic;
        for (auto i : this->i_shape)
        {
            if (i.size() == 4)
            {
                temp_cubic.h = i[1];
                temp_cubic.w = i[2];
                temp_cubic.c = i[3];
                this->i_cubic.emplace_back(temp_cubic);
            }
        }
        for (auto i : this->head_hardop_i_shape)
        {
            if (i.size() == 5)
            {
                temp_cubic.h = i[2];
                temp_cubic.w = i[3];
                temp_cubic.c = i[4];
                this->head_hardop_i_shape_cubic.emplace_back(temp_cubic);
            }
        }
        for (auto i : this->o_shape)
        {
            if (i.size() == 4)
            {
                temp_cubic.h = i[1];
                temp_cubic.w = i[2];
                temp_cubic.c = i[3];
                this->o_cubic.emplace_back(temp_cubic);
            }
        }
        spdlog::info("NetInfo init done");
        // if (this->mmu) {
        //     spdlog::info("The speedmode or compressFtmp of input model is enabled, The device must enable mmu mode.");
        // }
        // else {
        //     spdlog::info("Neither speedmode nor compressFtmp of input model is enabled, You can decide whether to enable the mmu mode of the device by using the configuration item mmu in the yaml file");

        //}
    }

    virtual std::unordered_set<std::string> fpgaOPlist(icraft::xir::Network &network)
    {
        std::unordered_set<std::string> customop_set;
        auto oplist = network->ops;
        int op_idx = 0;
        for (; op_idx < oplist.size(); op_idx++)
        {
            // std::cout << op_idx << ":" << oplist[op_idx]->typeKey() << std::endl;
            if (oplist[op_idx]->typeKey().find("Resize") != std::string::npos)
            {
                this->resize_on = true;
            }
            if (oplist[op_idx]->typeKey().find("SwapOrder") != std::string::npos)
            {
                this->swaporder_on = true;
            }
            if (oplist[op_idx]->typeKey().find("ImageMakeNode") != std::string::npos)
            {
                this->ImageMake_on = true;
                this->ImageMakes_.emplace_back(oplist[op_idx]);
                this->Imk_op_indices.push_back(op_idx);
                std::cout << "ImageMake_op_idx: " << this->Imk_op_indices.back() << ", ImageMake_op_id: " << this->ImageMakes_.back()->op_id << std::endl;
            }
            if (oplist[op_idx]->typeKey().find("DetPostNode") != std::string::npos)
            {
                this->DetPost_on = true;
                this->DetPost_ = oplist[op_idx];
            }
            if (oplist[op_idx]->typeKey().find("DetPostZGNode") != std::string::npos)
            {
                this->DetPost_on = true;
                this->DetPost_ = oplist[op_idx];
            }
            if (oplist[op_idx]->typeKey().find("WarpAffineNode") != std::string::npos)
            {
                this->WarpAffine_on = true;
                this->WarpAffine_ = oplist[op_idx];
                this->WarpAffine_op_idx = op_idx;
            }
            if (oplist[op_idx]->typeKey().find("CastNode") != std::string::npos)
            {
                if (this->First_cast_op_idx == -1)
                    this->First_cast_op_idx = op_idx;
            }
            if (oplist[op_idx]->typeKey().find("customop") != std::string::npos)
            {
                customop_set.insert(std::string(oplist[op_idx]->typeKey()));
            }
        }

        if (this->resize_on)
            this->inp_shape_opid++;
        // if (this->swaporder_on) this->inp_shape_opid++;
        return customop_set;
    }
};
