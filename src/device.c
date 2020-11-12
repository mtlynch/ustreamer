/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPG-HTTP streamer.                    #
#                                                                            #
#    Copyright (C) 2018  Maxim Devaev <mdevaev@gmail.com>                    #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#include "device.h"

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include <sys/select.h>
#include <sys/mman.h>
#include <sys/time.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "tools.h"
#include "logging.h"
#include "threading.h"
#include "xioctl.h"
#include "picture.h"


static const struct {
	const char *name;
	const v4l2_std_id standard;
} _STANDARDS[] = {
	{"UNKNOWN",	V4L2_STD_UNKNOWN},
	{"PAL",		V4L2_STD_PAL},
	{"NTSC",	V4L2_STD_NTSC},
	{"SECAM",	V4L2_STD_SECAM},
};

static const struct {
	const char *name;
	const unsigned format;
} _FORMATS[] = {
	{"YUYV",	V4L2_PIX_FMT_YUYV},
	{"UYVY",	V4L2_PIX_FMT_UYVY},
	{"RGB565",	V4L2_PIX_FMT_RGB565},
	{"RGB24",	V4L2_PIX_FMT_RGB24},
	{"JPEG",	V4L2_PIX_FMT_MJPEG},
	{"JPEG",	V4L2_PIX_FMT_JPEG},
};

static const struct {
	const char *name;
	const enum v4l2_memory io_method;
} _IO_METHODS[] = {
	{"MMAP",	V4L2_MEMORY_MMAP},
	{"USERPTR",	V4L2_MEMORY_USERPTR},
};


static int _device_open_check_cap(struct device_t *dev);
static int _device_open_dv_timings(struct device_t *dev);
static int _device_apply_dv_timings(struct device_t *dev);
static int _device_open_format(struct device_t *dev);
static void _device_open_hw_fps(struct device_t *dev);
static int _device_open_io_method(struct device_t *dev);
static int _device_open_io_method_mmap(struct device_t *dev);
static int _device_open_io_method_userptr(struct device_t *dev);
static int _device_open_queue_buffers(struct device_t *dev);
static void _device_open_alloc_picbufs(struct device_t *dev);
static int _device_apply_resolution(struct device_t *dev, unsigned width, unsigned height);

static void _device_apply_controls(struct device_t *dev);
static int _device_query_control(struct device_t *dev, struct v4l2_queryctrl *query, const char *name, unsigned cid, bool quiet);
static void _device_set_control(struct device_t *dev, struct v4l2_queryctrl *query, const char *name, unsigned cid, int value, bool quiet);

static const char *_format_to_string_fourcc(char *buf, size_t size, unsigned format);
static const char *_format_to_string_nullable(unsigned format);
static const char *_format_to_string_supported(unsigned format);
static const char *_standard_to_string(v4l2_std_id standard);
static const char *_io_method_to_string_supported(enum v4l2_memory io_method);


struct device_t *device_init(void) {
	struct device_runtime_t *run;
	struct device_t *dev;
	long cores_sysconf;
	unsigned cores_available;

	cores_sysconf = sysconf(_SC_NPROCESSORS_ONLN);
	cores_sysconf = (cores_sysconf < 0 ? 0 : cores_sysconf);
	cores_available = max_u(min_u(cores_sysconf, 4), 1);

	A_CALLOC(run, 1);
	run->fd = -1;

	A_CALLOC(dev, 1);
	dev->path = "/dev/video0";
	dev->width = 640;
	dev->height = 480;
	dev->format = V4L2_PIX_FMT_YUYV;
	dev->standard = V4L2_STD_UNKNOWN;
	dev->n_buffers = cores_available + 1;
	dev->n_workers = min_u(cores_available, dev->n_buffers);
	dev->min_frame_size = 128;
	dev->timeout = 1;
	dev->error_delay = 1;
	dev->io_method = V4L2_MEMORY_MMAP;
	dev->run = run;
	return dev;
}

void device_destroy(struct device_t *dev) {
	free(dev->run);
	free(dev);
}

int device_parse_format(const char *str) {
	for (unsigned index = 0; index < ARRAY_LEN(_FORMATS); ++index) {
		if (!strcasecmp(str, _FORMATS[index].name)) {
			return _FORMATS[index].format;
		}
	}
	return FORMAT_UNKNOWN;
}

