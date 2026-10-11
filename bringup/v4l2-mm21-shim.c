// LD_PRELOAD shim: present the MediaTek vendor decoder's 'M21S' capture format
// as the standard 'MM21' (same 16x32/16x16 tiled NV12 layout, verified by
// detiling a decoded frame), so Chromium's V4L2 decoder accepts the device.
// Only touches V4L2 format ioctls on capture queues.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <linux/videodev2.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>

#define FOURCC_M21S v4l2_fourcc('M', '2', '1', 'S')

static int (*real_ioctl)(int, unsigned long, ...);

static int is_capture(__u32 type) {
    return type == V4L2_BUF_TYPE_VIDEO_CAPTURE || type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

static void to_vendor(struct v4l2_format *f) {
    if (!is_capture(f->type)) return;
    if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && f->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_MM21)
        f->fmt.pix_mp.pixelformat = FOURCC_M21S;
    else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && f->fmt.pix.pixelformat == V4L2_PIX_FMT_MM21)
        f->fmt.pix.pixelformat = FOURCC_M21S;
}

static void to_standard(struct v4l2_format *f) {
    if (!is_capture(f->type)) return;
    if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE && f->fmt.pix_mp.pixelformat == FOURCC_M21S)
        f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MM21;
    else if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE && f->fmt.pix.pixelformat == FOURCC_M21S)
        f->fmt.pix.pixelformat = V4L2_PIX_FMT_MM21;
}

int ioctl(int fd, unsigned long request_arg, ...) {
    va_list ap;
    va_start(ap, request_arg);
    // Chromium passes requests as a signed int, so e.g. VIDIOC_ENUM_FMT arrives
    // sign-extended; the kernel only looks at the low 32 bits, and so do we.
    unsigned int request = (unsigned int)request_arg;
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");

    if (request == VIDIOC_S_FMT || request == VIDIOC_TRY_FMT) to_vendor(arg);
    if (request == VIDIOC_ENUM_FRAMESIZES && ((struct v4l2_frmsizeenum *)arg)->pixel_format == V4L2_PIX_FMT_MM21)
        ((struct v4l2_frmsizeenum *)arg)->pixel_format = FOURCC_M21S;
    int ret = real_ioctl(fd, request_arg, arg);
    if (request == VIDIOC_S_FMT || request == VIDIOC_TRY_FMT || request == VIDIOC_G_FMT) {
        to_standard(arg);
    } else if (request == VIDIOC_ENUM_FMT && ret == 0) {
        struct v4l2_fmtdesc *d = arg;
        if (is_capture(d->type) && d->pixelformat == FOURCC_M21S) {
            d->pixelformat = V4L2_PIX_FMT_MM21;
            d->flags &= ~V4L2_FMT_FLAG_COMPRESSED;
            strncpy((char *)d->description, "MM21 (vendor M21S)", sizeof(d->description));
        }
    } else if (request == VIDIOC_ENUM_FRAMESIZES && ret == 0) {
        struct v4l2_frmsizeenum *s = arg;
        if (s->pixel_format == FOURCC_M21S) s->pixel_format = V4L2_PIX_FMT_MM21;
    }
    return ret;
}
