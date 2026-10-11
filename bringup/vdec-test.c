// Stateful V4L2 decode test for the MediaTek vendor decoder (mtk-vcodec-dec-v1).
// usage: vdec-test FILE.h264 [heap] [dump.raw]
// Feeds one Annex-B access unit per OUTPUT buffer. Both queues use dmabufs from
// /dev/dma_heap/<heap> (default mtk_mm), like Android's codec service: with
// plain MMAP buffers the driver has no address for the VCP firmware.
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NOUT 8
#define OUT_SIZE (2 * 1024 * 1024)
#define MAXCAP 32

static int vfd, heap, out_mmap; // VDEC_OUT_MMAP=1: MMAP bitstream buffers (Chromium-style)
static uint8_t *bs; static size_t bs_len, bs_pos;
static int out_fd[NOUT]; static uint8_t *out_map[NOUT];
static int cap_fd[MAXCAP][2]; static int ncap, cap_planes; static size_t cap_size[2];

static void die(const char *what) { perror(what); exit(1); }
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int heap_alloc(size_t len) {
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) die("DMA_HEAP_IOCTL_ALLOC");
    return a.fd;
}

// Next NAL start (after a 00 00 01 start code) at or after pos; returns code offset.
static size_t next_start(size_t pos) {
    for (size_t i = pos; i + 3 <= bs_len; i++)
        if (bs[i] == 0 && bs[i + 1] == 0 && bs[i + 2] == 1) return i;
    return bs_len;
}

// One access unit: leading non-VCL NALs plus VCL NALs of one picture.
static size_t next_au(size_t *start) {
    size_t s = next_start(bs_pos), p = s, end = bs_len;
    int seen_vcl = 0;
    if (s >= bs_len) return 0;
    while (p < bs_len) {
        size_t hdr = p + 3;
        if (hdr >= bs_len) break;
        int type = bs[hdr] & 0x1f;
        int vcl = type == 1 || type == 5;
        int first_mb0 = vcl && hdr + 1 < bs_len && (bs[hdr + 1] & 0x80); // ue(v)=0 -> leading '1'
        if (seen_vcl && (!vcl || first_mb0)) { end = p; if (p > 0 && bs[p - 1] == 0) end = p - 1; break; }
        if (vcl) seen_vcl = 1;
        p = next_start(hdr);
    }
    if (s > 0 && bs[s - 1] == 0) s--;
    *start = s; bs_pos = end;
    return end - s;
}

static int queue_out(int i) {
    size_t s, n = next_au(&s);
    struct v4l2_plane pl = {0};
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                             .memory = out_mmap ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF,
                             .index = i, .length = 1, .m.planes = &pl };
    if (n > OUT_SIZE) { fprintf(stderr, "AU too large %zu\n", n); exit(1); }
    if (n) memcpy(out_map[i], bs + s, n);
    else b.flags = V4L2_BUF_FLAG_LAST;
    if (!out_mmap) pl.m.fd = out_fd[i];
    pl.length = OUT_SIZE; pl.bytesused = n;
    static long ts; b.timestamp.tv_usec = ++ts;
    if (ioctl(vfd, VIDIOC_QBUF, &b) < 0) die("QBUF out");
    return n != 0;
}