v4l2_std_id device_parse_standard(const char *str) {
	for (unsigned index = 1; index < ARRAY_LEN(_STANDARDS); ++index) {
		if (!strcasecmp(str, _STANDARDS[index].name)) {
			return _STANDARDS[index].standard;
		}
	}
	return STANDARD_UNKNOWN;
}

int device_parse_io_method(const char *str) {
	for (unsigned index = 0; index < ARRAY_LEN(_IO_METHODS); ++index) {
		if (!strcasecmp(str, _IO_METHODS[index].name)) {
			return _IO_METHODS[index].io_method;
		}
	}
	return IO_METHOD_UNKNOWN;
}

int device_open(struct device_t *dev) {
	if ((dev->run->fd = open(dev->path, O_RDWR|O_NONBLOCK)) < 0) {
		LOG_PERROR("Can't open device");
		goto error;
	}
	LOG_INFO("Device fd=%d opened", dev->run->fd);

	if (_device_open_check_cap(dev) < 0) {
		goto error;
	}
	if (_device_open_dv_timings(dev) < 0) {
		goto error;
	}
	if (_device_open_format(dev) < 0) {
		goto error;
	}
	_device_open_hw_fps(dev);
	if (_device_open_io_method(dev) < 0) {
		goto error;
	}
	if (_device_open_queue_buffers(dev) < 0) {
		goto error;
	}
	_device_open_alloc_picbufs(dev);
	_device_apply_controls(dev);

	dev->run->n_workers = min_u(dev->run->n_buffers, dev->n_workers);

	LOG_DEBUG("Device fd=%d initialized", dev->run->fd);
	return 0;

	error:
		device_close(dev);
		return -1;
}

void device_close(struct device_t *dev) {
	dev->run->n_workers = 0;

	if (dev->run->pictures) {
		LOG_DEBUG("Releasing picture buffers ...");
		for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
			picture_destroy(dev->run->pictures[index]);
		}
		free(dev->run->pictures);
		dev->run->pictures = NULL;
	}

	if (dev->run->hw_buffers) {
		LOG_DEBUG("Releasing device buffers ...");
		for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
#			define HW_BUFFER(_next) dev->run->hw_buffers[index]._next

			if (dev->io_method == V4L2_MEMORY_MMAP) {
				if (HW_BUFFER(allocated) > 0 && HW_BUFFER(data) != MAP_FAILED) {
					if (munmap(HW_BUFFER(data), HW_BUFFER(allocated)) < 0) {
						LOG_PERROR("Can't unmap device buffer %u", index);
					}
				}
			} else { // V4L2_MEMORY_USERPTR
				if (HW_BUFFER(data)) {
					free(HW_BUFFER(data));
				}
			}
			A_MUTEX_DESTROY(&HW_BUFFER(grabbed_mutex));

#			undef HW_BUFFER
		}
		dev->run->n_buffers = 0;
		free(dev->run->hw_buffers);
		dev->run->hw_buffers = NULL;
	}

	if (dev->run->fd >= 0) {
		LOG_DEBUG("Closing device ...");
		if (close(dev->run->fd) < 0) {
			LOG_PERROR("Can't close device fd=%d", dev->run->fd);
		} else {
			LOG_INFO("Device fd=%d closed", dev->run->fd);
		}
		dev->run->fd = -1;
	}
}

int device_switch_capturing(struct device_t *dev, bool enable) {
	if (enable != dev->run->capturing) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		LOG_DEBUG("Calling ioctl(%s) ...", (enable ? "VIDIOC_STREAMON" : "VIDIOC_STREAMOFF"));
		if (xioctl(dev->run->fd, (enable ? VIDIOC_STREAMON : VIDIOC_STREAMOFF), &type) < 0) {
			LOG_PERROR("Unable to %s capturing", (enable ? "start" : "stop"));
			if (enable) {
				return -1;
			}
		}

		dev->run->capturing = enable;
		LOG_INFO("Capturing %s", (enable ? "started" : "stopped"));
	}
    return 0;
}

