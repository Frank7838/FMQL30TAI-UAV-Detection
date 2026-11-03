// IR早期逐图Host输出导出，仅在PC读取模型与图像Tensor，保留六头供后续核对。

// Isolated Host-only SDK output probe. No sockets, board backend or GT access.
#include <icraft-xir/core/network.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-backends/hostbackend/backend.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc,char** argv)  {
    if(argc!=5) {
        std::cerr<<"IR_raw_host model.json model.raw inputs.tsv output_dir\n";
        return 2;
    }
    try  {
        using namespace icraft::xrt;
        auto network=icraft::xir::Network::CreateFromJsonFile(argv[1]);
        network.lazyLoadParamsFromFile(argv[2]);
        if(network.inputs().size()!=1)throw std::runtime_error("One input required");
        auto type=network.inputs()[0].tensorType().clone();
        if(type->shape!=icraft::xir::Array<int64_t> {
            1,640,640,3
        }
        || !type->element_dtype.isUInt(8)) throw std::runtime_error("Expected frozen UINT8 NHWC [1,640,640,3] input");
        auto folder=std::filesystem::path(argv[4]);
        if(std::filesystem::exists(folder))throw std::runtime_error("Preserve existing output folder");
        std::filesystem::create_directories(folder);
        HostBackend::setThreadsNum(1);
        auto device=HostDevice::Default();
        auto session=Session::Create<HostBackend>(network, {
            device
        });
        session.apply();
        std::ifstream manifest(argv[3]);
        if(!manifest)throw std::runtime_error("Manifest unreadable");
        std::ofstream receipt(folder/"frames.jsonl");
        std::string line;
        size_t count=0;
        while(std::getline(manifest,line))  {
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            auto split=line.find('\t');
            if(split==std::string::npos)throw std::runtime_error("Malformed input manifest");
            auto id=line.substr(0,split);
            auto path=line.substr(split+1);
            if(id.empty()||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos) throw std::runtime_error("Unsafe input ID");
            std::ifstream in(path,std::ios::binary);
            if(!in)throw std::runtime_error("Input unreadable");
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
            if(bytes.size()!=640*640*3)throw std::runtime_error("Input size differs");
            auto tensor=Tensor(type.clone()).mallocOn(HostDevice::MemRegion());
            std::memcpy(tensor.data().cptr(),bytes.data(),bytes.size());
            auto start=std::chrono::steady_clock::now();
            auto outputs=session.forward( {
                tensor
            });
            auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(outputs.size()!=6)throw std::runtime_error("Six raw heads required");
            auto sample=folder/id;
            if(std::filesystem::exists(sample))throw std::runtime_error("Repeated input ID");
            std::filesystem::create_directory(sample);
            std::ofstream meta(sample/"heads.json");
            meta<<"[";
            for(size_t i=0;i<outputs.size();++i) {
                auto t=outputs[i].to(HostDevice::MemRegion());
                std::ofstream out(sample/(std::to_string(i)+".SFB.bin"),std::ios::binary);
                t.dump(out,"SFB");
                if(!out)throw std::runtime_error("Output dump failed");
                if(i)meta<<",";
                meta<<"{\"index\":"<<i<<",\"value_id\":"<<network.outputs()[i]->v_id<<",\"shape\":[";
                for(size_t j=0;j<t.dtype()->shape.size();++j) {
                    if(j)meta<<",";
                    meta<<t.dtype()->shape[j];
                }
                meta<<"]}";
            }
            meta<<"]";
            receipt<<"{\"id\":\""<<id<<"\",\"SDK_Host_forward_ms\":"<<ms<<"}\n";
            receipt.flush();
            std::cout<<"IR_HOST_FRAME "<<++count<<" "<<id<<" "<<ms<<"ms"<<std::endl;
        }
        Device::Close(device);
        std::cout<<"IR_HOST_COMPLETE "<<count<<std::endl;
        return 0;
    }
    catch(const std::exception& e) {
        std::cerr<<e.what()<<std::endl;
        return 1;
    }
}
