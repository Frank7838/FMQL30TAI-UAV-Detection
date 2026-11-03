// 纯CPU固定3倍中心采样，输入只读，输出缓冲独立；不访问FPGA或NPU。

/* Pure fixed-geometry CPU byte sampling, derived from accepted M23 centers.
 * No allocation, SDK, device, image-header construction or source write. */
#include <stddef.h>
#include <stdint.h>

#define SRC_BYTES ((size_t)1080 * 1920 * 3)
#define DST_BYTES ((size_t)360 * 640 * 3)

// 输入1920×1080×3，输出640×360×3，字节数必须精确匹配。
// 输入只读、输出独立；返回-1表示参数/指针范围错误，-2表示缓冲重叠。
int fpga_center_pack_m93(const unsigned char *src, size_t src_bytes, unsigned char *dst,
                         size_t dst_bytes) {
    if (!src || !dst || src_bytes != SRC_BYTES || dst_bytes != DST_BYTES)
        return -1;
    const uintptr_t s = (uintptr_t)src, d = (uintptr_t)dst;
    if (s > UINTPTR_MAX - src_bytes || d > UINTPTR_MAX - dst_bytes)
        return -1;
    if (s < d + dst_bytes && d < s + src_bytes)
        return -2;
    // BGR通道顺序在采样时不变；采样中心为(3x+1,3y+1)，RGB转换留给C++预处理。
    for (size_t y = 0; y < 360; ++y) {
        const unsigned char *row = src + (3 * y + 1) * 1920 * 3;
        unsigned char *out = dst + y * 640 * 3;
        for (size_t x = 0; x < 640; ++x) {
            const size_t input = 9 * x + 3, output = 3 * x;
            out[output] = row[input];
            out[output + 1] = row[input + 1];
            out[output + 2] = row[input + 2];
        }
    }
    return 0;
}