int device_select(struct device_t *dev, bool *has_read, bool *has_write, bool *has_error) {
	struct timeval timeout;
	int retval;

#	define INIT_FD_SET(_set) \
		fd_set _set; FD_ZERO(&_set); FD_SET(dev->run->fd, &_set);

	INIT_FD_SET(read_fds);
	INIT_FD_SET(write_fds);
	INIT_FD_SET(error_fds);

#	undef INIT_FD_SET

	timeout.tv_sec = dev->timeout;
	timeout.tv_usec = 0;

	LOG_DEBUG("Calling select() on video device ...");

	retval = select(dev->run->fd + 1, &read_fds, &write_fds, &error_fds, &timeout);
	if (retval > 0) {
		*has_read = FD_ISSET(dev->run->fd, &read_fds);
		*has_write = FD_ISSET(dev->run->fd, &write_fds);
		*has_error = FD_ISSET(dev->run->fd, &error_fds);
	} else {
		*has_read = false;
		*has_write = false;
		*has_error = false;
	}

	LOG_DEBUG("Device select() --> %d", retval);
	return retval;
}

int device_grab_buffer(struct device_t *dev) {
	struct v4l2_buffer buf_info;

	MEMSET_ZERO(buf_info);
	buf_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf_info.memory = dev->io_method;

	LOG_DEBUG("Grabbing device buffer ...");
	if (xioctl(dev->run->fd, VIDIOC_DQBUF, &buf_info) < 0) {
		LOG_PERROR("Unable to grab device buffer");
		return -1;
	}

	LOG_DEBUG("Grabbed new frame in device buffer: index=%u, bytesused=%u",
		buf_info.index, buf_info.bytesused);

	if (buf_info.index >= dev->run->n_buffers) {
		LOG_ERROR("V4L2 error: grabbed invalid device buffer: index=%u, nbuffers=%u",
			buf_info.index, dev->run->n_buffers);
		return -1;
	}

#	define HW_BUFFER(_next) dev->run->hw_buffers[buf_info.index]._next

	A_MUTEX_LOCK(&HW_BUFFER(grabbed_mutex));
	if (HW_BUFFER(grabbed)) {
		LOG_ERROR("V4L2 error: grabbed device buffer is already used: index=%u, bytesused=%u",
			buf_info.index, buf_info.bytesused);
		A_MUTEX_UNLOCK(&HW_BUFFER(grabbed_mutex));
		return -1;
	}
	HW_BUFFER(grabbed) = true;
	A_MUTEX_UNLOCK(&HW_BUFFER(grabbed_mutex));

	HW_BUFFER(used) = buf_info.bytesused;
	memcpy(&HW_BUFFER(buf_info), &buf_info, sizeof(struct v4l2_buffer));
	dev->run->pictures[buf_info.index]->grab_ts = get_now_monotonic();

#	undef HW_BUFFER
	return buf_info.index;
}

int device_release_buffer(struct device_t *dev, unsigned index) {
#	define HW_BUFFER(_next) dev->run->hw_buffers[index]._next

	LOG_DEBUG("Releasing device buffer index=%u ...", index);

	A_MUTEX_LOCK(&HW_BUFFER(grabbed_mutex));
	if (xioctl(dev->run->fd, VIDIOC_QBUF, &HW_BUFFER(buf_info)) < 0) {
		LOG_PERROR("Unable to release device buffer index=%u", index);
		A_MUTEX_UNLOCK(&HW_BUFFER(grabbed_mutex));
		return -1;
	}
	HW_BUFFER(grabbed) = false;
	A_MUTEX_UNLOCK(&HW_BUFFER(grabbed_mutex));
	HW_BUFFER(used) = 0;

#	undef HW_BUFFER
	return 0;
}

int device_consume_event(struct device_t *dev) {
	struct v4l2_event event;

	LOG_DEBUG("Calling ioctl(VIDIOC_DQEVENT) ...");
	if (xioctl(dev->run->fd, VIDIOC_DQEVENT, &event) == 0) {
		switch (event.type) {
			case V4L2_EVENT_SOURCE_CHANGE:
				LOG_INFO("Got V4L2_EVENT_SOURCE_CHANGE: source changed");
				return -1;
			case V4L2_EVENT_EOS:
				LOG_INFO("Got V4L2_EVENT_EOS: end of stream (ignored)");
				return 0;
		}
	} else {
		LOG_PERROR("Got some V4L2 device event, but where is it? ");
	}
	return 0;
}

