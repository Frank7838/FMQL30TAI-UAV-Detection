// IR早期单图NPU入口及共享guard定义；流式程序通过宏重命名旧main复用guard。
// 必须继承当前boot与真实设备锁；仅检查签名，不写告警或自行下载位流。

// One-image raw-head NPU probe. Only use through the matching Python guard.
#include <icraft-xir/core/network.h>
#include <icraft-xrt/core/session.h>
#include <icraft-xrt/dev/host_device.h>
#include <icraft-backends/hostbackend/backend.h>
#include <icraft-backends/zg330backend/zg330backend.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <chrono>
#include <cstdlib>
static void require(bool ok,const char* why) {
    if(!ok)throw std::runtime_error(why);
}
static void guard() {
    const char* expected=std::getenv("M834_EXPECTED_BOOT");
    const char* inherited=std::getenv("M834_GUARD_FD");
    require(expected && inherited,"Python preflight/owned lock required before opening SDK device");
    std::string boot;
    std::ifstream("/proc/sys/kernel/random/boot_id")>>boot;
    require(boot==expected,"Boot changed");
    int fd=std::stoi(inherited);
    struct stat held {
    },named {
    };
    require(fstat(fd,&held)==0 && stat("/root/fpga_gaming/ps_session_m53.lock",&named)==0,"Real lock missing");
    require(held.st_ino==943514 && held.st_ino==named.st_ino && held.st_dev==named.st_dev,"Real lock identity differs");
    require(flock(fd,LOCK_EX|LOCK_NB)==0,"Real lock unavailable");
    struct statvfs disk {
    };
    require(statvfs("/root/fpga_gaming",&disk)==0,"Disk query failed");
    require(uint64_t(disk.f_bavail)*disk.f_frsize>=(1ULL<<30)+(64ULL<<20),"Disk floor rejected");
    std::ifstream mem("/proc/meminfo");
    std::string line;
    uint64_t available=0;
    while(std::getline(mem,line))if(line.rfind("MemAvailable:",0)==0)available=std::stoull(line.substr(13));
    require(available>=131072,"Memory floor rejected");
    int physical=open("/dev/mem",O_RDONLY|O_SYNC);
    require(physical>=0,"Read-only signature access failed");
    void* map=mmap(nullptr,4096,PROT_READ,MAP_SHARED,physical,0x400C0000ULL);
    if(map==MAP_FAILED) {
        close(physical);
        throw std::runtime_error("Signature mmap failed");
    }
    uint32_t signature=*reinterpret_cast<volatile const uint32_t*>(static_cast<char*>(map)+0xC0);
    munmap(map,4096);
    close(physical);
    require(signature==0xA5050001,"M512 absent; no SDK device opened");
}
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--describe") {
        std::cout<<"IR_NPU_probe: one UINT8 NHWC RGB640 tensor, six SFB heads, no alarm or camera; guarded execution required\n";
        return 0;
    }
    if(argc!=5) {
        std::cerr<<"Use guarded launcher; model.json model.raw input.bin new_output_dir\n";
        return 2;
    }
    try {
        using namespace icraft::xrt;
        guard();
        std::filesystem::path folder(argv[4]);
        require(!std::filesystem::exists(folder),"Preserve output directory");
        std::ifstream stream(argv[3],std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(stream)), {
        });
        require(bytes.size()==1228800,"Expected exactly one UINT8 NHWC RGB640 tensor");
        auto net=icraft::xir::Network::CreateFromJsonFile(argv[1]);
        net.lazyLoadParamsFromFile(argv[2]);
        require(net.inputs().size()==1 && net.outputs().size()==6,"Input/output count differs");
        auto type=net.inputs()[0].tensorType().clone();
        require(type->shape==icraft::xir::Array<int64_t> {
            1,640,640,3
        }
        && type->element_dtype.isUInt(8),"UINT8 NHWC input required");
        guard();
        // Repeat after reading model/input, before any SDK hardware open.
        HostBackend::setThreadsNum(1);
        auto dev=Device::Open("axi://zg330aiu?npu=0x40000000&dma=0x80000000");
        auto session=Session::Create<zg330::ZG330Backend,HostBackend>(net, {
            dev,HostDevice::Default()
        });
        session.enableTimeProfile(true);
        session.apply();
        auto input=Tensor(type).mallocOn(HostDevice::MemRegion());
        std::memcpy(input.data().cptr(),bytes.data(),bytes.size());
        auto start=std::chrono::steady_clock::now();
        auto heads=session.forward( {
            input
        });
        auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        require(heads.size()==6,"Six heads required");
        std::filesystem::create_directory(folder);
        std::ofstream meta(folder/"heads.json");
        meta<<"[";
        for(size_t i=0;i<heads.size();++i) {
            auto t=heads[i].to(HostDevice::MemRegion());
            std::ofstream raw(folder/(std::to_string(i)+".SFB.bin"),std::ios::binary);
            t.dump(raw,"SFB");
            require(bool(raw),"Head write failed");
            if(i)meta<<",";
            meta<<"{\"index\":"<<i<<",\"value_id\":"<<net.outputs()[i]->v_id<<",\"shape\":[";
            for(size_t j=0;j<t.dtype()->shape.size();++j) {
                if(j)meta<<",";
                meta<<t.dtype()->shape[j];
            }
            meta<<"]}";
        }
        meta<<"]";
        meta.close();
        std::ofstream receipt(folder/"timing.json");
        receipt<<"{\"images\":1,\"actual_NPU_forward_ms\":"<<ms<<",\"alarm_writes\":0,\"camera\":false}";
        Device::Close(dev);
        std::cout<<"NPU_ONE_IMAGE_COMPLETE "<<ms<<"ms\n";
        return 0;
    }
    catch(const std::exception& e) {
        std::cerr<<e.what()<<std::endl;
        return 1;
    }
}
