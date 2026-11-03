// 同步CPU MPEG4编码库，动态链接已有FFmpeg。
// 独占创建输出文件、持有独立AVFrame，关闭时排空编码包并写文件尾。

/* M134: owned MPEG4 frames and exclusive output, synchronous CPU encoder.
 * Dynamically links existing OpenCV FFmpeg libraries (build licenses apply); never opens hardware
 * codecs/devices.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
typedef struct {
    AVFormatContext *format;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    AVIOContext *io;
    struct SwsContext *sws;
    AVStream *stream;
    int fd, width, height, failed;
    int64_t count;
    int64_t last_pts_ms;
} Writer;
// FFmpeg的自定义AVIO写回调，循环处理短写并重试EINTR；写入失败保留负错误码。
static int output(void *opaque, uint8_t *buf, int size) {
    Writer *w = opaque;
    int done = 0;
    while (done < size) {
        ssize_t n = write(w->fd, buf + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return AVERROR(n == 0 ? EIO : errno);
        done += (int)n;
    }
    return done;
}
// MP4封装需要回写索引；支持查询文件大小并通过lseek定位，不把输出当作仅追加流。
static int64_t seek_output(void *opaque, int64_t off, int whence) {
    Writer *w = opaque;
    if (whence == AVSEEK_SIZE) {
        struct stat s;
        if (fstat(w->fd, &s) < 0)
            return AVERROR(errno);
        return s.st_size;
    }
    int64_t ret = lseek(w->fd, off, whence & ~AVSEEK_FORCE);
    return ret < 0 ? AVERROR(errno) : ret;
}
// 统一释放sws、packet、frame、codec、AVIO和文件描述符，供失败回退及正常关闭复用。
static void cleanup(Writer *w) {
    if (!w)
        return;
    if (w->sws)
        sws_freeContext(w->sws);
    av_packet_free(&w->packet);
    av_frame_free(&w->frame);
    avcodec_free_context(&w->codec);
    if (w->io) {
        av_freep(&w->io->buffer);
        avio_context_free(&w->io);
    }
    if (w->format) {
        w->format->pb = NULL;
        avformat_free_context(w->format);
    }
    if (w->fd >= 0)
        close(w->fd);
    free(w);
}
// 取出编码器已产生的数据包，换算到stream时间基后封装；每次写完解除packet引用。
static int drain(Writer *w, int flushing) {
    int r;
    while ((r = avcodec_receive_packet(w->codec, w->packet)) >= 0) {
        if (w->packet->duration <= 0)
            w->packet->duration = av_rescale_q(1, av_inv_q(w->codec->framerate), w->codec->time_base);
        av_packet_rescale_ts(w->packet, w->codec->time_base, w->stream->time_base);
        w->packet->stream_index = w->stream->index;
        r = av_interleaved_write_frame(w->format, w->packet);
        av_packet_unref(w->packet);
        if (r < 0)
            return r;
    }
    if (r == AVERROR_EOF)
        return flushing ? 0 : AVERROR(EIO);
    return r == AVERROR(EAGAIN) && !flushing ? 0 : r;
}
unsigned m134_codec_version(void) { return avcodec_version(); }
// 校验偶数几何、线程与FFmpeg ABI版本后创建编码器；失败通过status返回原因，句柄为NULL。
void *m134_open(const char *path, int width, int height, int fps_num, int fps_den, int threads,
                int *status) {
    *status = AVERROR(EINVAL);
    if (!path || !*path || width < 2 || height < 2 || width > 4096 || height > 4096 ||
        (width & 1) || (height & 1) || fps_num <= 0 || fps_den <= 0 || fps_num > 100000 ||
        fps_den > 10000 || threads < 1 || threads > 2)
        return NULL;
    if (avcodec_version() != AV_VERSION_INT(58, 134, 100) ||
        avformat_version() != AV_VERSION_INT(58, 76, 100) ||
        avutil_version() != AV_VERSION_INT(56, 70, 100) ||
        swscale_version() != AV_VERSION_INT(5, 9, 100)) {
        *status = AVERROR(ENOSYS);
        return NULL;
    }
    Writer *w = calloc(1, sizeof(*w));
    if (!w) {
        *status = AVERROR(ENOMEM);
        return NULL;
    }
    w->fd = -1;
    w->width = width;
    w->height = height;
    int r = avformat_alloc_output_context2(&w->format, NULL, "mp4", path);
    if (r < 0 || !w->format)
        goto fail;
    AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!encoder || !(encoder->capabilities & AV_CODEC_CAP_SLICE_THREADS)) {
        r = AVERROR(ENOSYS);
        goto fail;
    }
    w->stream = avformat_new_stream(w->format, NULL);
    w->codec = avcodec_alloc_context3(encoder);
    w->frame = av_frame_alloc();
    w->packet = av_packet_alloc();
    if (!w->stream || !w->codec || !w->frame || !w->packet) {
        r = AVERROR(ENOMEM);
        goto fail;
    }
    AVCodecContext *c = w->codec;
    c->codec_type = AVMEDIA_TYPE_VIDEO;
    c->codec_id = AV_CODEC_ID_MPEG4;
    c->width = width;
    c->height = height;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = (AVRational){1, 1000}; /* Millisecond timestamps; framerate remains nominal. */
    c->framerate = (AVRational){fps_num, fps_den};
    c->gop_size = 12;
    c->max_b_frames = 0;
    c->qmin = 2;
    c->qmax = 31;
    int64_t base = (int64_t)width * height * fps_num / fps_den;
    if (base > INT32_MAX / 2) {
        r = AVERROR(EINVAL);
        goto fail;
    }
    c->bit_rate = base + base / 2;
    c->bit_rate_tolerance = base * 2;
    c->thread_count = threads;
    c->thread_type = FF_THREAD_SLICE;
    if (w->format->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((r = avcodec_open2(c, encoder, NULL)) < 0)
        goto fail;
    if (c->thread_count != threads || (threads > 1 && !(c->active_thread_type & FF_THREAD_SLICE))) {
        r = AVERROR(ENOSYS);
        goto fail;
    }
    if ((r = avcodec_parameters_from_context(w->stream->codecpar, c)) < 0)
        goto fail;
    w->stream->time_base = c->time_base;
    w->stream->avg_frame_rate = c->framerate;
    w->stream->codecpar->codec_tag = MKTAG('m', 'p', '4', 'v');
    w->frame->format = c->pix_fmt;
    w->frame->width = width;
    w->frame->height = height;
    if ((r = av_frame_get_buffer(w->frame, 32)) < 0)
        goto fail;
    w->sws = sws_getContext(width, height, AV_PIX_FMT_BGR24, width, height, c->pix_fmt, SWS_BICUBIC,
                            NULL, NULL, NULL);
    if (!w->sws) {
        r = AVERROR(ENOMEM);
        goto fail;
    }
    w->fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (w->fd < 0) {
        r = AVERROR(errno);
        goto fail;
    }
    uint8_t *buffer = av_malloc(32768);
    if (!buffer) {
        r = AVERROR(ENOMEM);
        goto fail;
    }
    w->io = avio_alloc_context(buffer, 32768, 1, w, NULL, output, seek_output);
    if (!w->io) {
        av_free(buffer);
        r = AVERROR(ENOMEM);
        goto fail;
    }
    w->io->seekable = AVIO_SEEKABLE_NORMAL;
    w->format->pb = w->io;
    w->format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if ((r = avformat_write_header(w->format, NULL)) < 0)
        goto fail;
    *status = 0;
    return w;
fail:
    *status = r < 0 ? r : AVERROR(ENOMEM);
    cleanup(w);
    return NULL;
}

