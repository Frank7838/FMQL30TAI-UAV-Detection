// SDK打开前与每次推理前核对boot、继承锁和业务签名。
// 锁inode943514绑定当前已部署文件系统，不是通用硬件常数；迁移整块板端须重新确认。

#pragma once
// 检查推理进程继承的启动信息和设备锁。
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

static void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
// 在打开SDK和每次forward前核对当前boot、继承fd、真实锁文件身份、资源下限和PL签名。
// 核对当前文件系统中的设备锁与继承句柄。
static void product_guard() {
    const char *expected = std::getenv("M834_EXPECTED_BOOT");
    const char *inherited = std::getenv("M834_GUARD_FD");
    require(expected && inherited,
            "Python preflight/owned lock required before opening SDK device");
    std::string boot;
    std::ifstream("/proc/sys/kernel/random/boot_id") >> boot;
    require(boot == expected, "Boot changed");
    // fd由Python持锁后通过pass_fds继承；核对inode与设备号，防止同名文件被替换。
    int fd = std::stoi(inherited);
    struct stat held {
    }, named{};
    require(fstat(fd, &held) == 0 && stat("/root/fpga_gaming/ps_session_m53.lock", &named) == 0,
            "Real lock missing");
    require(held.st_ino == 943514 && held.st_ino == named.st_ino && held.st_dev == named.st_dev,
            "Real lock identity differs");
    require(flock(fd, LOCK_EX | LOCK_NB) == 0, "Real lock unavailable");
    struct statvfs disk {};
    require(statvfs("/root/fpga_gaming", &disk) == 0, "Disk query failed");
    require(uint64_t(disk.f_bavail) * disk.f_frsize >= (1ULL << 30) + (64ULL << 20),
            "Disk floor rejected");
    std::ifstream mem("/proc/meminfo");
    std::string line;
    uint64_t available = 0;
    while (std::getline(mem, line))
        if (line.rfind("MemAvailable:", 0) == 0)
            available = std::stoull(line.substr(13));
    require(available >= 131072, "Memory floor rejected");
    int physical = open("/dev/mem", O_RDONLY | O_SYNC);
    require(physical >= 0, "Read-only signature access failed");
    // 只读映射0x400C0000的4KiB窗口读取0xC0签名；此保护函数不写任何告警寄存器。
    void *map = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, physical, 0x400C0000ULL);
    if (map == MAP_FAILED) {
        close(physical);
        throw std::runtime_error("Signature mmap failed");
    }
    uint32_t signature =
        *reinterpret_cast<volatile const uint32_t *>(static_cast<char *>(map) + 0xC0);
    munmap(map, 4096);
    close(physical);
    require(signature == 0xA5050001, "M512 absent; no SDK device opened");
}