static int _device_open_check_cap(struct device_t *dev) {
	struct v4l2_capability cap;
	int input = dev->input; // Needs pointer to int for ioctl()

	MEMSET_ZERO(cap);

	LOG_DEBUG("Calling ioctl(VIDIOC_QUERYCAP) ...");
	if (xioctl(dev->run->fd, VIDIOC_QUERYCAP, &cap) < 0) {
		LOG_PERROR("Can't query device (VIDIOC_QUERYCAP)");
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		LOG_ERROR("Video capture not supported by the device");
		return -1;
	}

	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		LOG_ERROR("Device does not support streaming IO");
		return -1;
	}

	LOG_INFO("Using input channel: %d", input);
	if (xioctl(dev->run->fd, VIDIOC_S_INPUT, &input) < 0) {
		LOG_ERROR("Can't set input channel");
		return -1;
	}

	if (dev->standard != V4L2_STD_UNKNOWN) {
		LOG_INFO("Using TV standard: %s", _standard_to_string(dev->standard));
		if (xioctl(dev->run->fd, VIDIOC_S_STD, &dev->standard) < 0) {
			LOG_ERROR("Can't set video standard");
			return -1;
		}
	} else {
		LOG_INFO("Using TV standard: DEFAULT");
	}
	return 0;
}

