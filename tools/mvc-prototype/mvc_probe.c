/* 直接把 merged.264 按 AUD(NAL 9) 切成 AU，一 AU 一个 packet 送进解码器 ——
 * 绕开 libavformat 的 h264 parser（它会丢掉 NAL 15/20，正是 Kodi 用 noparse 的原因）。
 * 输出 AU 内两帧的 Y 平面，供外部比较是否不同视角。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

static unsigned char *readall(const char *p, long *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n); if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

int main(int argc, char **argv)
{
    long n = 0;
    unsigned char *buf = readall(argc > 1 ? argv[1] : "merged.264", &n);
    if (!buf) { fprintf(stderr, "读文件失败\n"); return 1; }
    int both = argc > 2 ? atoi(argv[2]) : 1;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    c->thread_count = 0;
    if (both) {
        int ids[2] = {0, 1};
        printf("  set view_ids={0,1} -> %d\n",
               av_opt_set_array(c, "view_ids", AV_OPT_SEARCH_CHILDREN, 0, 2, AV_OPT_TYPE_INT, ids));
    }
    if (avcodec_open2(c, codec, NULL) < 0) { fprintf(stderr, "open2 失败\n"); return 1; }

    /* 找所有 AUD 起点，按 AU 送包 */
    long *starts = malloc(sizeof(long) * (n / 4 + 2)); int ns = 0;
    for (long i = 0; i + 3 < n; i++) {
        long sc = -1;
        if (i + 4 < n && !memcmp(buf + i, "\x00\x00\x00\x01", 4)) sc = i + 4;
        else if (!memcmp(buf + i, "\x00\x00\x01", 3)) sc = i + 3;
        if (sc > 0 && sc < n && (buf[sc] & 0x1F) == 9) starts[ns++] = i;
    }
    printf("  AU（AUD）数: %d\n", ns);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *f = av_frame_alloc();
    int nf = 0, dumped = 0;
    for (int k = 0; k < ns; k++) {
        long s = starts[k], e = (k + 1 < ns) ? starts[k + 1] : n;
        av_new_packet(pkt, (int)(e - s));
        memcpy(pkt->data, buf + s, (size_t)(e - s));
        if (avcodec_send_packet(c, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(c, f) >= 0) {
            if (nf < 4) printf("  帧 %d: %dx%d\n", nf, f->width, f->height);
            if (nf >= 120 && nf < 122) {
                char name[96]; snprintf(name, sizeof name, "au_%d.y", nf);
                FILE *o = fopen(name, "wb");
                if (o) { for (int y = 0; y < f->height; y++) fwrite(f->data[0] + (size_t)y * f->linesize[0], 1, f->width, o); fclose(o); }
                dumped++;
            }
            nf++; av_frame_unref(f);
        }
    }
    avcodec_send_packet(c, NULL);
    while (avcodec_receive_frame(c, f) >= 0) { nf++; av_frame_unref(f); }
    printf("  帧数 %d（%.2f 帧/AU），导出 %d 帧 Y 平面\n", nf, ns ? (double)nf / ns : 0.0, dumped);
    return 0;
}
