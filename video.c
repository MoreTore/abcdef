/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <endian.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <linux/videodev2.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>
#include <media/msm_media_info.h>

#include "common.h"

#define DBG_TAG "   vid"

#define CASE(ENUM) case ENUM: return #ENUM;

static const struct {
	uint32_t mask;
	const char *str;
} v4l2_buf_flags[] = {
	{ V4L2_BUF_FLAG_MAPPED, "MAPPED" },
	{ V4L2_BUF_FLAG_QUEUED, "QUEUED" },
	{ V4L2_BUF_FLAG_DONE, "DONE" },
	{ V4L2_BUF_FLAG_KEYFRAME, "KEYFRAME" },
	{ V4L2_BUF_FLAG_PFRAME, "PFRAME" },
	{ V4L2_BUF_FLAG_BFRAME, "BFRAME" },
	{ V4L2_BUF_FLAG_ERROR, "ERROR" },
	{ V4L2_BUF_FLAG_TIMECODE, "TIMECODE" },
	{ V4L2_BUF_FLAG_PREPARED, "PREPARED" },
	{ V4L2_BUF_FLAG_NO_CACHE_INVALIDATE, "NO_CACHE_INVALIDATE" },
	{ V4L2_BUF_FLAG_NO_CACHE_CLEAN, "NO_CACHE_CLEAN" },
	{ V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN, "TIMESTAMP_UNKNOWN" },
	{ V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC" },
	{ V4L2_BUF_FLAG_TIMESTAMP_COPY, "TIMESTAMP_COPY" },
	{ V4L2_BUF_FLAG_TSTAMP_SRC_EOF, "TSTAMP_SRC_EOF" },
	{ V4L2_BUF_FLAG_TSTAMP_SRC_SOE, "TSTAMP_SRC_SOE" },
	{ V4L2_QCOM_BUF_FLAG_CODECCONFIG, "QCOM_CODECCONFIG" },
	{ V4L2_QCOM_BUF_FLAG_EOSEQ, "QCOM_EOSEQ" },
	{ V4L2_QCOM_BUF_TIMESTAMP_INVALID, "QCOM_TIMESTAMP_INVALID" },
	{ V4L2_QCOM_BUF_FLAG_IDRFRAME, "QCOM_IDRFRAME" },
	{ V4L2_QCOM_BUF_FLAG_DECODEONLY, "QCOM_DECODEONLY" },
	{ V4L2_QCOM_BUF_DATA_CORRUPT, "QCOM_DATA_CORRUPT" },
	{ V4L2_QCOM_BUF_DROP_FRAME, "QCOM_DROP_FRAME" },
	{ V4L2_QCOM_BUF_INPUT_UNSUPPORTED, "QCOM_INPUT_UNSUPPORTED" },
	{ V4L2_QCOM_BUF_FLAG_EOS, "QCOM_EOS" },
	{ V4L2_QCOM_BUF_FLAG_READONLY, "QCOM_READONLY" },
	{ V4L2_MSM_VIDC_BUF_START_CODE_NOT_FOUND, "MSM_START_CODE_NOT_FOUND" },
	{ V4L2_MSM_BUF_FLAG_YUV_601_709_CLAMP, "MSM_YUV_601_709_CLAMP" },
	{ V4L2_MSM_BUF_FLAG_MBAFF, "MSM_MBAFF" },
	{ V4L2_MSM_BUF_FLAG_DEFER, "MSM_DEFER" },
};

static const char *buf_flags_to_string(uint32_t flags)
{
	static __thread char s[256];
	size_t n = 0;

	for (size_t i = 0; i < ARRAY_LENGTH(v4l2_buf_flags); i++) {
		if (flags & v4l2_buf_flags[i].mask) {
			n += snprintf(s + n, sizeof (s) - n, "%s%s",
				      n > 0 ? "|" : "",
				      v4l2_buf_flags[i].str);
			if (n >= sizeof (s))
				break;
		}
	}

	s[MIN(n, sizeof (s) - 1)] = '\0';

	return s;
}

static const char *fourcc_to_string(uint32_t fourcc)
{
	static __thread char s[4];
	uint32_t fmt = htole32(fourcc);

	memcpy(s, &fmt, 4);

	return s;
}

static const char *buf_type_to_string(enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return "OUTPUT";
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return "CAPTURE";
	default:
		return "??";
	}
}