static int _device_open_dv_timings(struct device_t *dev) {
	_device_apply_resolution(dev, dev->width, dev->height);
	if (dev->dv_timings) {
		LOG_DEBUG("Using DV timings");

		if (_device_apply_dv_timings(dev) < 0) {
			return -1;
		}

		struct v4l2_event_subscription sub;

		MEMSET_ZERO(sub);
		sub.type = V4L2_EVENT_SOURCE_CHANGE;

		LOG_DEBUG("Calling ioctl(VIDIOC_SUBSCRIBE_EVENT) ...");
		if (xioctl(dev->run->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
			LOG_PERROR("Can't subscribe to V4L2_EVENT_SOURCE_CHANGE");
			return -1;
		}
	}
	return 0;
}

static int _device_apply_dv_timings(struct device_t *dev) {
	struct v4l2_dv_timings dv;

	MEMSET_ZERO(dv);

	LOG_DEBUG("Calling ioctl(VIDIOC_QUERY_DV_TIMINGS) ...");
	if (xioctl(dev->run->fd, VIDIOC_QUERY_DV_TIMINGS, &dv) == 0) {
		LOG_INFO("Got new DV timings: resolution=%ux%u, pixclk=%llu",
			dv.bt.width, dv.bt.height, (unsigned long long)dv.bt.pixelclock); // Issue #11

		LOG_DEBUG("Calling ioctl(VIDIOC_S_DV_TIMINGS) ...");
		if (xioctl(dev->run->fd, VIDIOC_S_DV_TIMINGS, &dv) < 0) {
			LOG_PERROR("Failed to set DV timings");
			return -1;
		}

		if (_device_apply_resolution(dev, dv.bt.width, dv.bt.height) < 0) {
			return -1;
		}

	} else {
		LOG_DEBUG("Calling ioctl(VIDIOC_QUERYSTD) ...");
		if (xioctl(dev->run->fd, VIDIOC_QUERYSTD, &dev->standard) == 0) {
			LOG_INFO("Applying the new VIDIOC_S_STD: %s ...", _standard_to_string(dev->standard));
			if (xioctl(dev->run->fd, VIDIOC_S_STD, &dev->standard) < 0) {
				LOG_PERROR("Can't set video standard");
				return -1;
			}
		}
	}
	return 0;
}

static int _device_open_format(struct device_t *dev) {
	struct v4l2_format fmt;

	MEMSET_ZERO(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = dev->run->width;
	fmt.fmt.pix.height = dev->run->height;
	fmt.fmt.pix.pixelformat = dev->format;
	fmt.fmt.pix.field = V4L2_FIELD_ANY;

	// Set format
	LOG_DEBUG("Calling ioctl(VIDIOC_S_FMT) ...");
	if (xioctl(dev->run->fd, VIDIOC_S_FMT, &fmt) < 0) {
		LOG_PERROR("Unable to set pixelformat=%s, resolution=%ux%u",
			_format_to_string_supported(dev->format),
			dev->run->width,
			dev->run->height);
		return -1;
	}

	// Check resolution
	if (fmt.fmt.pix.width != dev->run->width || fmt.fmt.pix.height != dev->run->height) {
		LOG_ERROR("Requested resolution=%ux%u is unavailable", dev->run->width, dev->run->height);
	}
	if (_device_apply_resolution(dev, fmt.fmt.pix.width, fmt.fmt.pix.height) < 0) {
		return -1;
	}
	LOG_INFO("Using resolution: %ux%u", dev->run->width, dev->run->height);

	// Check format
	if (fmt.fmt.pix.pixelformat != dev->format) {
		char format_obtained_str[8];
		char *format_str_nullable;

		LOG_ERROR("Could not obtain the requested pixelformat=%s; driver gave us %s",
			_format_to_string_supported(dev->format),
			_format_to_string_supported(fmt.fmt.pix.pixelformat));

		if ((format_str_nullable = (char *)_format_to_string_nullable(fmt.fmt.pix.pixelformat)) != NULL) {
			LOG_INFO("Falling back to pixelformat=%s", format_str_nullable);
		} else {
			LOG_ERROR("Unsupported pixelformat=%s (fourcc)",
				_format_to_string_fourcc(format_obtained_str, 8, fmt.fmt.pix.pixelformat));
			return -1;
		}
	}

	dev->run->format = fmt.fmt.pix.pixelformat;
	LOG_INFO("Using pixelformat: %s", _format_to_string_supported(dev->run->format));

	dev->run->raw_size = fmt.fmt.pix.sizeimage; // Only for userptr
	return 0;
}

static void _device_open_hw_fps(struct device_t *dev) {
	struct v4l2_streamparm setfps;

	dev->run->hw_fps = 0;

	MEMSET_ZERO(setfps);
	setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	LOG_DEBUG("Calling ioctl(VIDIOC_G_PARM) ...");
	if (xioctl(dev->run->fd, VIDIOC_G_PARM, &setfps) < 0) {
		if (errno == ENOTTY) { // Quiet message for Auvidea B101
			LOG_INFO("Querying HW FPS changing is not supported");
		} else {
			LOG_PERROR("Unable to query HW FPS changing");
		}
		return;
	}

	if (!(setfps.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
		LOG_INFO("Changing HW FPS is not supported");
		return;
	}

#	define SETFPS_TPF(_next) setfps.parm.capture.timeperframe._next

	MEMSET_ZERO(setfps);
	setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	SETFPS_TPF(numerator) = 1;
	SETFPS_TPF(denominator) = (dev->desired_fps == 0 ? 255 : dev->desired_fps);

	if (xioctl(dev->run->fd, VIDIOC_S_PARM, &setfps) < 0) {
		LOG_PERROR("Unable to set HW FPS");
		return;
	}

	if (SETFPS_TPF(numerator) != 1) {
		LOG_ERROR("Invalid HW FPS numerator: %u != 1", SETFPS_TPF(numerator));
		return;
	}

	if (SETFPS_TPF(denominator) == 0) { // Не знаю, бывает ли так, но пускай на всякий случай
		LOG_ERROR("Invalid HW FPS denominator: 0");
		return;
	}

	dev->run->hw_fps = SETFPS_TPF(denominator);
	if (dev->desired_fps != dev->run->hw_fps) {
		LOG_INFO("Using HW FPS: %u -> %u (coerced)", dev->desired_fps, dev->run->hw_fps);
	} else {
		LOG_INFO("Using HW FPS: %u", dev->run->hw_fps);
	}

#	undef SETFPS_TPF
}

static int _device_open_io_method(struct device_t *dev) {
	LOG_INFO("Using IO method: %s", _io_method_to_string_supported(dev->io_method));
	switch (dev->io_method) {
		case V4L2_MEMORY_MMAP: return _device_open_io_method_mmap(dev);
		case V4L2_MEMORY_USERPTR: return _device_open_io_method_userptr(dev);
		default: assert(0 && "Unsupported IO method");
	}
	return -1;
}

static int _device_open_io_method_mmap(struct device_t *dev) {
	struct v4l2_requestbuffers req;

	MEMSET_ZERO(req);
	req.count = dev->n_buffers;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	LOG_DEBUG("Calling ioctl(VIDIOC_REQBUFS) for V4L2_MEMORY_MMAP ...");
	if (xioctl(dev->run->fd, VIDIOC_REQBUFS, &req) < 0) {
		LOG_PERROR("Device '%s' doesn't support V4L2_MEMORY_MMAP", dev->path);
		return -1;
	}

	if (req.count < 1) {
		LOG_ERROR("Insufficient buffer memory: %u", req.count);
		return -1;
	} else {
		LOG_INFO("Requested %u device buffers, got %u", dev->n_buffers, req.count);
	}

	LOG_DEBUG("Allocating device buffers ...");

	A_CALLOC(dev->run->hw_buffers, req.count);
	for (dev->run->n_buffers = 0; dev->run->n_buffers < req.count; ++dev->run->n_buffers) {
		struct v4l2_buffer buf_info;

		MEMSET_ZERO(buf_info);
		buf_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf_info.memory = V4L2_MEMORY_MMAP;
		buf_info.index = dev->run->n_buffers;

		LOG_DEBUG("Calling ioctl(VIDIOC_QUERYBUF) for device buffer %u ...", dev->run->n_buffers);
		if (xioctl(dev->run->fd, VIDIOC_QUERYBUF, &buf_info) < 0) {
			LOG_PERROR("Can't VIDIOC_QUERYBUF");
			return -1;
		}

#		define HW_BUFFER(_next) dev->run->hw_buffers[dev->run->n_buffers]._next

		A_MUTEX_INIT(&HW_BUFFER(grabbed_mutex));

		LOG_DEBUG("Mapping device buffer %u ...", dev->run->n_buffers);
		HW_BUFFER(data) = mmap(NULL, buf_info.length, PROT_READ|PROT_WRITE, MAP_SHARED, dev->run->fd, buf_info.m.offset);
		if (HW_BUFFER(data) == MAP_FAILED) {
			LOG_PERROR("Can't map device buffer %u", dev->run->n_buffers);
			return -1;
		}
		HW_BUFFER(allocated) = buf_info.length;

#		undef HW_BUFFER
	}
	return 0;
}

static int _device_open_io_method_userptr(struct device_t *dev) {
	struct v4l2_requestbuffers req;
	unsigned page_size = getpagesize();
	unsigned buf_size = align_size(dev->run->raw_size, page_size);

	MEMSET_ZERO(req);
	req.count = dev->n_buffers;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_USERPTR;

	LOG_DEBUG("Calling ioctl(VIDIOC_REQBUFS) for V4L2_MEMORY_USERPTR ...");
	if (xioctl(dev->run->fd, VIDIOC_REQBUFS, &req) < 0) {
		LOG_PERROR("Device '%s' doesn't support V4L2_MEMORY_USERPTR", dev->path);
		return -1;
	}

	if (req.count < 1) {
		LOG_ERROR("Insufficient buffer memory: %u", req.count);
		return -1;
	} else {
		LOG_INFO("Requested %u device buffers, got %u", dev->n_buffers, req.count);
	}

	LOG_DEBUG("Allocating device buffers ...");

	A_CALLOC(dev->run->hw_buffers, req.count);
	for (dev->run->n_buffers = 0; dev->run->n_buffers < req.count; ++dev->run->n_buffers) {
#       define HW_BUFFER(_next) dev->run->hw_buffers[dev->run->n_buffers]._next

		assert(HW_BUFFER(data) = aligned_alloc(page_size, buf_size));
		memset(HW_BUFFER(data), 0, buf_size);
		HW_BUFFER(allocated) = buf_size;

#		undef HW_BUFFER
	}
	return 0;
}

static int _device_open_queue_buffers(struct device_t *dev) {
	for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
		struct v4l2_buffer buf_info;

		MEMSET_ZERO(buf_info);
		buf_info.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf_info.memory = dev->io_method;
		buf_info.index = index;
		if (dev->io_method == V4L2_MEMORY_USERPTR) {
			buf_info.m.userptr = (unsigned long)dev->run->hw_buffers[index].data;
			buf_info.length = dev->run->hw_buffers[index].allocated;
		}

		LOG_DEBUG("Calling ioctl(VIDIOC_QBUF) for buffer %u ...", index);
		if (xioctl(dev->run->fd, VIDIOC_QBUF, &buf_info) < 0) {
			LOG_PERROR("Can't VIDIOC_QBUF");
			return -1;
		}
	}
	return 0;
}

static void _device_open_alloc_picbufs(struct device_t *dev) {
	size_t picture_size = picture_get_generous_size(dev->run->width, dev->run->height);

	LOG_DEBUG("Allocating picture buffers ...");
	A_CALLOC(dev->run->pictures, dev->run->n_buffers);

	for (unsigned index = 0; index < dev->run->n_buffers; ++index) {
		dev->run->pictures[index] = picture_init();
		LOG_DEBUG("Pre-allocating picture buffer %u sized %zu bytes... ", index, picture_size);
		picture_realloc_data(dev->run->pictures[index], picture_size);
	}
}

static int _device_apply_resolution(struct device_t *dev, unsigned width, unsigned height) {
	// Тут VIDEO_MIN_* не используются из-за странностей минимального разрешения при отсутствии сигнала
	// у некоторых устройств, например Auvidea B101
	if (
		width == 0 || width > VIDEO_MAX_WIDTH
		|| height == 0 || height > VIDEO_MAX_HEIGHT
	) {
		LOG_ERROR("Requested forbidden resolution=%ux%u: min=1x1, max=%ux%u",
			width, height, VIDEO_MAX_WIDTH, VIDEO_MAX_HEIGHT);
		return -1;
	}
	dev->run->width = width;
	dev->run->height = height;
	return 0;
}

static void _device_apply_controls(struct device_t *dev) {
#	define SET_CID_VALUE(_cid, _field, _value, _quiet) { \
			struct v4l2_queryctrl query; \
			if (_device_query_control(dev, &query, #_field, _cid, _quiet) == 0) { \
				_device_set_control(dev, &query, #_field, _cid, _value, _quiet); \
			} \
		}

#	define SET_CID_DEFAULT(_cid, _field, _quiet) { \
			struct v4l2_queryctrl query; \
			if (_device_query_control(dev, &query, #_field, _cid, _quiet) == 0) { \
				_device_set_control(dev, &query, #_field, _cid, query.default_value, _quiet); \
			} \
		}

#	define CONTROL_MANUAL_CID(_cid, _field) { \
			if (dev->ctl._field.mode == CTL_MODE_VALUE) { \
				SET_CID_VALUE(_cid, _field, dev->ctl._field.value, false); \
			} else if (dev->ctl._field.mode == CTL_MODE_DEFAULT) { \
				SET_CID_DEFAULT(_cid, _field, false); \
			} \
		}

#	define CONTROL_AUTO_CID(_cid_auto, _cid_manual, _field) { \
			if (dev->ctl._field.mode == CTL_MODE_VALUE) { \
				SET_CID_VALUE(_cid_auto, _field##_auto, 0, true); \
				SET_CID_VALUE(_cid_manual, _field, dev->ctl._field.value, false); \
			} else if (dev->ctl._field.mode == CTL_MODE_AUTO) { \
				SET_CID_VALUE(_cid_auto, _field##_auto, 1, false); \
			} else if (dev->ctl._field.mode == CTL_MODE_DEFAULT) { \
				SET_CID_VALUE(_cid_auto, _field##_auto, 0, true); /* Reset inactive flag */ \
				SET_CID_DEFAULT(_cid_manual, _field, false); \
				SET_CID_DEFAULT(_cid_auto, _field##_auto, false); \
			} \
		}

	CONTROL_AUTO_CID	(V4L2_CID_AUTOBRIGHTNESS,		V4L2_CID_BRIGHTNESS,				brightness);
	CONTROL_MANUAL_CID	(								V4L2_CID_CONTRAST,					contrast);
	CONTROL_MANUAL_CID	(								V4L2_CID_SATURATION,				saturation);
	CONTROL_AUTO_CID	(V4L2_CID_HUE_AUTO,				V4L2_CID_HUE,						hue);
	CONTROL_MANUAL_CID	(								V4L2_CID_GAMMA,						gamma);
	CONTROL_MANUAL_CID	(								V4L2_CID_SHARPNESS,					sharpness);
	CONTROL_MANUAL_CID	(								V4L2_CID_BACKLIGHT_COMPENSATION,	backlight_compensation);
	CONTROL_AUTO_CID	(V4L2_CID_AUTO_WHITE_BALANCE,	V4L2_CID_WHITE_BALANCE_TEMPERATURE,	white_balance);
	CONTROL_AUTO_CID	(V4L2_CID_AUTOGAIN,				V4L2_CID_GAIN,						gain);
	CONTROL_MANUAL_CID	(								V4L2_CID_COLORFX,					color_effect);
	CONTROL_MANUAL_CID	(								V4L2_CID_VFLIP,						flip_vertical);
	CONTROL_MANUAL_CID	(								V4L2_CID_HFLIP,						flip_horizontal);

#	undef CONTROL_AUTO_CID
#	undef CONTROL_MANUAL_CID
#	undef SET_CID_DEFAULT
#	undef SET_CID_VALUE
}

static int _device_query_control(struct device_t *dev, struct v4l2_queryctrl *query, const char *name, unsigned cid, bool quiet) {
	// cppcheck-suppress redundantPointerOp
	MEMSET_ZERO(*query);
	query->id = cid;

	if (xioctl(dev->run->fd, VIDIOC_QUERYCTRL, query) < 0 || query->flags & V4L2_CTRL_FLAG_DISABLED) {
		if (!quiet) {
			LOG_ERROR("Changing control %s is unsupported", name);
		}
		return -1;
	}
	return 0;
}

static void _device_set_control(struct device_t *dev, struct v4l2_queryctrl *query, const char *name, unsigned cid, int value, bool quiet) {
	struct v4l2_control ctl;

	if (value < query->minimum || value > query->maximum || value % query->step != 0) {
		if (!quiet) {
			LOG_ERROR("Invalid value %d of control %s: min=%d, max=%d, default=%d, step=%u",
				value, name, query->minimum, query->maximum, query->default_value, query->step);
		}
		return;
	}

	MEMSET_ZERO(ctl);
	ctl.id = cid;
	ctl.value = value;

	if (xioctl(dev->run->fd, VIDIOC_S_CTRL, &ctl) < 0) {
		if (!quiet) {
			LOG_PERROR("Can't set control %s", name);
		}
	} else if (!quiet) {
		LOG_INFO("Applying control %s: %d", name, ctl.value);
	}
}

static const char *_format_to_string_fourcc(char *buf, size_t size, unsigned format) {
	assert(size >= 8);
	buf[0] = format & 0x7F;
	buf[1] = (format >> 8) & 0x7F;
	buf[2] = (format >> 16) & 0x7F;
	buf[3] = (format >> 24) & 0x7F;
	if (format & ((unsigned)1 << 31)) {
		buf[4] = '-';
		buf[5] = 'B';
		buf[6] = 'E';
		buf[7] = '\0';
	} else {
		buf[4] = '\0';
	}
	return buf;
}

static const char *_format_to_string_nullable(unsigned format) {
    for (unsigned index = 0; index < ARRAY_LEN(_FORMATS); ++index) {
		if (format == _FORMATS[index].format) {
			return _FORMATS[index].name;
		}
    }
    return NULL;
}

static const char *_format_to_string_supported(unsigned format) {
	const char *format_str = _format_to_string_nullable(format);
	return (format_str == NULL ? "unsupported" : format_str);
}

static const char *_standard_to_string(v4l2_std_id standard) {
	for (unsigned index = 0; index < ARRAY_LEN(_STANDARDS); ++index) {
		if (standard == _STANDARDS[index].standard) {
			return _STANDARDS[index].name;
		}
	}
	return _STANDARDS[0].name;
}

static const char *_io_method_to_string_supported(enum v4l2_memory io_method) {
	for (unsigned index = 0; index < ARRAY_LEN(_IO_METHODS); ++index) {
		if (io_method == _IO_METHODS[index].io_method) {
			return _IO_METHODS[index].name;
		}
	}
	return "unsupported";
}
