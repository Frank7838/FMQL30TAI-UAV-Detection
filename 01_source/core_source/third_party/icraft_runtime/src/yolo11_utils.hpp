#pragma once

#include <icraft-xir/core/network.h>
#include <vector>
#include <string>
#include "opencv2/opencv.hpp"
#include <unordered_set>


std::vector<int> _getReal_out_channles(std::vector<int>ori_out_channles, int bits, int NOA) {
    int MINC = 0;
    int MAXC = 0;
    if (bits == 8) {

        MAXC = 64;
        MINC = 8;
    }
    else if (bits == 16) {
        MAXC = 32;
        MINC = 4;
    }
    else {
        throw "bits != 8 or 16";
        exit(EXIT_FAILURE);
    }
    //calc for anchor length. the last part should be supplemented with the integral multiple of min channel

    auto _last_c = [&](int ori_c)->int {
        return ceil((float)ori_c / (float)MINC) * MINC + MINC;
    };

    //calc for anchor length. the !last part should be supplemented with the integral multiple of max channel
    auto _mid_c = [&](int ori_c)->int {
        return ceil((float)ori_c / (float)MAXC) * MAXC;
    };


    int anchor_length = 0;
    switch (ori_out_channles.size()) {
    case 1: {
        int oneAnchor = ori_out_channles[0] / NOA;
        int anchor_length = _last_c(oneAnchor);
        return  std::vector<int>{ NOA* anchor_length };
    }
    case 2: {
        anchor_length = _last_c(ori_out_channles[1])
            + _mid_c(ori_out_channles[0]);
        return  std::vector<int>{ _mid_c(ori_out_channles[0]), _last_c(ori_out_channles[1]) };

    }
    case 3: {
        anchor_length = _last_c(ori_out_channles[2])
            + _mid_c(ori_out_channles[1]) + _mid_c(ori_out_channles[0]);
        return  std::vector<int>{ _mid_c(ori_out_channles[0]), _mid_c(ori_out_channles[1]),
            _last_c(ori_out_channles[2]) };
    }
    default: {
        throw "parts > 3, DetPost支持1个bbox的信息最多分散在3层中!";
        exit(EXIT_FAILURE);
    }

    }
}


//根据每个head数，将原本1维的norm分组
std::vector<std::vector<float>> set_norm_by_head(int NOH, int parts, std::vector<float>& normalratio) {
    std::vector<std::vector<float>> _norm;
    for (size_t i = 0; i < NOH; i++)
    {
        std::vector<float> _norm_;
        for (size_t j = 0; j < parts; j++)
        {
            _norm_.push_back(normalratio[i * parts + j]);

        }
        _norm.push_back(_norm_);
    }
    return _norm;
}