/* Limited-range BT.601 with 2x2 averaged chroma. Each frame remains owned by
 * FFmpeg. ARM NEON and scalar paths use identical integer arithmetic. */
#if defined(__aarch64__)
#include <arm_neon.h>
static uint8x8_t luma8(uint8x8x3_t a) {
    uint16x8_t y = vmull_u8(a.val[0], vdup_n_u8(25));
    y = vmlal_u8(y, a.val[1], vdup_n_u8(129));
    y = vmlal_u8(y, a.val[2], vdup_n_u8(66));
    return vadd_u8(vshrn_n_u16(vaddq_u16(y, vdupq_n_u16(128)), 8), vdup_n_u8(16));
}
static int16x4_t average4(uint8x8_t a, uint8x8_t b) {
    uint16x4_t sum = vadd_u16(vpaddl_u8(a), vpaddl_u8(b));
    return vreinterpret_s16_u16(vshr_n_u16(vadd_u16(sum, vdup_n_u16(2)), 2));
}
static void chroma4(uint8_t *u, uint8_t *v, uint8x8x3_t a, uint8x8x3_t b) {
    int16x4_t bb = average4(a.val[0], b.val[0]), gg = average4(a.val[1], b.val[1]),
              rr = average4(a.val[2], b.val[2]);
    int16x4_t us = vmul_n_s16(bb, 112);
    us = vmla_n_s16(us, gg, -74);
    us = vmla_n_s16(us, rr, -38);
    int16x4_t vs = vmul_n_s16(rr, 112);
    vs = vmla_n_s16(vs, gg, -94);
    vs = vmla_n_s16(vs, bb, -18);
    uint16x4_t uu = vshr_n_u16(vadd_u16(vreinterpret_u16_s16(us), vdup_n_u16(32896)), 8);
    uint16x4_t vv = vshr_n_u16(vadd_u16(vreinterpret_u16_s16(vs), vdup_n_u16(32896)), 8);
    uint8_t ub[8], vb[8];
    vst1_u8(ub, vmovn_u16(vcombine_u16(uu, uu)));
    vst1_u8(vb, vmovn_u16(vcombine_u16(vv, vv)));
    memcpy(u, ub, 4);
    memcpy(v, vb, 4);
}
#endif
// 按2×2块将BGR变换为YUV420，色度做四像素平均；NEON与标量尾部遵循相同整数公式。
static void convert_box(const uint8_t *bgr, int width, int height, int stride, uint8_t *yp, int ys,
                        uint8_t *up, int us, uint8_t *vp, int vs, int scalar) {
#if !defined(__aarch64__)
    (void)scalar;
#endif
    for (int row = 0; row < height; row += 2) {
        const uint8_t *a = bgr + (size_t)row * stride, *b = a + stride;
        uint8_t *y0 = yp + (size_t)row * ys, *y1 = y0 + ys, *u = up + (size_t)(row / 2) * us,
                *v = vp + (size_t)(row / 2) * vs;
        int x = 0;
#if defined(__aarch64__)
        if (!scalar)
            for (; x + 8 <= width; x += 8) {
                uint8x8x3_t aa = vld3_u8(a + 3 * x), bb = vld3_u8(b + 3 * x);
                vst1_u8(y0 + x, luma8(aa));
                vst1_u8(y1 + x, luma8(bb));
                chroma4(u + x / 2, v + x / 2, aa, bb);
            }
#endif
        for (; x < width; x += 2) {
            int blue = 0, green = 0, red = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t *p = (dy ? b : a) + 3 * (x + dx);
                    (dy ? y1 : y0)[x + dx] =
                        (uint8_t)(16 + ((25 * p[0] + 129 * p[1] + 66 * p[2] + 128) >> 8));
                    blue += p[0];
                    green += p[1];
                    red += p[2];
                }
            blue = (blue + 2) >> 2;
            green = (green + 2) >> 2;
            red = (red + 2) >> 2;
            u[x / 2] = (uint8_t)((112 * blue - 74 * green - 38 * red + 32896) >> 8);
            v[x / 2] = (uint8_t)((112 * red - 94 * green - 18 * blue + 32896) >> 8);
        }
    }
}
// 纯像素转换入口，用于核对字节合同；输出为Y平面后接U、V，大小width×height×3/2。
int m388_convert(const uint8_t *bgr, size_t bytes, int width, int height, int stride, uint8_t *out,
                 size_t out_bytes, int scalar) {
    if (!bgr || !out || width < 2 || height < 2 || width > 4096 || height > 4096 || (width & 1) ||
        (height & 1) || stride != width * 3 || bytes != (size_t)stride * height ||
        out_bytes != (size_t)width * height * 3 / 2 || (scalar != 0 && scalar != 1))
        return AVERROR(EINVAL);
    uint8_t *up = out + (size_t)width * height, *vp = up + (size_t)width * height / 4;
    convert_box(bgr, width, height, stride, out, width, up, width / 2, vp, width / 2, scalar);
    return 0;
}

