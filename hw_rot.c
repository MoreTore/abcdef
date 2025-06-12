#include <fcntl.h>  
#include <unistd.h>  
#include <sys/ioctl.h>  
#include <linux/videodev2.h>  
#include <sys/mman.h>  
#include <errno.h>  
#include <string.h>  
#include <stdlib.h>  
  
int convert_ubwc_to_linear(unsigned char *ubwc_data, size_t ubwc_size,  
                          int width, int height,  
                          unsigned char **linear_data, size_t *linear_size)  
{  
    int rotator_fd = -1;  
    int ret = -1;  
    struct v4l2_format fmt_in = {0}, fmt_out = {0};  
    struct v4l2_requestbuffers req_in = {0}, req_out = {0};  
    struct v4l2_buffer buf_in = {0}, buf_out = {0};  
    void *mapped_in = NULL, *mapped_out = NULL;  
      
    // Open SDE rotator device  
    rotator_fd = open("/dev/video2", O_RDWR); // Typical rotator device node  
    if (rotator_fd < 0) {  
        err("Failed to open rotator device");  
        return -1;  
    }

    // VIDIOC_QUERYCAP to check capabilities
    struct v4l2_capability cap;
    // V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_VIDEO_OUTPUT|V4L2_CAP_VIDEO_M2M|V4L2_CAP_EXT_PIX_FORMAT|V4L2_CAP_STREAMING|V4L2_CAP_DEVICE_CAPS, device_caps=V4L2_CAP_VIDEO_CAPTURE|V4L2_CAP_VIDEO_OUTPUT|V4L2_CAP_VIDEO_M2M|V4L2_CAP_EXT_PIX_FORMAT|V4L2_CAP_STREAMING
    if (ioctl(rotator_fd, VIDIOC_QUERYCAP, &cap) < 0) {  
        err("Failed to query capabilities");  
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {  
        err("Rotator does not support video capture");
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {  
        err("Rotator does not support video output");
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M)) {  
        err("Rotator does not support memory-to-memory operations");
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT)) {  
        err("Rotator does not support extended pixel formats");
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {  
        err("Rotator does not support streaming");
        goto cleanup;  
    }
    if (!(cap.capabilities & V4L2_CAP_DEVICE_CAPS)) {  
        err("Rotator does not support device capabilities");  
        goto cleanup;  
    }

    // Check if the rotator supports NV12_UBWC input format
    
      
    // Set input format (UBWC)  
    fmt_in.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  
    fmt_in.fmt.pix_mp.width = width;  
    fmt_in.fmt.pix_mp.height = height;  
    fmt_in.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12_UBWC;  
    fmt_in.fmt.pix_mp.field = V4L2_FIELD_NONE;  
    fmt_in.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;  
    fmt_in.fmt.pix_mp.num_planes = 1;
      
    if (ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_in) < 0) {  
        err("Failed to set input format");  
        goto cleanup;  
    }
      
    // Set output format (linear NV12)  
    fmt_out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  
    fmt_out.fmt.pix_mp.width = width;  
    fmt_out.fmt.pix_mp.height = height;  
    fmt_out.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;  
    fmt_out.fmt.pix_mp.field = V4L2_FIELD_NONE;  
    fmt_out.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;  
      
    if (ioctl(rotator_fd, VIDIOC_S_FMT, &fmt_out) < 0) {  
        err("Failed to set output format");  
        goto cleanup;  
    }  
      
    // Request input buffers  
    req_in.count = 1;  
    req_in.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  
    req_in.memory = V4L2_MEMORY_MMAP;  
      
    if (ioctl(rotator_fd, VIDIOC_REQBUFS, &req_in) < 0) {  
        err("Failed to request input buffers");  
        goto cleanup;  
    }  
      
    // Request output buffers  
    req_out.count = 1;  
    req_out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  
    req_out.memory = V4L2_MEMORY_MMAP;  
      
    if (ioctl(rotator_fd, VIDIOC_REQBUFS, &req_out) < 0) {  
        err("Failed to request output buffers");  
        goto cleanup;  
    }  
      
    // Query and map input buffer  
    buf_in.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  
    buf_in.memory = V4L2_MEMORY_MMAP;  
    buf_in.index = 0;  
      
    if (ioctl(rotator_fd, VIDIOC_QUERYBUF, &buf_in) < 0) {  
        err("Failed to query input buffer");  
        goto cleanup;  
    }  
      
    mapped_in = mmap(NULL, buf_in.m.planes[0].length,  
                     PROT_READ | PROT_WRITE, MAP_SHARED,  
                     rotator_fd, buf_in.m.planes[0].m.mem_offset);  
    if (mapped_in == MAP_FAILED) {  
        err("Failed to map input buffer");  
        goto cleanup;  
    }  
      
    // Query and map output buffer  
    buf_out.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  
    buf_out.memory = V4L2_MEMORY_MMAP;  
    buf_out.index = 0;  
      
    if (ioctl(rotator_fd, VIDIOC_QUERYBUF, &buf_out) < 0) {  
        err("Failed to query output buffer");  
        goto cleanup;  
    }  
      
    mapped_out = mmap(NULL, buf_out.m.planes[0].length,  
                      PROT_READ | PROT_WRITE, MAP_SHARED,  
                      rotator_fd, buf_out.m.planes[0].m.mem_offset);  
    if (mapped_out == MAP_FAILED) {  
        err("Failed to map output buffer");  
        goto cleanup;  
    }  
      
    // Copy UBWC data to input buffer  
    memcpy(mapped_in, ubwc_data, ubwc_size);  
    buf_in.bytesused = ubwc_size;  
      
    // Start streaming  
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  
    if (ioctl(rotator_fd, VIDIOC_STREAMON, &type) < 0) {  
        err("Failed to start input streaming");  
        goto cleanup;  
    }  
      
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  
    if (ioctl(rotator_fd, VIDIOC_STREAMON, &type) < 0) {  
        err("Failed to start output streaming");  
        goto cleanup;  
    }  
      
    // Queue input buffer  
    if (ioctl(rotator_fd, VIDIOC_QBUF, &buf_in) < 0) {  
        err("Failed to queue input buffer");  
        goto cleanup;  
    }  
      
    // Queue output buffer  
    if (ioctl(rotator_fd, VIDIOC_QBUF, &buf_out) < 0) {  
        err("Failed to queue output buffer");  
        goto cleanup;  
    }  
      
    // Dequeue output buffer (this triggers the conversion)  
    if (ioctl(rotator_fd, VIDIOC_DQBUF, &buf_out) < 0) {  
        err("Failed to dequeue output buffer");  
        goto cleanup;  
    }  
      
    // Allocate and copy converted data  
    *linear_size = buf_out.bytesused;  
    *linear_data = malloc(*linear_size);  
    if (!*linear_data) {  
        err("Failed to allocate output buffer");  
        goto cleanup;  
    }  
      
    memcpy(*linear_data, mapped_out, *linear_size);  
    ret = 0; // Success  
      
cleanup:  
    // Stop streaming  
    if (rotator_fd >= 0) {  
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;  
        ioctl(rotator_fd, VIDIOC_STREAMON, &type);  
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  
        ioctl(rotator_fd, VIDIOC_STREAMOFF, &type);  
    }  
      
    // Unmap buffers  
    if (mapped_in && mapped_in != MAP_FAILED)  
        munmap(mapped_in, buf_in.m.planes[0].length);  
    if (mapped_out && mapped_out != MAP_FAILED)  
        munmap(mapped_out, buf_out.m.planes[0].length);  
      
    // Close device  
    if (rotator_fd >= 0)  
        close(rotator_fd);  
      
    return ret;  
}