static const char *v4l2_field_to_string(enum v4l2_field field)
{
	switch (field) {
	CASE(V4L2_FIELD_ANY)
	CASE(V4L2_FIELD_NONE)
	CASE(V4L2_FIELD_TOP)
	CASE(V4L2_FIELD_BOTTOM)
	CASE(V4L2_FIELD_INTERLACED)
	CASE(V4L2_FIELD_SEQ_TB)
	CASE(V4L2_FIELD_SEQ_BT)
	CASE(V4L2_FIELD_ALTERNATE)
	CASE(V4L2_FIELD_INTERLACED_TB)
	CASE(V4L2_FIELD_INTERLACED_BT)
	default: return "unknown";
	}
}

static const char *v4l2_colorspace_to_string(enum v4l2_colorspace cspace)
{
	switch (cspace) {
	CASE(V4L2_COLORSPACE_SMPTE170M)
	CASE(V4L2_COLORSPACE_SMPTE240M)
	CASE(V4L2_COLORSPACE_REC709)
	CASE(V4L2_COLORSPACE_BT878)
	CASE(V4L2_COLORSPACE_470_SYSTEM_M)
	CASE(V4L2_COLORSPACE_470_SYSTEM_BG)
	CASE(V4L2_COLORSPACE_JPEG)
	CASE(V4L2_COLORSPACE_SRGB)
	default: return "unknown";
	}
}

#undef CASE

static void list_formats(struct instance *i, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fdesc;
	struct v4l2_frmsizeenum frmsize;

	dbg("%s formats:", buf_type_to_string(type));

	memzero(fdesc);
	fdesc.type = type;

	while (!ioctl(i->video.fd, VIDIOC_ENUM_FMT, &fdesc)) {
		dbg("  %s", fdesc.description);

		memzero(frmsize);
		frmsize.pixel_format = fdesc.pixelformat;

		while (!ioctl(i->video.fd, VIDIOC_ENUM_FRAMESIZES, &frmsize)) {
			switch (frmsize.type) {
			case V4L2_FRMSIZE_TYPE_DISCRETE:
				dbg("    %dx%d",
				    frmsize.discrete.width,
				    frmsize.discrete.height);
				break;
			case V4L2_FRMSIZE_TYPE_STEPWISE:
			case V4L2_FRMSIZE_TYPE_CONTINUOUS:
				dbg("    %dx%d to %dx%d, step %+d%+d",
				    frmsize.stepwise.min_width,
				    frmsize.stepwise.min_height,
				    frmsize.stepwise.max_width,
				    frmsize.stepwise.max_height,
				    frmsize.stepwise.step_width,
				    frmsize.stepwise.step_height);
				break;
			}

			if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
				break;

			frmsize.index++;
		}

		fdesc.index++;
	}
}

int video_open(struct instance *i, char *name)
{
	struct v4l2_capability cap;

	i->video.fd = open(name, O_RDWR, 0);
	if (i->video.fd < 0) {
		err("Failed to open video decoder: %s", name);
		return -1;
	}

	memzero(cap);
	if (ioctl(i->video.fd, VIDIOC_QUERYCAP, &cap) < 0) {
		err("Failed to verify capabilities: %m");
		return -1;
	}

	dbg("caps (%s): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
	    "version=%u.%u.%u", name, cap.driver, cap.bus_info, cap.card,
	    (cap.version >> 16) & 0xff,
	    (cap.version >> 8) & 0xff,
	    cap.version & 0xff);

	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_CAPTURE",
	    cap.capabilities & V4L2_CAP_RDS_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_HW_FREQ_SEEK",
	    cap.capabilities & V4L2_CAP_HW_FREQ_SEEK ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_OUTPUT",
	    cap.capabilities & V4L2_CAP_RDS_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_TUNER",
	    cap.capabilities & V4L2_CAP_TUNER ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_AUDIO",
	    cap.capabilities & V4L2_CAP_AUDIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RADIO",
	    cap.capabilities & V4L2_CAP_RADIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_MODULATOR",
	    cap.capabilities & V4L2_CAP_MODULATOR ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SDR_CAPTURE",
	    cap.capabilities & V4L2_CAP_SDR_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_EXT_PIX_FORMAT",
	    cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_READWRITE",
	    cap.capabilities & V4L2_CAP_READWRITE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_ASYNCIO",
	    cap.capabilities & V4L2_CAP_ASYNCIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_STREAMING",
	    cap.capabilities & V4L2_CAP_STREAMING ? '*' : ' ');

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE) ||
	    !(cap.capabilities & V4L2_CAP_STREAMING)) {
		err("Insufficient capabilities for video device (is %s correct?)",
		    name);
		return -1;
	}

	list_formats(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	list_formats(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        return 0;
}

void video_close(struct instance *i)
{
	close(i->video.fd);
}

int video_set_control(struct instance *i)
{
	struct v4l2_control control = {0};

	if (i->decode_order) {
		control.id = V4L2_CID_MPEG_VIDC_VIDEO_OUTPUT_ORDER;
		control.value = V4L2_MPEG_VIDC_VIDEO_OUTPUT_ORDER_DECODE;

		if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
			err("failed to set output order: %m");
			return -1;
		}
	}

	if (i->skip_frames) {
		control.id = V4L2_CID_MPEG_VIDC_VIDEO_PICTYPE_DEC_MODE;
		control.value = V4L2_MPEG_VIDC_VIDEO_PICTYPE_DECODE_ON;

		if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
			err("failed to set skip mode: %m");
			return -1;
		}
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONTINUE_DATA_TRANSFER;
	control.value = 1;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set data transfer mode: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_SET_PERF_LEVEL;
	control.value = V4L2_CID_MPEG_VIDC_PERF_LEVEL_TURBO;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set perf level: %m");
		return -1;
	}

	control.id = V4L2_CID_MPEG_VIDC_VIDEO_CONCEAL_COLOR;
	control.value = 0x00ff;

	if (ioctl(i->video.fd, VIDIOC_S_CTRL, &control) < 0) {
		err("failed to set conceal color: %m");
		return -1;
	}

	return 0;
}

