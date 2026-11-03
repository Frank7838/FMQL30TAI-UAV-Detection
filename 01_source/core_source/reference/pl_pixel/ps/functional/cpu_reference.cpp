// 提供CPU取样、BGR转RGB、补边和计时参考。
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

constexpr size_t OUT = 640 * 640 * 3;
using Kernel = void (*)(const uint8_t *, uint8_t *, bool);
static volatile uint64_t sink = 0;

static std::vector<uint8_t> load(const std::string &p, size_t n) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot read input");
    std::vector<uint8_t> v(n);
    f.read(reinterpret_cast<char *>(v.data()), static_cast<std::streamsize>(n));
    if (f.gcount() != static_cast<std::streamsize>(n) || f.peek() != EOF)
        throw std::runtime_error("Exact input size required");
    return v;
}
// Independent coordinate definition, used for correctness, never as slow timed baseline.
static void reference(const uint8_t *s, uint8_t *d, bool a) {
    for (int y = 0; y < 640; ++y)
        for (int x = 0; x < 640; ++x) {
            const size_t o = (size_t(y) * 640 + x) * 3;
            if (y >= (a ? 360 : 480)) {
                d[o] = d[o + 1] = d[o + 2] = 114;
                continue;
            }
            const size_t i =
                (size_t(a ? 3 * y + 1 : y) * (a ? 1920 : 640) + (a ? 3 * x + 1 : x)) * 3;
            d[o] = s[i + 2];
            d[o + 1] = s[i + 1];
            d[o + 2] = s[i];
        }
}
__attribute__((noinline)) static void fused(const uint8_t *__restrict s, uint8_t *__restrict d,
                                            bool a) {
    const int h = a ? 360 : 480;
    for (int y = 0; y < h; ++y) {
        const uint8_t *p = s + size_t(a ? 3 * y + 1 : y) * (a ? 1920 : 640) * 3 + (a ? 3 : 0);
        uint8_t *q = d + size_t(y) * 640 * 3;
        for (int x = 0; x < 640; ++x, p += (a ? 9 : 3), q += 3) {
            q[0] = p[2];
            q[1] = p[1];
            q[2] = p[0];
        }
    }
    std::memset(d + size_t(h) * 640 * 3, 114, OUT - size_t(h) * 640 * 3);
}
#if defined(__aarch64__)
__attribute__((noinline)) static void neon(const uint8_t *__restrict s, uint8_t *__restrict d,
                                           bool a) {
    const int h = a ? 360 : 480;
    const uint8_t idx_bytes[16] = {1, 4, 7, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46};
    const uint8x16_t indices = vld1q_u8(idx_bytes);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < 640; x += 16) {
            uint8x16x3_t out;
            if (a) {
                const uint8_t *p = s + (size_t(3 * y + 1) * 1920 + 3 * x) * 3;
                const auto u = vld3q_u8(p), v = vld3q_u8(p + 48), w = vld3q_u8(p + 96);
                for (int c = 0; c < 3; ++c) {
                    uint8x16x3_t table;
                    table.val[0] = u.val[2 - c];
                    table.val[1] = v.val[2 - c];
                    table.val[2] = w.val[2 - c];
                    out.val[c] = vqtbl3q_u8(table, indices);
                }
            } else {
                const auto p = vld3q_u8(s + (size_t(y) * 640 + x) * 3);
                out.val[0] = p.val[2];
                out.val[1] = p.val[1];
                out.val[2] = p.val[0];
            }
            vst3q_u8(d + (size_t(y) * 640 + x) * 3, out);
        }
    std::memset(d + size_t(h) * 640 * 3, 114, OUT - size_t(h) * 640 * 3);
}
#endif
static double seconds(clockid_t c) {
    timespec t{};
    if (clock_gettime(c, &t))
        throw std::runtime_error("clock_gettime failed");
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static uint64_t checksum(const uint8_t *p, size_t n) {
    uint64_t v = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        v ^= p[i];
        v *= 1099511628211ull;
    }
    return v;
}
int main(int argc, char **argv) try {
    if (argc != 5)
        throw std::runtime_error("Usage: benchmark A|B input.bin expected.bin verify|measure");
    const std::string profile = argv[1], mode = argv[4];
    if ((profile != "A" && profile != "B") || (mode != "verify" && mode != "measure"))
        throw std::runtime_error("Invalid mode");
    const bool a = profile == "A";
    auto src = load(argv[2], a ? 6220800 : 921600), expected = load(argv[3], OUT);
    const auto before = checksum(src.data(), src.size());
    std::vector<uint8_t> actual(OUT + 128, 165), ref(OUT);
    reference(src.data(), ref.data(), a);
    if (ref != expected)
        throw std::runtime_error("Independent reference disagrees with sealed CPU golden");
    std::vector<std::pair<std::string, Kernel>> methods{{"fused", fused}};
#if defined(__aarch64__)
    methods.push_back({"neon", neon});
#endif
    for (const auto &k : methods) {
        std::fill(actual.begin(), actual.end(), 165);
        k.second(src.data(), actual.data() + 64, a);
        if (!std::equal(expected.begin(), expected.end(), actual.begin() + 64) ||
            !std::all_of(actual.begin(), actual.begin() + 64, [](uint8_t x) { return x == 165; }) ||
            !std::all_of(actual.end() - 64, actual.end(), [](uint8_t x) { return x == 165; }))
            throw std::runtime_error("Output/guard mismatch: " + k.first);
        std::cout << "{\"event\":\"verify\",\"profile\":\"" << profile << "\",\"method\":\""
                  << k.first << "\",\"bytes\":" << OUT
                  << ",\"fnv1a64\":" << checksum(actual.data() + 64, OUT) << "}" << std::endl;
    }
    if (mode == "measure") {
        // Rotating four complete source frames; no artificial cache flush.
        std::vector<std::vector<uint8_t>> pool(4, src);
        for (size_t f = 1; f < pool.size(); ++f)
            for (auto &b : pool[f])
                b ^= uint8_t(37 * f);
        std::vector<uint8_t> dst(OUT), host(OUT);
        constexpr int WARMUP = 16, ROUNDS = 5, ITER = 128;
        for (const auto &k : methods)
            for (int i = 0; i < WARMUP; ++i)
                k.second(pool[i % 4].data(), dst.data(), a);
        for (int round = 0; round < ROUNDS; ++round)
            for (size_t offset = 0; offset < methods.size(); ++offset) {
                const auto &k = methods[(offset + round) % methods.size()];
                for (int with_copy = 0; with_copy < 2; ++with_copy) {
                    const auto c0 = seconds(CLOCK_PROCESS_CPUTIME_ID),
                               w0 = seconds(CLOCK_MONOTONIC_RAW);
                    for (int i = 0; i < ITER; ++i) {
                        k.second(pool[i % 4].data(), dst.data(), a);
                        if (with_copy)
                            std::memcpy(host.data(), dst.data(), OUT);
                        asm volatile("" : : "r"(dst.data()), "r"(host.data()) : "memory");
                    }
                    const auto wall = seconds(CLOCK_MONOTONIC_RAW) - w0,
                               cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - c0;
                    sink = checksum(with_copy ? host.data() : dst.data(), OUT);
                    std::cout.precision(12);
                    std::cout << "{\"event\":\"timing\",\"profile\":\"" << profile
                              << "\",\"method\":\"" << k.first << "\",\"host_copy\":" << with_copy
                              << ",\"round\":" << round << ",\"iterations\":" << ITER
                              << ",\"wall_ms_per_frame\":" << wall * 1000 / ITER
                              << ",\"cpu_ms_per_frame\":" << cpu * 1000 / ITER << "}" << std::endl;
                }
            }
    }
    if (checksum(src.data(), src.size()) != before)
        throw std::runtime_error("Source modified");
    std::cout
        << "{\"event\":\"complete\",\"passed\":true,\"source_unchanged\":true,\"device_calls\":0}"
        << std::endl;
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return 1;
}
