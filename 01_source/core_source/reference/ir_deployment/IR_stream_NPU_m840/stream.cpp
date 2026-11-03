// IR扩展：既有固定图像的真实NPU流式推理。输入已在CPU补成640×640 UInt8 RGB。
// 这不是红外相机采集入口，也不写自研告警寄存器；原实验最多1200个请求。

#define main m834_unused_probe_main
#include "../IR_probe_m834/probe.cpp"
#undef main
#include "decode.hpp"
#include <sstream>
// 每次读满固定字段并重试EINTR；uint32序号后跟1228800字节Tensor。
static void read_exact(int fd,char* p,size_t n) {
    while(n) {
        auto count=read(fd,p,n);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)throw std::runtime_error("Input stream closed");
        p+=count;
        n-=size_t(count);
    }
}
// 响应管道独立于stdout诊断；先发4字节JSON长度，再发完整JSON。
static void write_exact(int fd,const char*p,size_t n) {
    while(n) {
        auto count=write(fd,p,n);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)throw std::runtime_error("Result stream closed");
        p+=count;
        n-=size_t(count);
    }
}
// 参数为模型JSON、raw和响应fd；stdin顺序送帧，0xffffffff序号表示正常退出。
int main(int argc,char**argv) {
    if(argc!=4)return 2;
    try {
        using namespace icraft::xrt;
        guard();
        int output_fd=std::stoi(argv[3]);
        auto net=icraft::xir::Network::CreateFromJsonFile(argv[1]);
        net.lazyLoadParamsFromFile(argv[2]);
        require(net.inputs().size()==1 && net.outputs().size()==6,"One input/six heads required");
        auto type=net.inputs()[0].tensorType().clone();
        require(type->shape==icraft::xir::Array<int64_t> {
            1,640,640,3
        }
        &&type->element_dtype.isUInt(8),"UINT8 NHWC required");
        HostBackend::setThreadsNum(1);
        auto dev=Device::Open("axi://zg330aiu?npu=0x40000000&dma=0x80000000");
        auto session=Session::Create<zg330::ZG330Backend,HostBackend>(net, {
            dev,HostDevice::Default()
        });
        session.enableTimeProfile(true);
        session.apply();
        std::cout<<"M840_READY"<<std::endl;
        dev.reset(1);
        // Completion-state clear used by vendor yolo11s and accepted M705 worker.
        std::vector<char> data(1228800);
        uint32_t expected=0;
        for(;;) {
            uint32_t index;
            read_exact(0,reinterpret_cast<char*>(&index),4);
            if(index==0xffffffffU)break;
            require(index==expected && expected<1200,"Frame sequence/budget differs");
            read_exact(0,data.data(),data.size());
            guard();
            auto input=Tensor(type.clone()).mallocOn(HostDevice::MemRegion());
            std::memcpy(input.data().cptr(),data.data(),data.size());
            auto begin=std::chrono::steady_clock::now();
            // 同步forward后按厂商流程reset，输出头再拷回Host完成DFL/NMS。
            // 计时不含相机、网络、绘图等整机开销。
            auto tensors=session.forward( {
                input
            });
            dev.reset(1);
            auto after=std::chrono::steady_clock::now();
            require(tensors.size()==6,"Output count differs");
            std::vector<ir840::Head> heads;
            for(const auto& tensor:tensors) {
                auto t=tensor.to(HostDevice::MemRegion());
                auto shape=t.dtype()->shape;
                require(shape.size()==4 && shape[0]==1,"Four-dimensional heads required");
                ir840::Head h;
                if((shape[1]==1||shape[1]==64)&&shape[2]==shape[3]) {
                    h.c=int(shape[1]);
                    h.h=int(shape[2]);
                    h.nhwc=false;
                }
                else {
                    h.h=int(shape[1]);
                    h.c=int(shape[3]);
                    h.nhwc=true;
                    require(shape[1]==shape[2],"Square head required");
                }
                std::ostringstream out(std::ios::out|std::ios::binary);
                t.dump(out,"SFB");
                auto bytes=out.str();
                require(bytes.size()==size_t(h.h*h.h*h.c*4),"Head byte size differs");
                h.v.resize(bytes.size()/4);
                std::memcpy(h.v.data(),bytes.data(),bytes.size());
                for(float v:h.v)require(std::isfinite(v),"Nonfinite output");
                heads.push_back(std::move(h));
            }
            auto boxes=ir840::decode(heads);
            auto end=std::chrono::steady_clock::now();
            auto result=ir840::json(boxes);
            result.pop_back();
            std::ostringstream message;
            message<<std::setprecision(10)<<result<<",\"index\":"<<index<<",\"NPU_ms\":"<<std::chrono::duration<double,std::milli>(after-begin).count()<<",\"postprocess_ms\":"<<std::chrono::duration<double,std::milli>(end-after).count()<<",\"board_forward_postprocess_ms\":"<<std::chrono::duration<double,std::milli>(end-begin).count();
            if(index==0) {
                message<<",\"first_class_logit_max\":[";
                bool comma=false;
                for(const auto& h:heads)if(h.c==1) {
                    if(comma)message<<",";
                    message<<*std::max_element(h.v.begin(),h.v.end());
                    comma=true;
                }
                message<<"]";
            }
            message<<"}";
            auto text=message.str();
            uint32_t length=uint32_t(text.size());
            write_exact(output_fd,reinterpret_cast<char*>(&length),4);
            write_exact(output_fd,text.data(),text.size());
            ++expected;
        }
        Device::Close(dev);
        std::cout<<"M840_COMPLETE "<<expected<<std::endl;
        return 0;
    }
    catch(const std::exception&e) {
        std::cerr<<e.what()<<std::endl;
        return 1;
    }
}
