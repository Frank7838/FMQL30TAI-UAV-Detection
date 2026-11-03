// 在同一ARM核上比较CPU与FPGA的全通路预处理耗时。
#define main historical_cpu_benchmark_main
#include "cpu_reference.cpp"
#undef main
#include "cached_mmio.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <stdexcept>
#include <iomanip>
#include <sched.h>

static uint64_t test_now(clockid_t id = CLOCK_MONOTONIC) {
    timespec t{};
    if (clock_gettime(id, &t))
        throw std::runtime_error("clock_gettime");
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
static void need(bool b, const std::string &s) {
    if (!b)
        throw std::runtime_error(s);
}
static void rc(int v, const std::string &s) {
    if (v)
        throw std::runtime_error(s + ":" + std::to_string(v));
}
static const size_t test_out_bytes = 1228800;
static plc_mmio *hardware = nullptr;
static plc_lease *lease = nullptr;
static int lockfd = -1;
static uint64_t deadline_ns;
static void budget() { need(test_now() < deadline_ns, "60s paired cost deadline"); }
static plc_mmio_snapshot snapshot() {
    plc_mmio_snapshot s{};
    rc(plc_mmio_get_snapshot(hardware, &s), "snapshot");
    return s;
}
static void store(uint32_t off, uint32_t value) {
    rc(plc_mmio_write32(hardware, off, value), "register write");
}
static plc_idle_guard guard() {
    plc_idle_guard g{};
    rc(plc_mmio_make_idle_guard(hardware, &g), "actual idle guard");
    return g;
}
static void open_hardware() {
    rc(plc_mmio_open(&hardware, lockfd, 943514), "MMIO open");
    auto s = snapshot();
    rc(plc_mmio_expect_epoch(hardware, s.epoch), "epoch pin");
    need(s.enable == 0, "starts already enabled; no initial write");
}
static void configure(bool a) {
    auto info = plc_lease_info{};
    rc(plc_lease_get_info(lease, &info), "lease info");
    plc_mmio_descriptor d{uint32_t(info.physical_base), uint32_t(info.physical_base + info.bytes),
                          uint32_t(info.src_physical), uint32_t(info.dst_physical),
                          uint32_t(a ? 6220800 : 921600)};
    rc(plc_mmio_validate_mode_descriptor(a ? 2 : 1, &d), "descriptor range");
    const uint32_t offsets[] = {0x10, 0x14, 0x18, 0x1c, 0x20, 0x3c};
    const uint32_t values[] = {d.base,        d.limit,  d.source,
                               d.destination, d.length, uint32_t(a ? 2 : 1)};
    uint64_t end = test_now() + 30000000000ull;
    unsigned attempts = 0;
    for (unsigned j = 0; j < 6; ++j) {
        bool complete = false;
        while (!complete) {
            need(test_now() < end && ++attempts <= 20, "configuration finite attempt budget");
            const auto before = snapshot();
            plc_config_attempt x{};
            const int config_rc = plc_mmio_config_once(hardware, offsets[j], values[j], &x);
            const auto after = snapshot();
            std::cerr << "CONFIG_DIAGNOSTIC {\"attempt\":" << attempts
                      << ",\"descriptor_index\":" << j << ",\"offset\":" << offsets[j]
                      << ",\"expected\":" << values[j] << ",\"rc\":" << config_rc
                      << ",\"issued\":" << x.issued << ",\"readback\":" << x.readback
                      << ",\"before_status\":" << before.status
                      << ",\"before_enable\":" << before.enable
                      << ",\"before_epoch\":" << before.epoch
                      << ",\"after_status\":" << after.status
                      << ",\"after_enable\":" << after.enable << ",\"after_epoch\":" << after.epoch
                      << ",\"accepted_before\":" << x.accepted_before
                      << ",\"accepted_after\":" << x.accepted_after
                      << ",\"rejected_after\":" << x.rejected_after << "}" << std::endl;
            rc(config_rc, "configuration once");
            if (x.issued) {
                need(x.readback == values[j], "configuration readback");
                complete = true;
            } else
                usleep(1000);
        }
    }
}
static bool safe_close() {
    try {
        if (hardware) {
            auto s = snapshot();
            if (s.enable)
                store(0x30, 0);
            if (lease) {
                auto g = guard();
                rc(plc_lease_release(&lease, &g), "release with real idle proof");
            }
            rc(plc_mmio_close(&hardware), "MMIO close");
        }
        if (lockfd >= 0) {
            close(lockfd);
            lockfd = -1;
        }
        return true;
    } catch (const std::exception &e) {
        std::cerr << "CLEANUP_REJECTED " << e.what() << std::endl;
        return false;
    }
}
struct Stage {
    uint64_t copy_in = 0, cache_handoff = 0, start = 0, wait = 0, reclaim = 0, copy_out = 0;
};
static void pl_frame(const uint8_t *src, size_t n, uint8_t *dst, Stage &p) {
    uint64_t t = test_now();
    rc(plc_lease_write_source(lease, src, n), "source copy");
    p.copy_in = test_now() - t;
    t = test_now();
    plc_lease_info info{};
    rc(plc_lease_handoff(lease, &info), "cache handoff");
    p.cache_handoff = test_now() - t;
    t = test_now();
    store(0x30, 1);
    store(8, 1);
    p.start = test_now() - t;
    t = test_now();
    const uint64_t end = t + 2000000000ull;
    while (true) {
        plc_mmio_snapshot s{};
        rc(plc_mmio_get_wait_snapshot(hardware, &s), "session wait snapshot");
        need(!(s.status & (4 | 8 | 32)), "DMA error/quarantine/rejected");
        if (!(s.status & (1 | 16 | 256 | 512)) && (s.status & 2))
            break;
        need(test_now() < end, "DMA completion timeout, retain ownership");
        usleep(100);
    }
    p.wait = test_now() - t;
    t = test_now();
    store(0x30, 0);
    auto g = guard();
    rc(plc_lease_reclaim(lease, &g), "real completion and cache reclaim");
    p.reclaim = test_now() - t;
    t = test_now();
    rc(plc_lease_read_destination(lease, dst, test_out_bytes),
       "destination copy to ordinary output");
    p.copy_out = test_now() - t;
}

static uint32_t readreg(uint32_t off) {
    uint32_t x = 0;
    rc(plc_mmio_read32(hardware, off, &x), "read register");
    return x;
}

int main(int argc, char **argv) {
    deadline_ns = test_now() + 60000000000ull;
    unsigned entered = 0, completed = 0, passed = 0;
    try {
        need(argc == 4, "Usage: --measure-A|--measure-B expected_boot data_directory");
        const std::string mode = argv[1];
        need(mode == "--measure-A" || mode == "--measure-B", "fixed functional mode");
        const bool a = mode == "--measure-A";
        const unsigned frames = 10;
        const size_t n = a ? 6220800 : 921600;
        std::ifstream bf("/proc/sys/kernel/random/boot_id");
        std::string boot;
        std::getline(bf, boot);
        need(boot == argv[2], "boot identity");
        std::string root = argv[3];
        std::vector<std::vector<uint8_t>> sources, golden;
        for (unsigned j = 0; j < 2u; ++j) {
            std::string tag = a ? (j ? "A_0101" : "A_0100") : (j ? "B_0004" : "B_0003");
            sources.push_back(load(root + "/" + tag + "_input.bin", n));
            golden.push_back(load(root + "/" + tag + "_output.bin", test_out_bytes));
            std::vector<uint8_t> cpu(test_out_bytes);
            reference(sources.back().data(), cpu.data(), a);
            need(cpu == golden.back(), "CPU coordinate reference differs from synthetic golden");
        }
        cpu_set_t available;
        CPU_ZERO(&available);
        need(!sched_getaffinity(0, sizeof(available), &available), "affinity inventory");
        int core = 0;
        while (core < CPU_SETSIZE && !CPU_ISSET(core, &available))
            ++core;
        need(core < CPU_SETSIZE, "available core");
        cpu_set_t chosen;
        CPU_ZERO(&chosen);
        CPU_SET(core, &chosen);
        need(!sched_setaffinity(0, sizeof(chosen), &chosen), "fixed one core");
        std::cout << "{\"type\":\"measurement_contract\",\"profile\":\"" << (a ? "A" : "B")
                  << "\",\"core\":" << core
                  << ",\"warmup_pairs\":2,\"measured_pairs\":8,\"CPU_kernel\":\""
                  << (a ? "fused" : "NEON")
                  << "\",\"PL_includes_copy_cache_start_wait_reclaim_output\":true}" << std::endl;
        lockfd = open("/root/fpga_gaming/ps_session_m53.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        need(lockfd >= 0, "real lock FD");
        struct stat st {};
        need(!fstat(lockfd, &st) && st.st_ino == 943514 && st.st_dev == 45826 &&
                 S_ISREG(st.st_mode),
             "real lock inode/dev");
        need(flock(lockfd, LOCK_EX | LOCK_NB) == 0, "exclusive lock");
        open_hardware();
        auto initial = snapshot();
        // DONE may remain from the successful preceding frame; never CLEAR it.
        // Native bits10/11 are diagnostics. The original own-idle guard is unchanged.
        need(initial.enable == 0 && !(initial.status & 0x33d),
             "own idle without error/quarantine/pending/reject/outstanding");
        const uint32_t baseline_accepted = readreg(0x34), baseline_done = readreg(0x38);
        need(baseline_accepted == baseline_done, "no unmatched previous request");
        rc(plc_lease_create(&lease, lockfd, 943514), "one private cached 16MiB lease");
        plc_lease_info info{};
        rc(plc_lease_get_info(lease, &info), "lease info");
        std::cout << "{\"type\":\"lease\",\"pid\":" << getpid() << ",\"name\":\"" << info.name
                  << "\",\"base\":" << info.physical_base
                  << ",\"destination\":" << info.dst_physical
                  << ",\"initial_status\":" << initial.status
                  << ",\"initial_epoch\":" << initial.epoch
                  << ",\"baseline_accepted\":" << baseline_accepted
                  << ",\"baseline_completed\":" << baseline_done << "}" << std::endl;
        configure(a);
        auto g = guard();
        rc(plc_lease_prepare(lease, n, test_out_bytes, &g), "guarded prepare");
        need(readreg(0x34) == baseline_accepted && readreg(0x38) == baseline_done,
             "configuration issued no command");
        std::vector<uint8_t> output(test_out_bytes), source_check(n), cpu_output(test_out_bytes);
        for (unsigned f = 0; f < frames; ++f) {
            budget();
            unsigned j = f % 2;
            rc(plc_lease_fill_destination(lease, uint8_t(0xa5 ^ f), test_out_bytes),
               "destination sentinel");
            Stage timing;
            uint64_t cpu_wall = 0, cpu_process = 0, pl_wall = 0, pl_process = 0;
            auto cpu_run = [&]() {
                const uint64_t cp = test_now(CLOCK_PROCESS_CPUTIME_ID), t = test_now();
#if defined(__aarch64__)
                if (a)
                    fused(sources[j].data(), cpu_output.data(), a);
                else
                    neon(sources[j].data(), cpu_output.data(), a);
#else
                throw std::runtime_error(
                    "Measured CPU kernel requires AArch64; host timing forbidden");
#endif
                cpu_wall = test_now() - t;
                cpu_process = test_now(CLOCK_PROCESS_CPUTIME_ID) - cp;
            };
            auto pl_run = [&]() {
                const uint64_t cp = test_now(CLOCK_PROCESS_CPUTIME_ID), t = test_now();
                ++entered;
                pl_frame(sources[j].data(), n, output.data(), timing);
                ++completed;
                pl_wall = test_now() - t;
                pl_process = test_now(CLOCK_PROCESS_CPUTIME_ID) - cp;
            };
            const bool cpu_first = (f % 4 == 0 || f % 4 == 3);
            if (cpu_first) {
                cpu_run();
                pl_run();
            } else {
                pl_run();
                cpu_run();
            }
            need(cpu_output == golden[j], "efficient CPU kernel differs from same golden");
            std::cout << "{\"type\":\"paired_cost\",\"phase\":\"" << (f < 2 ? "warmup" : "measured")
                      << "\",\"pair\":" << f << ",\"input_index\":" << (a ? 100 + j : 3 + j)
                      << ",\"CPU_first\":" << (cpu_first ? "true" : "false")
                      << ",\"CPU_wall_ns\":" << cpu_wall << ",\"CPU_process_ns\":" << cpu_process
                      << ",\"PL_wall_ns\":" << pl_wall << ",\"PL_process_ns\":" << pl_process
                      << ",\"copy_in_ns\":" << timing.copy_in
                      << ",\"cache_handoff_ns\":" << timing.cache_handoff
                      << ",\"start_ns\":" << timing.start << ",\"wait_ns\":" << timing.wait
                      << ",\"reclaim_ns\":" << timing.reclaim
                      << ",\"copy_out_ns\":" << timing.copy_out << "}" << std::endl;
            const uint32_t accepted = readreg(0x34), done = readreg(0x38);
            need(accepted == baseline_accepted + f + 1 && done == baseline_done + f + 1,
                 "exact accepted/completed increments for this frame");
            need(readreg(0x24) == 0 && readreg(0x28) == test_out_bytes,
                 "hardware error0/outputbyte count");
            rc(plc_lease_read_source(lease, source_check.data(), n), "source readback");
            need(source_check == sources[j], "source unchanged");
            auto fg = guard();
            rc(plc_lease_check_full_batch(lease, &fg), "full lease guards after every frame");
            size_t differences = 0, first = output.size();
            for (size_t i = 0; i < output.size(); ++i)
                if (output[i] != golden[j][i]) {
                    ++differences;
                    if (first == output.size())
                        first = i;
                }
            std::string outname =
                root + "/actual_" + (a ? "A" : "B") + "_" + std::to_string(f) + ".bin";
            std::ofstream actual(outname, std::ios::binary);
            need(bool(actual), "actual output artifact");
            actual.write(reinterpret_cast<const char *>(output.data()), output.size());
            actual.close();
            need(bool(actual), "save output artifact");
            std::cout << "{\"type\":\"frame\",\"profile\":\"" << (a ? "A" : "B")
                      << "\",\"frame\":" << f << ",\"input_index\":" << (a ? 100 + j : 3 + j)
                      << ",\"different_bytes\":" << differences << ",\"first_difference\":" << first
                      << ",\"bytes\":" << output.size() << ",\"accepted\":" << accepted
                      << ",\"completed\":" << done
                      << ",\"source_unchanged\":true,\"full_guards\":true,\"wait_ns\":"
                      << timing.wait << "}" << std::endl;
            need(differences == 0, "PL output differs from CPU golden");
            ++passed;
        }
        need(entered == frames && completed == frames && passed == frames,
             "full fixed denominator");
        need(safe_close(), "safe final lease release");
        std::cout << "P2_SESSION_HOTPATH_PASS profile=" << (a ? "A" : "B")
                  << " CPU_golden_cases=" << 2 << " PL_frames=" << frames
                  << " DMA_starts=" << frames
                  << " source_unchanged=1 full_guards=1 lease_released=1 NPU_calls=0" << std::endl;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "SESSION_HOTPATH_FAILURE " << e.what() << " mmio=" << plc_mmio_error(hardware)
                  << " lease=" << plc_lease_error(lease) << std::endl;
        bool closed = safe_close();
        std::cout << "{\"type\":\"failed\",\"start_paths_entered\":" << entered
                  << ",\"completed_frames\":" << completed << ",\"passed_frames\":" << passed
                  << ",\"safe_closed\":" << closed << ",\"hold_PID\":" << getpid() << "}"
                  << std::endl;
        if (!closed) {
            signal(SIGTERM, SIG_IGN);
            signal(SIGHUP, SIG_IGN);
            while (true)
                pause();
        }
        return 1;
    }
}