int video_set_dpb(struct instance *i,
		  enum v4l2_mpeg_vidc_video_dpb_color_format format)
{
	struct v4l2_ext_control control[2] = {0};
	struct v4l2_ext_controls controls = {0};

	control[0].id = V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_MODE;
	control[0].value =
		(format == V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_TP10_UBWC) ?
		V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_SECONDARY :
		V4L2_CID_MPEG_VIDC_VIDEO_STREAM_OUTPUT_PRIMARY;

	control[1].id = V4L2_CID_MPEG_VIDC_VIDEO_DPB_COLOR_FORMAT;
	control[1].value = format;

	controls.count = 2;
	controls.ctrl_class = V4L2_CTRL_CLASS_MPEG;
	controls.controls = control;

	if (ioctl(i->video.fd, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
		err("failed to set dpb format: %m");
		return -1;
	}

	return 0;
}

int video_set_framerate(struct instance *i, int num, int den)
{
	struct v4l2_streamparm parm;

	dbg("set framerate to %.3f", (float)num / (float)den);

	memzero(parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	parm.parm.output.timeperframe.numerator = den;
	parm.parm.output.timeperframe.denominator = num;

	if (ioctl(i->video.fd, VIDIOC_S_PARM, &parm) < 0) {
		err("Failed to set framerate on OUTPUT: %m");
		return -1;
	}

	return 0;
}

static int video_count_capture_queued_bufs(struct video *vid)
{
	int cap_queued = 0;

	for (int idx = 0; idx < vid->cap_buf_cnt; idx++) {
		if (vid->cap_buf_flag[idx])
			cap_queued++;
	}

	return cap_queued;
}

static int video_count_output_queued_bufs(struct video *vid)
{
	int out_queued = 0;

	for (int idx = 0; idx < vid->out_buf_cnt; idx++) {
		if (vid->out_buf_flag[idx])
			out_queued++;
	}

	return out_queued;
}

int video_queue_buf_out(struct instance *i, int n, int length,
			uint32_t flags, struct timeval timestamp)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[1];

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (n >= vid->out_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = n;
	buf.length = 1;
	buf.m.planes = planes;

	buf.m.planes[0].m.userptr = (unsigned long)vid->out_ion_addr;
	buf.m.planes[0].reserved[0] = vid->out_ion_fd;
	buf.m.planes[0].reserved[1] = vid->out_buf_off[n];
	buf.m.planes[0].length = vid->out_buf_size;
	buf.m.planes[0].bytesused = length;
	buf.m.planes[0].data_offset = 0;

	buf.flags = flags;
	buf.timestamp = timestamp;

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	dbg("%s: queued buffer %d (flags:%08x:%s, bytesused:%d, "
	    "ts: %ld.%06lu), %d/%d queued", buf_type_to_string(buf.type),
	    buf.index, buf.flags, buf_flags_to_string(buf.flags),
	    buf.m.planes[0].bytesused,
	    buf.timestamp.tv_sec, buf.timestamp.tv_usec,
	    video_count_output_queued_bufs(vid), vid->out_buf_cnt);

	return 0;
}

int video_queue_buf_cap(struct instance *i, int n)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (n >= vid->cap_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.index = n;
	buf.length = 2;
	buf.m.planes = planes;

	buf.m.planes[0].m.userptr = (unsigned long)vid->cap_ion_addr;
	buf.m.planes[0].reserved[0] = vid->cap_ion_fd;
	buf.m.planes[0].reserved[1] = vid->cap_buf_off[n][0];
	buf.m.planes[0].length = vid->cap_buf_size[0];
	buf.m.planes[0].bytesused = vid->cap_buf_size[0];
	buf.m.planes[0].data_offset = 0;

	buf.m.planes[1].m.userptr = (unsigned long)vid->cap_ion_addr;
	buf.m.planes[1].reserved[0] = vid->cap_ion_fd;
	buf.m.planes[1].reserved[1] = 0;
	buf.m.planes[1].length = 0;
	buf.m.planes[1].bytesused = 0;
	buf.m.planes[1].data_offset = 0;

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	vid->cap_buf_flag[n] = 1;

	dbg("%s: queued buffer %d, %d/%d queued", buf_type_to_string(buf.type),
	    buf.index, video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);

	return 0;
}

static int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		err("failed to dequeue buffer on %s queue: %m",
		    buf_type_to_string(buf->type));
		return -errno;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dbg("%s: dequeued buffer %d, %d/%d queued",
		    buf_type_to_string(buf->type), buf->index,
		    video_count_output_queued_bufs(vid), vid->out_buf_cnt);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vid->cap_buf_flag[buf->index] = 0;
		dbg("%s: dequeued buffer %d (flags:%08x:%s, bytesused:%d, "
		    "ts: %ld.%06lu), %d/%d queued",
		    buf_type_to_string(buf->type),
		    buf->index, buf->flags, buf_flags_to_string(buf->flags),
		    buf->m.planes[0].bytesused,
		    buf->timestamp.tv_sec, buf->timestamp.tv_usec,
		    video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);
		break;
	}

