#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <linux/ion.h>
#include <linux/msm_ion.h>

#define DBG_TAG "  main"

#include "common.h"
#include "video.h"
#include "defs.h"
#include "ts.h"
#include "packet.h"


#define INPUT_FILE "fcamera2.hevc"
#define VIDEO_DEVICE "/dev/video32"
#define ION_DEVICE "/dev/ion"
#define ROTATOR_DEVICE "/dev/video2"
#define WIDTH 1928
#define HEIGHT 1208
#define STREAM_BUFFER_SIZE (1024 * 1024)
#define OUTPUT_BUFFER_COUNT 4
#define CAPTURE_BUFFER_COUNT 4

#define EXTRADATA_IDX(__num_planes) ((__num_planes) ? (__num_planes) - 1 : 0)
#define TIMESTAMP_NONE	((uint64_t)-1)


#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))


int stream_open(struct instance *i) {
	const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	AVRational framerate;
	int codec;
	int ret;

	av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = avformat_find_stream_info(i->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(i->avctx, -1, i->url, 0);

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	i->width = codecpar->width ?: 1928;
	i->height = codecpar->height ?: 1208;
	i->need_header = 1;

	framerate = av_stream_get_r_frame_rate(i->stream);
	i->fps_n = framerate.num;
	i->fps_d = framerate.den;
    filter = av_bsf_get_by_name("hevc_mp4toannexb");

	i->fourcc = V4L2_PIX_FMT_HEVC;

    ret = av_bsf_alloc(filter, &i->bsf);
    if (ret < 0) {
        av_err(ret, "cannot allocate bistream filter");
        goto fail;
    }

    avcodec_parameters_copy(i->bsf->par_in, codecpar);
    i->bsf->time_base_in = i->stream->time_base;

    ret = av_bsf_init(i->bsf);
    if (ret < 0) {
        av_err(ret, "failed to initialize bitstream filter");
        goto fail;
    }
	

	return 0;

fail:
	stream_close(i);
	return -1;
}

void stream_close(struct instance *i) {
	i->stream = NULL;
	if (i->bsf)
		av_bsf_free(&i->bsf);
	if (i->avctx)
		avformat_close_input(&i->avctx);
}

int handle_video_event(struct instance *i) {
	struct v4l2_event event;

	if (video_dequeue_event(i, &event))
		return -1;

	switch (event.type) {
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_INSUFFICIENT: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int height = ptr[0];
		unsigned int width = ptr[1];
		// ptr[0] = event_notify->height;
		// ptr[1] = event_notify->width;
		// ptr[2] = event_notify->bit_depth;
		// ptr[3] = event_notify->pic_struct;
		// ptr[4] = event_notify->colour_space;
		// ptr[5] = event_notify->crop_data.top;
		// ptr[6] = event_notify->crop_data.left;
		// ptr[7] = event_notify->crop_data.height;
		// ptr[8] = event_notify->crop_data.width;
		// ptr[9] = msm_comm_get_v4l2_profile(
		// 	inst->fmts[OUTPUT_PORT].fourcc,
		// 	event_notify->profile);
		// ptr[10] = msm_comm_get_v4l2_level( // returns -22
		// 	inst->fmts[OUTPUT_PORT].fourcc, 
		// 	event_notify->level); 
		// ptr[11] = event_notify->max_dpb_count;
		// ptr[12] = event_notify->max_ref_count;
		// ptr[13] = event_notify->max_dec_buffering;

		info("Port Reconfig received insufficient, new size %ux%u",
		     width, height);

		i->depth = ptr[2];

		if (ptr[3] == MSM_VIDC_PIC_STRUCT_MAYBE_INTERLACED) {
			i->interlaced = 1;
		} else {
			i->interlaced = 0;
		}

		unsigned int cspace = ptr[4];

		i->width = width;
		i->height = height;
		i->reconfigure_pending = 1;
		info("See dmesg msm_vidc for more info");
		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_QCOM_CMD_FLUSH_CAPTURE);
		break;
	}
	case V4L2_EVENT_MSM_VIDC_PORT_SETTINGS_CHANGED_SUFFICIENT:
		dbg("Setting changed sufficient");
		break;
	case V4L2_EVENT_MSM_VIDC_FLUSH_DONE: {
		unsigned int *ptr = (unsigned int *)event.u.data;
		unsigned int flags = ptr[0];

		if (flags & V4L2_QCOM_CMD_FLUSH_CAPTURE)
			dbg("Flush Done received on CAPTURE queue");
		if (flags & V4L2_QCOM_CMD_FLUSH_OUTPUT)
			dbg("Flush Done received on OUTPUT queue");

		if (i->reconfigure_pending) {
			dbg("Reconfiguring output");
			restart_capture(i);
			i->reconfigure_pending = 0;
		}
		break;
	}
	case V4L2_EVENT_MSM_VIDC_SYS_ERROR:
		dbg("SYS Error received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_OVERLOAD:
		dbg("HW Overload received");
		break;
	case V4L2_EVENT_MSM_VIDC_HW_UNSUPPORTED:
		dbg("HW Unsupported received");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_BUFFER_REFERENCE:
		dbg("Release buffer reference");
		break;
	case V4L2_EVENT_MSM_VIDC_RELEASE_UNQUEUED_BUFFER:
		dbg("Release unqueued buffer");
		break;
	default:
		err("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

int handle_video_capture(struct instance *i) {
	struct video *vid = &i->video;
	struct timeval tv;
	uint32_t flags;
	uint64_t pts;
	unsigned int bytesused;
	struct msm_vidc_extradata_header *extradata;
	bool busy;
	int ret, n;

	/* capture buffer is ready */

	ret = video_dequeue_capture(i, &n, &bytesused, &flags, &tv, &extradata);
	if (ret < 0) {
		err("dequeue capture buffer fail");
		return ret;
	}

	if (flags & V4L2_QCOM_BUF_TIMESTAMP_INVALID)
		pts = TIMESTAMP_NONE;
	else
		pts = ((uint64_t)tv.tv_sec) * 1000000 + tv.tv_usec;

	busy = false;

	if (bytesused > 0) {
		struct ts_entry *l, *min = NULL;
		int pending = 0;

		vid->total_captured++;

		//pthread_mutex_lock(&i->lock);

		/* PTS are expected to be monotonically increasing,
		 * so when unknown use the lowest pending DTS */
		list_for_each_entry(l, &vid->pending_ts_list, link) {
			if (l->dts == TIMESTAMP_NONE)
				continue;
			if (min == NULL || min->dts > l->dts)
				min = l;
			pending++;
		}

		if (min) {
			dbg("pending %d min pts %" PRIi64
			    " dts %" PRIi64
			    " duration %" PRIi64, pending,
			    min->pts, min->dts, min->duration);
		}

		if (pts == TIMESTAMP_NONE) {
			dbg("no pts on frame");
			if (min && vid->pts_dts_delta != TIMESTAMP_NONE) {
				dbg("reuse dts %" PRIu64
				    " delta %" PRIu64,
				    min->dts, vid->pts_dts_delta);
				pts = min->dts + vid->pts_dts_delta;
			}
		}

		if (pts == TIMESTAMP_NONE) {
			if (min && vid->cap_last_pts != TIMESTAMP_NONE)
				pts = vid->cap_last_pts + min->duration;
			else
				pts = 0;

			dbg("guessing pts %" PRIu64, pts);
		}

		vid->cap_last_pts = pts;

		if (min != NULL) {
			pts -= min->base;
			ts_remove(min);
		}

		if (bytesused > 0 && vid->cap_buf_addr[n]) {
			info("Saving Frame %d, size %d", n, bytesused);
			// Convert UBWC to linear NV12 using SDE rotator  
			unsigned char *linear_data = NULL;  
			size_t linear_size = 0;
			unsigned long ion_fd = (unsigned long)vid->cap_buf_fd[n];

			int ret = convert_ubwc_to_linear(ion_fd, i->width, i->height, &linear_data, &linear_size);
		}

		//pthread_mutex_unlock(&i->lock);

		i->prerolled = 1;

	}

	if (!busy && !i->reconfigure_pending)
		video_queue_buf_cap(i, n);

	if (flags & V4L2_QCOM_BUF_FLAG_EOS) {
		info("End of stream");
		//finish(i);
	}

	return 0;
}

int handle_video_output(struct instance *i) {
	struct video *vid = &i->video;
	int ret, n;

	ret = video_dequeue_output(i, &n);
	if (ret < 0) {
		err("dequeue output buffer fail");
		return ret;
	}

	pthread_mutex_lock(&i->lock);
	vid->out_buf_flag[n] = 0;
	pthread_cond_signal(&i->cond);
	pthread_mutex_unlock(&i->lock);

	return 0;
}

int restart_capture(struct instance *i) {
	struct video *vid = &i->video;
	struct fb *fb, *next;
	int n;

	/*
	 * Destroy window buffers that are not in use by the
	 * wayland compositor; buffers in use will be destroyed
	 * when the release callback is called
	 */
	/* Stop capture and release buffers */
	if (vid->cap_buf_cnt > 0 && video_stop_capture(i))
		return -1;

	/* Setup capture queue with new parameters */
	if (video_setup_capture(i, 4, i->width, i->height))
		return -1;

	/* Start streaming */
	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	/* Queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_queue_buf_cap(i, n))
			return -1;
	}

	return 0;
}

int parse_frame(struct instance *i, AVPacket *pkt)
{
	int ret;

	if (!i->bsf_data_pending) {
		ret = av_read_frame(i->avctx, pkt);
		if (ret < 0)
			return ret;

		if (pkt->stream_index != i->stream->index) {
			av_packet_unref(pkt);
			return AVERROR(EAGAIN);
		}

		if (i->bsf) {
			ret = av_bsf_send_packet(i->bsf, pkt);
			if (ret < 0)
				return ret;

			i->bsf_data_pending = 1;
		}
	}

	if (i->bsf) {
		ret = av_bsf_receive_packet(i->bsf, pkt);
		if (ret == AVERROR(EAGAIN))
			i->bsf_data_pending = 0;

		if (ret < 0)
			return ret;
	}
	// for (int i = 0; i < pkt->size && i < 16; ++i)
    // 	print(2,"%02X ", pkt->data[i]);
	// print(2,"\n");
	return 0;
}

int get_buffer_unlocked(struct instance *i) {
	struct video *vid = &i->video;

	for (int n = 0; n < vid->out_buf_cnt; n++) {
		if (!vid->out_buf_flag[n])
			return n;
	}

	return -1;
}

void main_loop(struct instance *i) {
	struct video *vid = &i->video;
	struct wl_display *wl_display = NULL;
	struct pollfd pfd[EV_COUNT];
	int ev[EV_COUNT];
	short revents;
	int nfds = 0;
	int ret;

	dbg("main thread started");

	for (int i = 0; i < EV_COUNT; i++)
		ev[i] = -1;

	memset(pfd, 0, sizeof (pfd));

	pfd[nfds].fd = vid->fd;
	pfd[nfds].events = POLLOUT | POLLWRNORM | POLLPRI;
	ev[EV_VIDEO] = nfds++;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		ev[EV_SIGNAL] = nfds++;
	}

	AVPacket pkt;
	int buf = -1;
	int parse_ret;
	bool waiting_for_buf = false;
	av_init_packet(&pkt);
	
	while (!i->finish) {
		pfd[ev[EV_VIDEO]].events |= POLLIN | POLLRDNORM;

		ret = poll(pfd, nfds, 10);
		if (ret <= 0) {
			buf = get_buffer_unlocked(i);
			if (buf < 0) {
				err("No output buffer available");
				continue;
			}
			
			parse_ret = parse_frame(i, &pkt);
			if (parse_ret < 0) {
				if (parse_ret == AVERROR(EAGAIN)) {
					continue;
				} else if (parse_ret == AVERROR_EOF)
					dbg("Queue end of stream");
				else
					av_err(parse_ret, "Parsing failed");
				info("Sending EOS for buffer %d", buf);
				send_eos(i, buf);
				break;
			}
			if (send_pkt(i, buf, &pkt) < 0)
				break;
			av_packet_unref(&pkt);
			continue;
		}

		for (int idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			if (idx == ev[EV_VIDEO]) {
				if (revents & (POLLIN | POLLRDNORM))
					handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);

			} else if (idx == ev[EV_DISPLAY]) {
				if (revents & POLLOUT)
					pfd[ev[EV_DISPLAY]].events &= ~POLLOUT;

			} else if (idx == ev[EV_SIGNAL]) {
				handle_signal(i);
				break;
			}
		}
	}

	dbg("main thread finished");
}


int main(int argc, char **argv) {
    struct instance inst = {0};
	memset(&inst, 0, sizeof(inst));
    int ret;
	ret = parse_args(&inst, argc, argv);
	inst.sigfd = -1;
	INIT_LIST_HEAD(&inst.video.pending_ts_list);
	INIT_LIST_HEAD(&inst.fb_list);
	inst.video.pts_dts_delta = TIMESTAMP_NONE;
	inst.video.cap_last_pts = TIMESTAMP_NONE;
	inst.video.extradata_index = -1;
	inst.video.extradata_size = 0;
	inst.video.extradata_ion_fd = -1;
    
    if (ret < 0) {
        err("Usage: %s [-c] [-d] [-f] [-p] [-q] [-i] [-s] [-v] [-m device] url\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (stream_open(&inst)) {
        err("Failed to open stream\n");
        return EXIT_FAILURE;
    }

    inst.video.fd = open(VIDEO_DEVICE, O_RDWR, 0);
	if (inst.video.fd < 0) {
		err("Failed to open video decoder: %s", inst.video.name);
		return EXIT_FAILURE;
	}
	struct v4l2_capability cap;
	memzero(cap);
	if (ioctl(inst.video.fd, VIDIOC_QUERYCAP, &cap) < 0) {
		err("Failed to verify capabilities: %m");
		return -1;
	}

	const int n_events = sizeof(event_type) / sizeof(event_type[0]);
	for (int i = 0; i < n_events; i++) {
		if (video_subscribe_event(&inst, event_type[i])) {
			err("Failed to subscribe to event %d\n", event_type[i]);
			return EXIT_FAILURE;
		}
	}

	if (video_setup_output(&inst, inst.fourcc, STREAM_BUFFER_SIZE, OUTPUT_BUFFER_COUNT)) {
		err("Failed to setup video output\n");
		return EXIT_FAILURE;
	}

 	if (video_set_control(&inst)) {
		err("Failed to set video control\n");
		return EXIT_FAILURE;
	}
	
	if (video_stream(&inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,VIDIOC_STREAMON)) {
		err("Failed to start video output stream\n");
		return EXIT_FAILURE;
	}
	
	if (restart_capture(&inst)) {
		err("Failed to restart capture\n");
		return EXIT_FAILURE;
	}

	sigset_t sigmask;
	int fd;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	fd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	if (fd < 0) {
		perror("signalfd");
		return EXIT_FAILURE;
	}

	sigprocmask(SIG_BLOCK, &sigmask, NULL);
	inst.sigfd = fd;
	
	info("Video stream started successfully\n");
	
	main_loop(&inst);

	return 0;





}

