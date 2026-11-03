// IR持久Host量化仿真，用于与实板输出核对，不连接板卡。
// strict/default/fake属于历史算术诊断模式，不能把不同模式结果混合评分。

// Persistent Host inference with a bounded reusable tensor slot, no labels or board.
#include <icraft-xir/core/network.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-backends/hostbackend/backend.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <chrono>
int main(int argc,char** argv) {
    if(argc!=6) {
        std::cerr<<"model.json model.raw input.bin fresh_slot default|strict|fake\n";
        return 2;
    }
    try {
        using namespace icraft::xrt;
        std::string mode(argv[5]);
        if(mode!="default" && mode!="strict" && mode!="fake")throw std::runtime_error("Unknown diagnostic arithmetic mode");
        auto folder=std::filesystem::path(argv[4]);
        if(std::filesystem::exists(folder))throw std::runtime_error("Preserve existing slot");
        auto net=icraft::xir::Network::CreateFromJsonFile(argv[1]);
        net.lazyLoadParamsFromFile(argv[2]);
        if(net.inputs().size()!=1 || net.outputs().size()!=6)throw std::runtime_error("One input/six heads required");
        auto type=net.inputs()[0].tensorType().clone();
        if(type->shape!=icraft::xir::Array<int64_t> {
            1,640,640,3
        }
        || !type->element_dtype.isUInt(8))throw std::runtime_error("UINT8 NHWC contract differs");
        HostBackend::setThreadsNum(1);
        auto backend=HostBackend::Init();
        bool original=backend.getFakeQf();
        if(mode!="default")backend.setFakeQf(mode=="fake");
        auto dev=HostDevice::Default();
        auto session=Session::CreateByOrder(net, {
            backend
        }, {
            dev
        });
        session.apply();
        std::filesystem::create_directory(folder);
        std::cout<<"IR_STREAM_READY defaultFakeQf="<<original<<" actualFakeQf="<<backend.getFakeQf()<<std::endl;
        std::string line;
        size_t count=0;
        while(std::getline(std::cin,line)) {
            if(line=="QUIT")break;
            if(line!=std::to_string(count) || count>=1200)throw std::runtime_error("Frame sequence/budget rejected");
            std::ifstream in(argv[3],std::ios::binary);
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {
            });
            if(bytes.size()!=1228800)throw std::runtime_error("Exact tensor bytes required");
            auto input=Tensor(type.clone()).mallocOn(HostDevice::MemRegion());
            std::memcpy(input.data().cptr(),bytes.data(),bytes.size());
            auto start=std::chrono::steady_clock::now();
            auto heads=session.forward( {
                input
            });
            auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            if(heads.size()!=6)throw std::runtime_error("Six heads required");
            {
                std::ofstream meta(folder/"heads.json",std::ios::trunc);
                meta<<"[";
                for(size_t i=0;i<heads.size();++i) {
                    auto t=heads[i].to(HostDevice::MemRegion());
                    std::ofstream out(folder/(std::to_string(i)+".SFB.bin"),std::ios::binary|std::ios::trunc);
                    t.dump(out,"SFB");
                    if(!out)throw std::runtime_error("Head dump failed");
                    if(i)meta<<",";
                    meta<<"{\"index\":"<<i<<",\"value_id\":"<<net.outputs()[i]->v_id<<",\"shape\":[";
                    for(size_t j=0;j<t.dtype()->shape.size();++j) {
                        if(j)meta<<",";
                        meta<<t.dtype()->shape[j];
                    }
                    meta<<"]}";
                }
                meta<<"]";
            }
            std::cout<<"IR_STREAM_DONE "<<count++<<" "<<ms<<std::endl;
        }
        Device::Close(dev);
        std::cout<<"IR_STREAM_COMPLETE "<<count<<std::endl;
        return 0;
    }
    catch(const std::exception& e) {
        std::cerr<<e.what()<<std::endl;
        return 1;
    }
}
