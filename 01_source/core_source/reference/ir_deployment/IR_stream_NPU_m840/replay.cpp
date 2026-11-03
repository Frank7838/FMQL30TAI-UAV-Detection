// 离线解码回放工具，只读已导出的六头文件，不打开NPU或相机。

#include "decode.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    try {
        std::vector<ir840::Head> heads;
        for(int i=0;i<6;++i) {
            ir840::Head h;
            h.h=80>>(i/2);
            h.c=i%2?64:1;
            h.nhwc=true;
            auto path=std::filesystem::path(argv[1])/(std::to_string(i)+".SFB.bin");
            std::ifstream in(path,std::ios::binary);
            std::vector<char>b((std::istreambuf_iterator<char>(in)), {
            });
            if(b.size()!=size_t(h.h*h.h*h.c*4))throw std::runtime_error("Head size differs");
            h.v.resize(b.size()/4);
            std::memcpy(h.v.data(),b.data(),b.size());
            heads.push_back(std::move(h));
        }
        std::cout<<ir840::json(ir840::decode(heads))<<std::endl;
        return 0;
    }
    catch(const std::exception&e) {
        std::cerr<<e.what()<<std::endl;
        return 1;
    }
}
