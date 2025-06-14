#include <fcntl.h>  
#include <unistd.h>  
#include <sys/ioctl.h>  
#include <linux/videodev2.h>  
#include <sys/mman.h>  
#include <errno.h>  
#include <string.h>  
#include <stdlib.h> 
#include <assert.h>
#include <poll.h>
#include "common.h"
#include "video.h"


#define DBG_TAG "rot_test"
  
int convert_ubwc_to_linear(unsigned long out_buf_fd, unsigned char *ubwc_data, size_t ubwc_size,  
                          int width, int height,  
                          unsigned char **linear_data, size_t *linear_size)  
{  
    int rotator_fd = -1;  
    int ret = -1;  
    struct v4l2_format fmt_cap = {0}, fmt_out = {0};  
    struct v4l2_requestbuffers req_in = {0}, req_out = {0};  
    struct v4l2_buffer buf_in = {0}, buf_out = {0};  
    void *mapped_in = NULL, *mapped_out = NULL;  
      
    // Open SDE rotator device  
    rotator_fd = open("/dev/video2", O_RDWR);
    if (rotator_fd < 0) {  
        err("Failed to open rotator device");  
        return -1;  
    }

    // VIDIOC_QUERYCAP to check capabilities
    struct v4l2_capability cap;
    memzero(cap);
    if (ioctl(rotator_fd, VIDIOC_QUERYCAP, &cap) < 0) {
		err("Failed to verify capabilities: %m");
		return -1;
	}
	dbg("caps (/dev/video2): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
	    "version=%u.%u.%u",  cap.driver, cap.bus_info, cap.card,
	    (cap.version >> 16) & 0xff,
	    (cap.version >> 8) & 0xff,
	    cap.version & 0xff);

    fmt_out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt_out.fmt.pix.width       = 1952;      // e.g. 1952
    fmt_out.fmt.pix.height      = 1216;      // e.g. 1216
    fmt_out.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12_UBWC;  // the UBWC fourcc
    fmt_out.fmt.pix.field       = V4L2_FIELD_NONE;
    ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_out);

    fmt_cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt_cap.fmt.pix.width       = 1952;
    fmt_cap.fmt.pix.height      = 1216;
    fmt_cap.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;      // plain NV12
    fmt_cap.fmt.pix.field       = V4L2_FIELD_NONE;
    ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_cap);

    struct v4l2_requestbuffers req = {0};
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ioctl(rotator_fd, VIDIOC_REQBUFS, &req);

    struct v4l2_buffer buf = {0};
    info("ion buffer fd: %i", out_buf_fd);
    int sizeimage = fmt_out.fmt.pix.sizeimage;  // size of the output buffer
    buf.type      = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory    = V4L2_MEMORY_USERPTR;
    buf.index     = 0;
    buf.m.userptr = (unsigned long)out_buf_fd;
    buf.length    = sizeimage;               // use fmt_out.fmt.pix.sizeimage
    ioctl(rotator_fd, VIDIOC_QBUF, &buf);


    // allocate capture ion buffer
    size_t cap_size = fmt_cap.fmt.pix.sizeimage;
    int cap_ion_fd = alloc_ion_buffer(cap_size, 0);
    if (cap_ion_fd < 0) {
        err("Failed to allocate ION buffer for capture");
        return -1;
    }

    struct v4l2_requestbuffers req_cap = {0};
    req_cap.count  = 1;
    req_cap.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req_cap.memory = V4L2_MEMORY_USERPTR;
    ioctl(rotator_fd, VIDIOC_REQBUFS, &req_cap);

    struct v4l2_buffer cap_buf = {0};
    cap_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_buf.memory = V4L2_MEMORY_USERPTR;
    cap_buf.index  = 0;
    cap_buf.m.userptr = (unsigned long)cap_ion_fd;  // use the ION fd
    ioctl(rotator_fd, VIDIOC_QUERYBUF, &cap_buf);
    cap_buf.m.userptr = (unsigned long)cap_ion_fd;  // use the ION fd 
    assert(cap_buf.length == cap_size);  // ensure size matches
    ioctl(rotator_fd, VIDIOC_QBUF, &cap_buf);

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(rotator_fd, VIDIOC_STREAMON, &t); 
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(rotator_fd, VIDIOC_STREAMON, &t);

    struct pollfd pfd = {
        .fd = rotator_fd,
        .events = POLLIN | POLLRDNORM,
    };
    ret = poll(&pfd, 1, 2000);

    if (ret <= 0) {
        err("poll() timed out or error");
    }


    struct v4l2_buffer dq = {
    .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .memory = V4L2_MEMORY_USERPTR
    };
    if (ioctl(rotator_fd, VIDIOC_DQBUF, &dq) < 0) {
        err("CAP DQBUF");

    }

    struct v4l2_buffer dqout = {
        .type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ioctl(rotator_fd, VIDIOC_DQBUF, &dqout);

    void *linear_ptr = mmap(NULL, dq.bytesused,
                        PROT_READ|PROT_WRITE,
                        MAP_SHARED,
                        cap_ion_fd, 0);

    if (linear_ptr == MAP_FAILED) {
        err("mmap CAP");
        return -1;
    }
    // linear_ptr[0..dq.bytesused-1] is your NV12 frame
    *linear_data = (unsigned char *)linear_ptr;  // set the output pointer
    
    // clean up

    return 0;


    // Check if the rotator supports NV12_UBWC input format
    
   
}