// 以调用方给出的毫秒PTS写入，要求严格递增；视频间隔来自实际源时间，不由处理次数补齐。
int m26_write_at_ms(void *opaque, const uint8_t *bgr, size_t bytes, int stride, int64_t pts_ms) {
    Writer *w = opaque;
    if (!w || !bgr || stride != w->width * 3 || bytes != (size_t)stride * w->height || w->failed ||
        pts_ms < 0 || (w->count && pts_ms <= w->last_pts_ms))
        return AVERROR(EINVAL);
    int r = av_frame_make_writable(w->frame);
    if (r < 0)
        goto fail;
    convert_box(bgr, w->width, w->height, stride, w->frame->data[0], w->frame->linesize[0],
                w->frame->data[1], w->frame->linesize[1], w->frame->data[2], w->frame->linesize[2],
                0);
    w->frame->pts = pts_ms;
    if ((r = avcodec_send_frame(w->codec, w->frame)) < 0)
        goto fail;
    if ((r = drain(w, 0)) < 0)
        goto fail;
    w->last_pts_ms = pts_ms;
    w->count++;
    return 0;
fail:
    w->failed = 1;
    return r;
}
/* Retain the existing CFR entry point for source/API compatibility. */
// 保留固定帧率C ABI，将帧序号转换成当前编码时间基后调用带PTS入口。
int m134_write(void *opaque, const uint8_t *bgr, size_t bytes, int stride) {
    Writer *w = opaque;
    if (!w) return AVERROR(EINVAL);
    int64_t pts = av_rescale_q(w->count, av_inv_q(w->codec->framerate), w->codec->time_base);
    return m26_write_at_ms(opaque, bgr, bytes, stride, pts);
}
// 先发送空帧排空编码器，再写MP4尾部和flush；任何失败都会返回调用者，最后统一释放。
int m134_close(void *opaque) {
    Writer *w = opaque;
    if (!w)
        return 0;
    int r = w->failed ? AVERROR(EIO) : 0;
    if (!r) {
        r = avcodec_send_frame(w->codec, NULL);
        if (r >= 0)
            r = drain(w, 1);
    }
    if (r >= 0)
        r = av_write_trailer(w->format);
    if (w->io) {
        avio_flush(w->io);
        if (r >= 0 && w->io->error < 0)
            r = w->io->error;
    }
    cleanup(w);
    return r;
}