	return 0;
}

int video_dequeue_output(struct instance *i, int *n)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

	ret = video_dequeue_buf(i, &buf);
	if (ret < 0)
		return ret;

	*n = buf.index;

	return 0;
}

int video_dequeue_capture(struct instance *i, int *n, int *finished,
			  unsigned int *bytesused, struct timeval *ts)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_USERPTR;
	buf.m.planes = planes;
	buf.length = CAP_PLANES;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*finished = 0;

	if (buf.flags & V4L2_QCOM_BUF_FLAG_EOS)
		*finished = 1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	if (ts)
		*ts = buf.timestamp;

	return 0;
}

int video_stream(struct instance *i, enum v4l2_buf_type type, int status)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, status, &type);
	if (ret) {
		err("failed to stream on %s queue (status=%d)",
		    buf_type_to_string(type), status);
		return -1;
	}

	dbg("%s: stream %s", buf_type_to_string(type),
	    status == VIDIOC_STREAMON ? "ON" :
	    status == VIDIOC_STREAMOFF ? "OFF" : "??");

	return 0;
}

int video_flush(struct instance *i, uint32_t flags)
{
	struct video *vid = &i->video;
	struct v4l2_decoder_cmd dec;

	memzero(dec);
	dec.flags = flags;
	dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
	if (ioctl(vid->fd, VIDIOC_DECODER_CMD, &dec) < 0) {
		err("failed to flush: %m");
		return -1;
	}

	return 0;
}

static int
alloc_ion_buffer(struct instance *i, size_t size)
{
	struct ion_allocation_data ion_alloc = { 0 };
	struct ion_fd_data ion_fd_data = { 0 };
	struct ion_handle_data ion_handle_data = { 0 };
	static int ion_fd = -1;
	int ret;

	if (ion_fd < 0) {
		ion_fd = open("/dev/ion", O_RDONLY);
		if (ion_fd < 0) {
			err("Cannot open ion device: %m");
			return -1;
		}
	}

	ion_alloc.len = size;
	ion_alloc.align = 4096;
	ion_alloc.heap_id_mask = ION_HEAP(ION_IOMMU_HEAP_ID);
	ion_alloc.flags = 0;
	ion_alloc.handle = -1;

	if (ioctl(ion_fd, ION_IOC_ALLOC, &ion_alloc) < 0) {
		err("Failed to allocate ion buffer: %m");
		return -1;
	}

	dbg("Allocated %zd bytes ION buffer %d",
	    ion_alloc.len, ion_alloc.handle);

	ion_fd_data.handle = ion_alloc.handle;
	ion_fd_data.fd = -1;

	if (ioctl(ion_fd, ION_IOC_MAP, &ion_fd_data) < 0) {
		err("Failed to map ion buffer: %m");
		ret = -1;
	} else {
		ret = ion_fd_data.fd;
	}

	ion_handle_data.handle = ion_alloc.handle;
	if (ioctl(ion_fd, ION_IOC_FREE, &ion_handle_data) < 0)
		err("Failed to free ion buffer: %m");

	return ret;
}