static void setup_capture(void) {
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    if (ioctl(vfd, VIDIOC_G_FMT, &f) < 0) die("G_FMT cap");
    struct v4l2_pix_format_mplane *m = &f.fmt.pix_mp;
    cap_planes = m->num_planes;
    printf("capture %ux%u fourcc %.4s planes %u", m->width, m->height, (char *)&m->pixelformat, m->num_planes);
    for (int p = 0; p < m->num_planes; p++) { cap_size[p] = m->plane_fmt[p].sizeimage; printf(" [%zu bpl %u]", cap_size[p], m->plane_fmt[p].bytesperline); }
    struct v4l2_control c = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    ncap = ioctl(vfd, VIDIOC_G_CTRL, &c) == 0 ? c.value + 4 : 12;
    if (ncap > MAXCAP) ncap = MAXCAP;
    printf(" min+4=%d\n", ncap);
    struct v4l2_requestbuffers r = { .count = ncap, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF };
    if (ioctl(vfd, VIDIOC_REQBUFS, &r) < 0) die("REQBUFS cap");
    ncap = r.count;
    for (int i = 0; i < ncap; i++) {
        struct v4l2_plane pl[2] = {0};
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
                                 .index = i, .length = cap_planes, .m.planes = pl };
        for (int p = 0; p < cap_planes; p++) { cap_fd[i][p] = heap_alloc(cap_size[p]); pl[p].m.fd = cap_fd[i][p]; pl[p].length = cap_size[p]; }
        if (ioctl(vfd, VIDIOC_QBUF, &b) < 0) die("QBUF cap");
    }
    int t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(vfd, VIDIOC_STREAMON, &t) < 0) die("STREAMON cap");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: vdec-test FILE.h264 [heap] [dump.raw]\n"); return 2; }
    const char *heapname = argc > 2 ? argv[2] : "mtk_mm";
    FILE *dump = argc > 3 ? fopen(argv[3], "wb") : NULL;
    int ffd = open(argv[1], O_RDONLY); struct stat st;
    if (ffd < 0 || fstat(ffd, &st) < 0) die("input");
    bs_len = st.st_size; bs = mmap(NULL, bs_len, PROT_READ, MAP_PRIVATE, ffd, 0);
    char hp[64]; snprintf(hp, sizeof hp, "/dev/dma_heap/%s", heapname);
    if ((heap = open(hp, O_RDWR | O_CLOEXEC)) < 0) die(hp);
    if ((vfd = open("/dev/video0", O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) die("/dev/video0");

    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    if (ioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) die("SUBSCRIBE source change");
    sub.type = V4L2_EVENT_EOS; ioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264; f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.width = 1920; f.fmt.pix_mp.height = 1088; f.fmt.pix_mp.plane_fmt[0].sizeimage = OUT_SIZE;
    if (ioctl(vfd, VIDIOC_S_FMT, &f) < 0) die("S_FMT out");
    out_mmap = getenv("VDEC_OUT_MMAP") && atoi(getenv("VDEC_OUT_MMAP"));
    struct v4l2_requestbuffers r = { .count = NOUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                     .memory = out_mmap ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF };
    if (ioctl(vfd, VIDIOC_REQBUFS, &r) < 0) die("REQBUFS out");
    for (int i = 0; i < NOUT; i++) {
        if (out_mmap) {
            struct v4l2_plane qp = {0};
            struct v4l2_buffer qb = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
                                      .index = i, .length = 1, .m.planes = &qp };
            if (ioctl(vfd, VIDIOC_QUERYBUF, &qb) < 0) die("QUERYBUF out");
            out_map[i] = mmap(NULL, qp.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, qp.m.mem_offset);
        } else {
            out_fd[i] = heap_alloc(OUT_SIZE);
            out_map[i] = mmap(NULL, OUT_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd[i], 0);
        }
        if (out_map[i] == MAP_FAILED) die("mmap out");
    }
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(vfd, VIDIOC_STREAMON, &t) < 0) die("STREAMON out");
    int more = 1;
    for (int i = 0; i < NOUT && more; i++) more = queue_out(i);

    int frames = 0, capturing = 0; double t0 = now(), tfirst = 0;
    while (1) {
        struct pollfd pfd = { .fd = vfd, .events = POLLIN | POLLOUT | POLLPRI };
        int pr = poll(&pfd, 1, 3000);
        if (pr == 0) { fprintf(stderr, "timeout after %d frames\n", frames); break; }
        if (pfd.revents & POLLPRI) {
            struct v4l2_event ev;
            while (ioctl(vfd, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !capturing) { setup_capture(); capturing = 1; }
                else if (ev.type == V4L2_EVENT_EOS) printf("EOS event\n");
            }
        }
        struct v4l2_plane pl[2] = {0};
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                 .memory = out_mmap ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF, .length = 1, .m.planes = pl };
        while (ioctl(vfd, VIDIOC_DQBUF, &b) == 0)
            if (more) more = queue_out(b.index);
        if (!capturing) continue;
        struct v4l2_buffer c = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF, .length = cap_planes, .m.planes = pl };
        int last = 0;
        while (ioctl(vfd, VIDIOC_DQBUF, &c) == 0) {
            if (c.m.planes[0].bytesused) {
                if (!frames) tfirst = now();
                frames++;
                if (dump && frames == 30) {
                    for (int p = 0; p < cap_planes; p++) {
                        void *m = mmap(NULL, cap_size[p], PROT_READ, MAP_SHARED, cap_fd[c.index][p], 0);
                        if (m != MAP_FAILED) { fwrite(m, 1, c.m.planes[p].bytesused ? c.m.planes[p].bytesused : cap_size[p], dump); munmap(m, cap_size[p]); }
                    }
                    fclose(dump); dump = NULL;
                }
            }
            if (c.flags & V4L2_BUF_FLAG_LAST) { last = 1; break; }
            for (int p = 0; p < cap_planes; p++) { pl[p].m.fd = cap_fd[c.index][p]; pl[p].length = cap_size[p]; }
            if (ioctl(vfd, VIDIOC_QBUF, &c) < 0) die("re-QBUF cap");
        }
        if (last) break;
    }
    double t1 = now();
    printf("decoded %d frames in %.3f s (first frame after %.3f s): %.1f fps\n", frames, t1 - t0,
           tfirst ? tfirst - t0 : 0, frames > 1 ? (frames - 1) / (t1 - tfirst) : 0);
    return frames ? 0 : 1;
}
