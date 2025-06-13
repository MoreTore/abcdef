/*
 * rot_test.c – minimal Qualcomm SDM845 SDE-rotator demo (UBWC → linear)
 *
 * Builds:
 *   arm64-linux-gnu-gcc -Wall -O2 -o rot_test rot_test.c
 *
 * Typical run (root):
 *   ./rot_test /dev/video2 1920 1080
 *
 * Dependencies:
 *   • your kernel headers (for <media/msm_sde_rotator.h> and ION UAPI)
 *   • ion_helpers.c  – provides ion_alloc_contig_fd()
 *   • sys_heap_mask.h – defines SYS_HEAP_MASK  (contiguous heap bitmask)
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/videodev2.h>
#include <media/msm_sde_rotator.h>
#include <linux/ion.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../video.h"
#include "rot_test.h"

/* ─────────────── helpers ─────────────── */
#define CHECK(x, msg)  do { if ((x) < 0) { perror(msg); exit(1);} } while (0)
#define CLEAR(s)       memset(&(s), 0, sizeof(s))

static int xioctl(int fd, int req, void *arg, const char *tag)
{
    int r; do { r = ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
    if (r < 0) fprintf(stderr, "%s: %s\n", tag, strerror(errno));
    return r;
}

/* ─────────────── main ─────────────── */
int test_rot(int argc, char **argv)
{
    const char *node = (argc > 1) ? argv[1] : "/dev/video2";
    unsigned     W   = (argc > 2) ? atoi(argv[2]) : 1920;
    unsigned     H   = (argc > 3) ? atoi(argv[3]) : 1080;

    int fd = open(node, O_RDWR | O_NONBLOCK);
    CHECK(fd, "open rotator");

    /* 1. OUTPUT queue – NV12_UBWC (4 planes) */
    struct v4l2_format fmt_out; CLEAR(fmt_out);
    fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt_out.fmt.pix_mp.width       = W;
    fmt_out.fmt.pix_mp.height      = H;
    fmt_out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
    fmt_out.fmt.pix_mp.num_planes  = 4;                 /* Y, Ymeta, UV, UVmeta */
    CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt_out, "S_FMT OUT"), "S_FMT OUT");

    /* plane sizes returned by driver */
    size_t y_sz      = fmt_out.fmt.pix_mp.plane_fmt[0].sizeimage;
    size_t ymeta_sz  = fmt_out.fmt.pix_mp.plane_fmt[1].sizeimage;
    size_t uv_sz     = fmt_out.fmt.pix_mp.plane_fmt[2].sizeimage;
    size_t uvmeta_sz = fmt_out.fmt.pix_mp.plane_fmt[3].sizeimage;

    /* 2. CAPTURE queue – linear NV12 (2 planes) */
    struct v4l2_format fmt_cap; CLEAR(fmt_cap);
    fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt_cap.fmt.pix_mp.width       = W;
    fmt_cap.fmt.pix_mp.height      = H;
    fmt_cap.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt_cap.fmt.pix_mp.num_planes  = 2;                 /* Y, UV */
    CHECK(xioctl(fd, VIDIOC_S_FMT, &fmt_cap, "S_FMT CAP"), "S_FMT CAP");

    size_t y_lin_sz  = fmt_cap.fmt.pix_mp.plane_fmt[0].sizeimage;
    size_t uv_lin_sz = fmt_cap.fmt.pix_mp.plane_fmt[1].sizeimage;

    /* 3. ROTATE control = 0° (optional) */
    struct v4l2_control rot = { .id = V4L2_CID_ROTATE, .value = 0 };
    CHECK(xioctl(fd, VIDIOC_S_CTRL, &rot, "CTRL ROT"), "CTRL ROT");

    /* 4. REQBUFS (both DMABUF) */
    struct v4l2_requestbuffers req = { .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_DMABUF };
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS OUT"), "REQBUFS OUT");

    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(fd, VIDIOC_REQBUFS, &req, "REQBUFS CAP"), "REQBUFS CAP");

    /* 5. allocate ION planes */
    int fd_y  = alloc_ion_buffer(y_sz,      0);
    int fd_ym = alloc_ion_buffer(ymeta_sz,  0);
    int fd_uv = alloc_ion_buffer(uv_sz,     0);
    int fd_uvm= alloc_ion_buffer(uvmeta_sz, 0);

    int fd_y_lin  = alloc_ion_buffer(y_lin_sz,  0);
    int fd_uv_lin = alloc_ion_buffer(uv_lin_sz, 0);

    /* 6. QBUF OUT (4 planes) */
    struct v4l2_buffer  buf_out;  CLEAR(buf_out);
    struct v4l2_plane   planes_o[4]; CLEAR(planes_o);
    buf_out.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf_out.memory    = V4L2_MEMORY_DMABUF;
    buf_out.index     = 0;
    buf_out.length    = 4;
    buf_out.m.planes  = planes_o;

    planes_o[0].m.fd = fd_y;   planes_o[0].length = y_sz;      planes_o[0].bytesused = y_sz;
    planes_o[1].m.fd = fd_ym;  planes_o[1].length = ymeta_sz;  planes_o[1].bytesused = ymeta_sz;
    planes_o[2].m.fd = fd_uv;  planes_o[2].length = uv_sz;     planes_o[2].bytesused = uv_sz;
    planes_o[3].m.fd = fd_uvm; planes_o[3].length = uvmeta_sz; planes_o[3].bytesused = uvmeta_sz;

    CHECK(xioctl(fd, VIDIOC_QBUF, &buf_out, "QBUF OUT"), "QBUF OUT");

    /* 7. QBUF CAP (2 planes) */
    struct v4l2_buffer buf_cap; CLEAR(buf_cap);
    struct v4l2_plane  planes_c[2]; CLEAR(planes_c);
    buf_cap.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf_cap.memory    = V4L2_MEMORY_DMABUF;
    buf_cap.index     = 0;
    buf_cap.length    = 2;
    buf_cap.m.planes  = planes_c;

    planes_c[0].m.fd = fd_y_lin;  planes_c[0].length = y_lin_sz;
    planes_c[1].m.fd = fd_uv_lin; planes_c[1].length = uv_lin_sz;

    CHECK(xioctl(fd, VIDIOC_QBUF, &buf_cap, "QBUF CAP"), "QBUF CAP");

    /* 8. STREAMON both queues */
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    CHECK(xioctl(fd, VIDIOC_STREAMON, &t, "STREAMON OUT"), "STREAMON OUT");
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    CHECK(xioctl(fd, VIDIOC_STREAMON, &t, "STREAMON CAP"), "STREAMON CAP");

    /* 9. Wait for completion (DQBUF CAP) */
    fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
    select(fd+1, &set, NULL, NULL, NULL);

    CLEAR(buf_cap);
    buf_cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf_cap.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_DQBUF, &buf_cap, "DQBUF CAP") == 0)
        printf("Success! Y bytes=%u  UV bytes=%u\n",
               buf_cap.m.planes[0].bytesused, buf_cap.m.planes[1].bytesused);

    /* 10. STREAMOFF + cleanup */
    t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd, VIDIOC_STREAMOFF, &t);

    close(fd_y); close(fd_ym); close(fd_uv); close(fd_uvm);
    close(fd_y_lin); close(fd_uv_lin);
    close(fd);
    return 0;
}