static int get_msm_color_format(uint32_t fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_NV12:
		return COLOR_FMT_NV12;
	case V4L2_PIX_FMT_NV21:
		return COLOR_FMT_NV21;
	case V4L2_PIX_FMT_NV12_UBWC:
		return COLOR_FMT_NV12_UBWC;
	case V4L2_PIX_FMT_NV12_TP10_UBWC:
		return COLOR_FMT_NV12_BPP10_UBWC;
	case V4L2_PIX_FMT_RGBA8888_UBWC:
		return COLOR_FMT_RGBA8888_UBWC;
	}

	return -1;
}

int video_setup_capture(struct instance *i, int num_buffers, int w, int h)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	int buffer_size;
	int color_fmt;
	int ion_fd;
	void *buf_addr;
	int n;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	video_set_dpb(i, i->depth == 10 ?
		      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_TP10_UBWC :
		      V4L2_MPEG_VIDC_VIDEO_DPB_COLOR_FMT_NONE);

	memzero(fmt);
	fmt.type = type;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.width = w;
#if 0
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12_UBWC;
#else
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
#endif

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format (%dx%d)",
		    buf_type_to_string(fmt.type), w, h);
		return -1;
	}

	memzero(reqbuf);
	reqbuf.count = num_buffers;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	dbg("%s: requested %d buffers, got %d", buf_type_to_string(type),
	    num_buffers, reqbuf.count);

	vid->cap_buf_cnt = reqbuf.count;

	if (ioctl(vid->fd, VIDIOC_G_FMT, &fmt) < 0) {
		err("failed to get %s format", buf_type_to_string(type));
		return -1;
	}

	dbg("  %dx%d fmt=%s (%d planes) field=%s cspace=%s flags=%08x",
	    fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	    fourcc_to_string(fmt.fmt.pix_mp.pixelformat),
	    fmt.fmt.pix_mp.num_planes,
	    v4l2_field_to_string(fmt.fmt.pix_mp.field),
	    v4l2_colorspace_to_string(fmt.fmt.pix_mp.colorspace),
	    fmt.fmt.pix_mp.flags);

	for (n = 0; n < fmt.fmt.pix_mp.num_planes; n++)
		dbg("    plane %d: size=%d stride=%d scanlines=%d", n,
		    fmt.fmt.pix_mp.plane_fmt[n].sizeimage,
		    fmt.fmt.pix_mp.plane_fmt[n].bytesperline,
		    fmt.fmt.pix_mp.plane_fmt[n].reserved[0]);

	color_fmt = get_msm_color_format(fmt.fmt.pix_mp.pixelformat);
	if (color_fmt < 0) {
		err("unhandled %s pixel format", buf_type_to_string(type));
		return -1;
	}

	vid->cap_buf_format = fmt.fmt.pix_mp.pixelformat;
	vid->cap_w = fmt.fmt.pix_mp.width;
	vid->cap_h = fmt.fmt.pix_mp.height;
	vid->cap_buf_stride[0] = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	vid->cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

#if 0
	vid->cap_h = VENUS_Y_SCANLINES(color_fmt, fmt.fmt.pix_mp.height);
	vid->cap_buf_stride[0] = VENUS_Y_STRIDE(color_fmt, fmt.fmt.pix_mp.width);
