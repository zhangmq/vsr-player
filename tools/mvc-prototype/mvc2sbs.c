/* MVC 双视图 → SBS 全宽 rawvideo（yuv420p），写 stdout。
 * 解码用打过 MVC 补丁的 libavcodec（view_ids={0,1}）：每个 AU 输出两帧
 * （先 base 后 dependent），本程序把同 AU 的两帧并排写成 2W×H 的一帧。
 * 用法: mvc2sbs <merged.264> > out.yuv   （帧率/尺寸由调用方告知 ffmpeg） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

static unsigned char *readall(const char *p, long *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n);
    if (!b || fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <merged.264> > out.yuv\n", argv[0]); return 2; }
    long n = 0;
    unsigned char *buf = readall(argv[1], &n);
    if (!buf) { fprintf(stderr, "读失败\n"); return 1; }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->thread_count = 0;
    int ids[2] = {0, 1};
    if (av_opt_set_array(c, "view_ids", AV_OPT_SEARCH_CHILDREN, 0, 2, AV_OPT_TYPE_INT, ids) < 0) {
        fprintf(stderr, "view_ids 设置失败（解码器无 MVC 支持？）\n"); return 1;
    }
    if (avcodec_open2(c, codec, NULL) < 0) { fprintf(stderr, "open2 失败\n"); return 1; }

    /* 按 AUD(NAL 9) 切 AU */
    long *starts = malloc(sizeof(long) * (n / 4 + 2)); int ns = 0;
    for (long i = 0; i + 3 < n; ) {
        long sc = -1;
        if (i + 4 < n && !memcmp(buf + i, "\x00\x00\x00\x01", 4)) sc = i + 4;
        else if (!memcmp(buf + i, "\x00\x00\x01", 3)) sc = i + 3;
        if (sc < 0) { i++; continue; }
        if ((buf[sc] & 0x1F) == 9) starts[ns++] = i;
        i = sc;
    }
    fprintf(stderr, "AU: %d\n", ns);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    AVFrame *base = av_frame_alloc();
    unsigned char *sbs = NULL;
    int written = 0, pending = 0;
    for (int k = 0; k < ns; k++) {
        long s = starts[k], e = (k + 1 < ns) ? starts[k + 1] : n;
        av_new_packet(pkt, (int)(e - s));
        memcpy(pkt->data, buf + s, (size_t)(e - s));
        if (avcodec_send_packet(c, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, f) >= 0) {
            if (!pending) { av_frame_ref(base, f); pending = 1; }   /* 视图 0 */
            else {                                                   /* 视图 1（同 AU） */
                const int W = base->width, H = base->height;
                const int W2 = W * 2;
                /* 两帧尺寸必须一致（MVC 要求），否则跳过 */
                if (f->width != W || f->height != H) { fprintf(stderr, "视图尺寸不一致，跳过\n"); pending = 0; av_frame_unref(base); av_frame_unref(f); continue; }
                free(sbs);
                sbs = malloc((size_t)W2 * H * 3 / 2);
                unsigned char *d = sbs;
                for (int y = 0; y < H; y++, d += W2) {
                    memcpy(d,          base->data[0] + (size_t)y * base->linesize[0], W);
                    memcpy(d + W,      f->data[0]     + (size_t)y * f->linesize[0],     W);
                }
                for (int y = 0; y < H / 2; y++, d += W2) {
                    memcpy(d,          base->data[1] + (size_t)y * base->linesize[1], W / 2);
                    memcpy(d + W / 2,  f->data[1]     + (size_t)y * f->linesize[1],     W / 2);
                    memcpy(d + W,      base->data[2] + (size_t)y * base->linesize[2], W / 2);
                    memcpy(d + W + W/2, f->data[2]    + (size_t)y * f->linesize[2],     W / 2);
                }
                fwrite(sbs, 1, (size_t)W2 * H * 3 / 2, stdout);
                written++;
                pending = 0;
            }
            av_frame_unref(f);
        }
    }
    free(sbs); free(starts); free(buf);
    fprintf(stderr, "输出 SBS 帧: %d\n", written);
    return written > 0 ? 0 : 1;
}
