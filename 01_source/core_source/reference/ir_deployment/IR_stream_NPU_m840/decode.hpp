// IR六头解码：3个尺度，每尺度1通道分类与64通道DFL回归。
// 固定conf>0.25、NMS IoU 0.7，最多300框；回映射减去上方64行补边。

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace ir840  {
    // 兼容NHWC和NCHW头布局，get将两种内存顺序统一为(pixel,channel)。
    struct Head {
        int h=0,c=0;
        bool nhwc=true;
        std::vector<float> v;
        float get(int pixel,int channel)const {
            return nhwc?v[size_t(pixel)*c+channel]:v[size_t(channel)*h*h+pixel];
        }
    };
    struct Detection {
        std::array<float,4> box;
        float score;
    };
    // 分类做sigmoid；四边各16桶softmax求期望距离，再按8/16/32步长还原坐标。
    inline std::vector<Detection> decode(const std::vector<Head>& heads) {
        if(heads.size()!=6)throw std::runtime_error("Six heads required");
        std::vector<Detection> candidates;
        for(int scale=0;scale<3;++scale) {
            int stride=8<<scale,h=640/stride;
            const Head* cls=nullptr;
            const Head* reg=nullptr;
            for(const auto& head:heads)if(head.h==h) {
                if(head.c==1)cls=&head;
                else if(head.c==64)reg=&head;
            }
            if(!cls||!reg)throw std::runtime_error("Head dimensions differ");
            for(int pixel=0;pixel<h*h;++pixel) {
                float logit=std::clamp(cls->get(pixel,0),-80.f,80.f);
                float score=1.f/(1.f+std::exp(-logit));
                if(!(score>.25f))continue;
                std::array<float,4> dist {
                };
                for(int side=0;side<4;++side) {
                    float maximum=-INFINITY;
                    for(int j=0;j<16;++j)maximum=std::max(maximum,reg->get(pixel,side*16+j));
                    float exponential[16],sum=0;
                    for(int j=0;j<16;++j) {
                        exponential[j]=std::exp(reg->get(pixel,side*16+j)-maximum);
                        sum+=exponential[j];
                    }
                    float value=0;
                    for(int j=0;j<16;++j)value+=(exponential[j]/sum)*float(j);
                    dist[side]=value;
                }
                float x=float(pixel%h)+.5f,y=float(pixel/h)+.5f;
                candidates.push_back( {
                    {
                        (x-dist[0])*stride,(y-dist[1])*stride,(x+dist[2])*stride,(y+dist[3])*stride
                    },score
                });
            }
        }
        // 按置信度稳定降序排序，再执行贪心NMS。
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b) {
            return a.score>b.score;
        });
        std::vector<Detection> selected;
        while(!candidates.empty() && selected.size()<300) {
            auto current=candidates.front();
            selected.push_back(current);
            std::vector<Detection> rest;
            for(size_t i=1;i<candidates.size();++i) {
                const auto& b=candidates[i];
                const auto&a=current.box;
                const auto&v=b.box;
                float intersection=std::max(0.f,std::min(a[2],v[2])-std::max(a[0],v[0]))*std::max(0.f,std::min(a[3],v[3])-std::max(a[1],v[1]));
                float area=(a[2]-a[0])*(a[3]-a[1])+(v[2]-v[0])*(v[3]-v[1])-intersection;
                if(intersection/std::max(area,1e-12f)<=.7f)rest.push_back(b);
            }
            candidates.swap(rest);
        }
        // 裁回原640×512区域：横坐标不补偿，纵坐标减64；与RGB右/下补边不同。
        for(auto& d:selected) {
            d.box[0]=std::clamp(d.box[0],0.f,640.f);
            d.box[2]=std::clamp(d.box[2],0.f,640.f);
            d.box[1]=std::clamp(d.box[1]-64.f,0.f,512.f);
            d.box[3]=std::clamp(d.box[3]-64.f,0.f,512.f);
        }
        return selected;
    }
    // 只序列化原检测坐标和置信度；标签由评分程序独立读取，不进入推理。
    inline std::string json(const std::vector<Detection>& boxes) {
        std::ostringstream out;
        out<<std::setprecision(9)<<"{\"boxes\":[";
        for(size_t i=0;i<boxes.size();++i) {
            if(i)out<<",";
            out<<"[";
            for(int j=0;j<4;++j) {
                if(j)out<<",";
                out<<boxes[i].box[j];
            }
            out<<"]";
        }
        out<<"],\"confidence\":[";
        for(size_t i=0;i<boxes.size();++i) {
            if(i)out<<",";
            out<<boxes[i].score;
        }
        out<<"]}";
        return out.str();
    }
}