#endif

	buffer_size = VENUS_BUFFER_SIZE(color_fmt,
					fmt.fmt.pix_mp.width,
					fmt.fmt.pix_mp.height);

	if (vid->cap_buf_size[0] < buffer_size)
		vid->cap_buf_size[0] = buffer_size;

	ion_fd = alloc_ion_buffer(i, vid->cap_buf_cnt * vid->cap_buf_size[0]);
	if (ion_fd < 0)
		return -1;

	buf_addr = mmap(NULL, vid->cap_buf_cnt * vid->cap_buf_size[0],
			PROT_READ, MAP_SHARED, ion_fd, 0);
	if (buf_addr == MAP_FAILED) {
		err("failed to map %s buffer: %m", buf_type_to_string(type));
		return -1;
	}

	vid->cap_ion_fd = ion_fd;
	vid->cap_ion_addr = buf_addr;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		vid->cap_buf_off[n][0] = n * vid->cap_buf_size[0];
		vid->cap_buf_addr[n][0] = buf_addr + vid->cap_buf_off[n][0];
	}

	dbg("%s: succesfully mmapped %d buffers", buf_type_to_string(type),
	    vid->cap_buf_cnt);

	return 0;
}

int video_stop_capture(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

	if (vid->cap_ion_addr) {
		if (munmap(vid->cap_ion_addr,
			   vid->cap_buf_cnt * vid->cap_buf_size[0]))
			err("failed to unmap %s buffer: %m",
			    buf_type_to_string(type));
	}

	if (vid->cap_ion_fd >= 0) {
		if (close(vid->cap_ion_fd) < 0)
			err("failed to close %s ion buffer: %m",
			    buf_type_to_string(type));
	}

	vid->cap_ion_fd = -1;
	vid->cap_ion_addr = NULL;
	vid->cap_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	return 0;
}

int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	int ion_fd;
	void *buf_addr;
	int n;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	memzero(fmt);
	fmt.type = type;
	fmt.fmt.pix_mp.width = i->width;
	fmt.fmt.pix_mp.height = i->height;
	fmt.fmt.pix_mp.pixelformat = codec;

	video_set_framerate(i, i->fps_n, i->fps_d);

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format: %m", buf_type_to_string(type));
		return -1;
	}

	dbg("%s: setup buffer size=%u (requested=%u)", buf_type_to_string(type),
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage, size);

	vid->out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	memzero(reqbuf);
	reqbuf.count = count;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_USERPTR;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	vid->out_buf_cnt = reqbuf.count;

	dbg("%s: requested %d buffers, got %d", buf_type_to_string(type),
	    count, reqbuf.count);

	ion_fd = alloc_ion_buffer(i, vid->out_buf_cnt * vid->out_buf_size);
	if (ion_fd < 0)
		return -1;

	buf_addr = mmap(NULL, vid->out_buf_cnt * vid->out_buf_size,
			PROT_READ | PROT_WRITE, MAP_SHARED, ion_fd, 0);
	if (buf_addr == MAP_FAILED) {
		err("failed to map %s buffer: %m", buf_type_to_string(type));
		return -1;
	}

	vid->out_ion_fd = ion_fd;
	vid->out_ion_addr = buf_addr;

	for (n = 0; n < vid->out_buf_cnt; n++) {
		vid->out_buf_off[n] = n * vid->out_buf_size;
		vid->out_buf_addr[n] = buf_addr + vid->out_buf_off[n];
		vid->out_buf_flag[n] = 0;
	}

	dbg("%s: succesfully mmapped %d buffers", buf_type_to_string(type),
	    vid->out_buf_cnt);

	return 0;
}

int video_stop_output(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

	if (vid->out_ion_addr) {
		if (munmap(vid->out_ion_addr,
			   vid->out_buf_cnt * vid->out_buf_size))
			err("failed to unmap %s buffer: %m",
			    buf_type_to_string(type));
	}

	if (vid->out_ion_fd >= 0) {
		if (close(vid->out_ion_fd) < 0)
			err("failed to close %s ion buffer: %m",
			    buf_type_to_string(type));
	}

	vid->out_ion_fd = -1;
	vid->out_ion_addr = NULL;
	vid->out_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_USERPTR;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	return 0;
}

int video_subscribe_event(struct instance *i, int event_type)
{
	struct v4l2_event_subscription sub;

	memset(&sub, 0, sizeof(sub));
	sub.type = event_type;

	if (ioctl(i->video.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		err("failed to subscribe to event type %u: %m", sub.type);
		return -1;
	}

	return 0;
}

int video_dequeue_event(struct instance *i, struct v4l2_event *ev)
{
	struct video *vid = &i->video;

	memset(ev, 0, sizeof (*ev));

	if (ioctl(vid->fd, VIDIOC_DQEVENT, ev) < 0) {
		err("failed to dequeue event: %m");
		return -1;
	}

	return 0;
}
