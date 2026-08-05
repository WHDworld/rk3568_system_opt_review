#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

#define MAX_PLANES VIDEO_MAX_PLANES

struct mapped_plane {
	void *addr;
	size_t length;
};

struct mapped_buffer {
	struct mapped_plane planes[MAX_PLANES];
	unsigned int num_planes;
};

struct options {
	const char *device;
	const char *output;
	const char *csv;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	unsigned int buffers;
	uint64_t frames;
	unsigned int fps_num;
	unsigned int fps_den;
	int timeout_ms;
	unsigned int log_every;
	bool enumerate_only;
	bool preview;
	unsigned int preview_width;
	unsigned int preview_height;
};

#ifdef HAVE_X11
struct preview {
	Display *display;
	Window window;
	GC gc;
	Atom wm_delete;
	XImage *image;
	uint8_t *pixels;
	unsigned int width;
	unsigned int height;
	uint64_t title_frames;
	uint64_t title_start_ns;
};
#endif

struct capture {
	int fd;
	enum v4l2_buf_type type;
	bool multiplanar;
	bool streaming;
	struct mapped_buffer *buffers;
	unsigned int buffer_count;
	unsigned int num_planes;
	FILE *output;
	FILE *csv;
	uint32_t width;
	uint32_t height;
	uint32_t pixfmt;
	uint32_t bytesperline;
#ifdef HAVE_X11
	struct preview preview;
#endif
};

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, request, arg);
	} while (ret < 0 && errno == EINTR && !stop_requested);
	return ret;
}

static uint64_t timespec_ns(const struct timespec *ts)
{
	return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static uint64_t timeval_ns(const struct timeval *tv)
{
	return (uint64_t)tv->tv_sec * 1000000000ULL + (uint64_t)tv->tv_usec * 1000ULL;
}

static void fourcc_string(uint32_t fmt, char text[5])
{
	text[0] = fmt & 0xff;
	text[1] = (fmt >> 8) & 0xff;
	text[2] = (fmt >> 16) & 0xff;
	text[3] = (fmt >> 24) & 0xff;
	text[4] = '\0';
}

static uint32_t parse_fourcc(const char *text)
{
	if (strlen(text) != 4) {
		fprintf(stderr, "pixel format must contain exactly four characters\n");
		exit(EXIT_FAILURE);
	}
	return v4l2_fourcc(text[0], text[1], text[2], text[3]);
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d, --device PATH       video node (default /dev/video0)\n"
		"  -W, --width N           requested width (default 1920)\n"
		"  -H, --height N          requested height (default 1080)\n"
		"  -p, --pixfmt FOURCC     requested format (default NV12)\n"
		"  -b, --buffers N         requested MMAP buffers (default 4)\n"
		"  -n, --frames N          frames to capture (default 300)\n"
		"  -r, --fps N/D           requested frame rate (default 30/1)\n"
		"  -o, --output FILE       concatenate payloads into FILE\n"
		"  -c, --csv FILE          save per-frame diagnostics as CSV\n"
		"  -t, --timeout MS        poll timeout (default 2000)\n"
		"  -l, --log-every N       print every N frames (default 30)\n"
		"  -P, --preview           show an X11 preview window\n"
		"      --preview-size WxH  window size (default 640x360)\n"
		"  -e, --enumerate         enumerate capabilities and exit\n"
		"  -h, --help              show this help\n",
		program);
}

static struct options parse_options(int argc, char **argv)
{
	struct options opt = {
		.device = "/dev/video0",
		.width = 1920,
		.height = 1080,
		.pixfmt = V4L2_PIX_FMT_NV12,
		.buffers = 4,
		.frames = 300,
		.fps_num = 30,
		.fps_den = 1,
		.timeout_ms = 2000,
		.log_every = 30,
		.preview_width = 640,
		.preview_height = 360,
	};
	static const struct option long_options[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "width", required_argument, NULL, 'W' },
		{ "height", required_argument, NULL, 'H' },
		{ "pixfmt", required_argument, NULL, 'p' },
		{ "buffers", required_argument, NULL, 'b' },
		{ "frames", required_argument, NULL, 'n' },
		{ "fps", required_argument, NULL, 'r' },
		{ "output", required_argument, NULL, 'o' },
		{ "csv", required_argument, NULL, 'c' },
		{ "timeout", required_argument, NULL, 't' },
		{ "log-every", required_argument, NULL, 'l' },
		{ "preview", no_argument, NULL, 'P' },
		{ "preview-size", required_argument, NULL, 1000 },
		{ "enumerate", no_argument, NULL, 'e' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int ch;

	while ((ch = getopt_long(argc, argv, "d:W:H:p:b:n:r:o:c:t:l:Peh",
				 long_options, NULL)) != -1) {
		switch (ch) {
		case 'd': opt.device = optarg; break;
		case 'W': opt.width = strtoul(optarg, NULL, 0); break;
		case 'H': opt.height = strtoul(optarg, NULL, 0); break;
		case 'p': opt.pixfmt = parse_fourcc(optarg); break;
		case 'b': opt.buffers = strtoul(optarg, NULL, 0); break;
		case 'n': opt.frames = strtoull(optarg, NULL, 0); break;
		case 'r':
			if (sscanf(optarg, "%u/%u", &opt.fps_num, &opt.fps_den) != 2 ||
			    !opt.fps_num || !opt.fps_den) {
				fprintf(stderr, "invalid fps; use N/D, for example 30/1\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'o': opt.output = optarg; break;
		case 'c': opt.csv = optarg; break;
		case 't': opt.timeout_ms = strtol(optarg, NULL, 0); break;
		case 'l': opt.log_every = strtoul(optarg, NULL, 0); break;
		case 'P': opt.preview = true; break;
		case 1000:
			if (sscanf(optarg, "%ux%u", &opt.preview_width,
				   &opt.preview_height) != 2 || !opt.preview_width ||
			    !opt.preview_height) {
				fprintf(stderr, "invalid preview size; use WxH\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'e': opt.enumerate_only = true; break;
		case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
		default: usage(argv[0]); exit(EXIT_FAILURE);
		}
	}
	if (opt.buffers < 2 || !opt.frames || opt.timeout_ms <= 0) {
		fprintf(stderr, "buffers must be >= 2, frames and timeout must be > 0\n");
		exit(EXIT_FAILURE);
	}
#ifndef HAVE_X11
	if (opt.preview) {
		fprintf(stderr, "this binary was built without X11 preview support\n");
		exit(EXIT_FAILURE);
	}
#endif
	return opt;
}

static int query_capabilities(struct capture *cap)
{
	struct v4l2_capability info = { 0 };
	uint32_t device_caps;

	if (xioctl(cap->fd, VIDIOC_QUERYCAP, &info) < 0) {
		perror("VIDIOC_QUERYCAP");
		return -1;
	}
	device_caps = info.capabilities & V4L2_CAP_DEVICE_CAPS ?
		info.device_caps : info.capabilities;
	printf("QUERYCAP driver=%s card=%s bus=%s version=%u.%u.%u caps=0x%08x\n",
		info.driver, info.card, info.bus_info,
		(info.version >> 16) & 0xff, (info.version >> 8) & 0xff,
		info.version & 0xff, device_caps);
	if (!(device_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "device does not support streaming I/O\n");
		return -1;
	}
	if (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
		cap->multiplanar = true;
		cap->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	} else if (device_caps & V4L2_CAP_VIDEO_CAPTURE) {
		cap->multiplanar = false;
		cap->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	} else {
		fprintf(stderr, "device is not a capture node\n");
		return -1;
	}
	printf("API type=%s\n", cap->multiplanar ? "multi-planar" : "single-planar");
	return 0;
}

static void enumerate_capabilities(struct capture *cap)
{
	struct v4l2_fmtdesc desc = { .type = cap->type };

	printf("=== VIDIOC_ENUM_FMT / FRAMESIZES / FRAMEINTERVALS ===\n");
	for (desc.index = 0; xioctl(cap->fd, VIDIOC_ENUM_FMT, &desc) == 0;
	     desc.index++) {
		struct v4l2_frmsizeenum size = { .pixel_format = desc.pixelformat };
		char fourcc[5];

		fourcc_string(desc.pixelformat, fourcc);
		printf("format[%u]=%s description=%s flags=0x%x\n", desc.index,
		       fourcc, desc.description, desc.flags);
		for (size.index = 0; xioctl(cap->fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0;
		     size.index++) {
			if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				struct v4l2_frmivalenum interval = {
					.pixel_format = desc.pixelformat,
					.width = size.discrete.width,
					.height = size.discrete.height,
				};
				printf("  size[%u]=%ux%u discrete\n", size.index,
				       size.discrete.width, size.discrete.height);
				for (interval.index = 0;
				     xioctl(cap->fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0;
				     interval.index++) {
					if (interval.type == V4L2_FRMIVAL_TYPE_DISCRETE)
						printf("    interval[%u]=%u/%u s\n", interval.index,
						       interval.discrete.numerator,
						       interval.discrete.denominator);
					else
						break;
				}
			} else if (size.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
				   size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
				printf("  size=stepwise min=%ux%u max=%ux%u step=%ux%u\n",
				       size.stepwise.min_width, size.stepwise.min_height,
				       size.stepwise.max_width, size.stepwise.max_height,
				       size.stepwise.step_width, size.stepwise.step_height);
				break;
			}
		}
	}
	if (errno != EINVAL)
		perror("VIDIOC_ENUM_FMT");
}

static void print_format(const char *label, const struct v4l2_format *fmt,
			 bool multiplanar)
{
	char fourcc[5];
	unsigned int i;

	if (multiplanar) {
		const struct v4l2_pix_format_mplane *pix = &fmt->fmt.pix_mp;
		fourcc_string(pix->pixelformat, fourcc);
		printf("%s actual=%ux%u fourcc=%s field=%u planes=%u colorspace=%u "
		       "ycbcr_enc=%u quantization=%u xfer_func=%u\n",
		       label, pix->width, pix->height, fourcc, pix->field,
		       pix->num_planes, pix->colorspace, pix->ycbcr_enc,
		       pix->quantization, pix->xfer_func);
		for (i = 0; i < pix->num_planes; i++)
			printf("  plane[%u] bytesperline=%u sizeimage=%u\n", i,
			       pix->plane_fmt[i].bytesperline,
			       pix->plane_fmt[i].sizeimage);
	} else {
		const struct v4l2_pix_format *pix = &fmt->fmt.pix;
		fourcc_string(pix->pixelformat, fourcc);
		printf("%s actual=%ux%u fourcc=%s field=%u bytesperline=%u "
		       "sizeimage=%u colorspace=%u ycbcr_enc=%u quantization=%u "
		       "xfer_func=%u\n", label, pix->width, pix->height, fourcc,
		       pix->field, pix->bytesperline, pix->sizeimage,
		       pix->colorspace, pix->ycbcr_enc, pix->quantization,
		       pix->xfer_func);
	}
}

static int configure_format(struct capture *cap, const struct options *opt)
{
	struct v4l2_format fmt = { .type = cap->type };

	if (xioctl(cap->fd, VIDIOC_G_FMT, &fmt) < 0) {
		perror("VIDIOC_G_FMT");
		return -1;
	}
	print_format("G_FMT", &fmt, cap->multiplanar);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = cap->type;
	if (cap->multiplanar) {
		fmt.fmt.pix_mp.width = opt->width;
		fmt.fmt.pix_mp.height = opt->height;
		fmt.fmt.pix_mp.pixelformat = opt->pixfmt;
		fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	} else {
		fmt.fmt.pix.width = opt->width;
		fmt.fmt.pix.height = opt->height;
		fmt.fmt.pix.pixelformat = opt->pixfmt;
		fmt.fmt.pix.field = V4L2_FIELD_ANY;
	}
	if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		return -1;
	}
	print_format("S_FMT", &fmt, cap->multiplanar);
	cap->num_planes = cap->multiplanar ? fmt.fmt.pix_mp.num_planes : 1;
	if (cap->multiplanar) {
		cap->width = fmt.fmt.pix_mp.width;
		cap->height = fmt.fmt.pix_mp.height;
		cap->pixfmt = fmt.fmt.pix_mp.pixelformat;
		cap->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	} else {
		cap->width = fmt.fmt.pix.width;
		cap->height = fmt.fmt.pix.height;
		cap->pixfmt = fmt.fmt.pix.pixelformat;
		cap->bytesperline = fmt.fmt.pix.bytesperline;
	}
	if (!cap->num_planes || cap->num_planes > MAX_PLANES) {
		fprintf(stderr, "invalid plane count %u\n", cap->num_planes);
		return -1;
	}
	return 0;
}

#ifdef HAVE_X11
static uint8_t clamp_u8(int value)
{
	if (value < 0)
		return 0;
	if (value > 255)
		return 255;
	return value;
}

static int preview_init(struct capture *cap, const struct options *opt)
{
	struct preview *preview = &cap->preview;
	int screen;
	Visual *visual;
	int depth;

	if (!opt->preview)
		return 0;
	if (cap->pixfmt != V4L2_PIX_FMT_NV12 || cap->num_planes != 1) {
		fprintf(stderr, "X11 preview currently requires one-plane NV12\n");
		return -1;
	}
	preview->display = XOpenDisplay(NULL);
	if (!preview->display) {
		fprintf(stderr, "XOpenDisplay failed; check DISPLAY and Xauthority\n");
		return -1;
	}
	preview->width = opt->preview_width;
	preview->height = opt->preview_height;
	screen = DefaultScreen(preview->display);
	visual = DefaultVisual(preview->display, screen);
	depth = DefaultDepth(preview->display, screen);
	preview->window = XCreateSimpleWindow(preview->display,
		RootWindow(preview->display, screen), 20, 20, preview->width,
		preview->height, 1, BlackPixel(preview->display, screen),
		BlackPixel(preview->display, screen));
	XSelectInput(preview->display, preview->window,
		ExposureMask | KeyPressMask | StructureNotifyMask);
	preview->wm_delete = XInternAtom(preview->display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(preview->display, preview->window, &preview->wm_delete, 1);
	XStoreName(preview->display, preview->window, "OV5695 V4L2 Preview");
	XMapRaised(preview->display, preview->window);
	preview->gc = XCreateGC(preview->display, preview->window, 0, NULL);
	preview->pixels = calloc((size_t)preview->width * preview->height, 4);
	if (!preview->pixels) {
		perror("calloc preview pixels");
		return -1;
	}
	preview->image = XCreateImage(preview->display, visual, depth, ZPixmap, 0,
		(char *)preview->pixels, preview->width, preview->height, 32, 0);
	if (!preview->image) {
		fprintf(stderr, "XCreateImage failed\n");
		return -1;
	}
	XSync(preview->display, False);
	printf("PREVIEW X11 window=%ux%u source=%ux%u stride=%u\n",
	       preview->width, preview->height, cap->width, cap->height,
	       cap->bytesperline);
	return 0;
}

static bool preview_events(struct preview *preview)
{
	while (XPending(preview->display)) {
		XEvent event;
		XNextEvent(preview->display, &event);
		if (event.type == ClientMessage &&
		    (Atom)event.xclient.data.l[0] == preview->wm_delete)
			return false;
		if (event.type == KeyPress) {
			char text[8] = { 0 };
			KeySym key;
			XLookupString(&event.xkey, text, sizeof(text), &key, NULL);
			if (key == XK_Escape || text[0] == 'q' || text[0] == 'Q')
				return false;
		}
	}
	return true;
}

static int preview_frame(struct capture *cap, const uint8_t *nv12,
			 size_t bytesused, uint64_t frames, uint32_t sequence,
			 uint64_t forward_missing, uint64_t duplicates,
			 uint64_t regressions)
{
	struct preview *preview = &cap->preview;
	size_t y_size = (size_t)cap->bytesperline * cap->height;
	const uint8_t *uv;
	unsigned int x, y;
	struct timespec now;
	uint64_t now_ns;

	if (!preview->display)
		return 0;
	if (!preview_events(preview)) {
		stop_requested = 1;
		return 0;
	}
	if (bytesused < y_size + (size_t)cap->bytesperline * cap->height / 2) {
		fprintf(stderr, "preview payload too small: %zu\n", bytesused);
		return -1;
	}
	uv = nv12 + y_size;
	for (y = 0; y < preview->height; y++) {
		unsigned int sy = (uint64_t)y * cap->height / preview->height;
		const uint8_t *y_row = nv12 + (size_t)sy * cap->bytesperline;
		const uint8_t *uv_row = uv + (size_t)(sy / 2) * cap->bytesperline;
		uint32_t *dst = (uint32_t *)preview->pixels +
			(size_t)y * preview->width;
		for (x = 0; x < preview->width; x++) {
			unsigned int sx = (uint64_t)x * cap->width / preview->width;
			int yy = y_row[sx];
			int u = uv_row[sx & ~1U] - 128;
			int v = uv_row[(sx & ~1U) + 1] - 128;
			int r = yy + ((359 * v) >> 8);
			int g = yy - ((88 * u + 183 * v) >> 8);
			int b = yy + ((454 * u) >> 8);
			dst[x] = (uint32_t)clamp_u8(b) |
				 ((uint32_t)clamp_u8(g) << 8) |
				 ((uint32_t)clamp_u8(r) << 16);
		}
	}
	XPutImage(preview->display, preview->window, preview->gc, preview->image,
		  0, 0, 0, 0, preview->width, preview->height);
	XFlush(preview->display);
	clock_gettime(CLOCK_MONOTONIC, &now);
	now_ns = timespec_ns(&now);
	if (!preview->title_start_ns) {
		preview->title_start_ns = now_ns;
		preview->title_frames = frames;
	} else if (now_ns - preview->title_start_ns >= 1000000000ULL) {
		char title[256];
		double fps = (frames - preview->title_frames) * 1000000000.0 /
			(now_ns - preview->title_start_ns);
		snprintf(title, sizeof(title),
			 "OV5695 %ux%u NV12 | FPS %.2f | frame %llu seq %u | F/D/R %llu/%llu/%llu",
			 cap->width, cap->height, fps, (unsigned long long)frames,
			 sequence, (unsigned long long)forward_missing,
			 (unsigned long long)duplicates,
			 (unsigned long long)regressions);
		XStoreName(preview->display, preview->window, title);
		preview->title_start_ns = now_ns;
		preview->title_frames = frames;
	}
	return 0;
}
#endif

static void configure_frame_rate(struct capture *cap, const struct options *opt)
{
	struct v4l2_streamparm parm = { .type = cap->type };

	if (xioctl(cap->fd, VIDIOC_G_PARM, &parm) < 0) {
		if (errno != ENOTTY && errno != EINVAL)
			perror("VIDIOC_G_PARM");
		else
			printf("G_PARM unsupported by this node\n");
		return;
	}
	printf("G_PARM capability=0x%x timeperframe=%u/%u s\n",
		parm.parm.capture.capability,
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
	parm.parm.capture.timeperframe.numerator = opt->fps_den;
	parm.parm.capture.timeperframe.denominator = opt->fps_num;
	if (xioctl(cap->fd, VIDIOC_S_PARM, &parm) < 0) {
		if (errno != ENOTTY && errno != EINVAL)
			perror("VIDIOC_S_PARM");
		else
			printf("S_PARM unsupported by this node\n");
		return;
	}
	printf("S_PARM actual timeperframe=%u/%u s\n",
		parm.parm.capture.timeperframe.numerator,
		parm.parm.capture.timeperframe.denominator);
}

static int request_and_map_buffers(struct capture *cap, unsigned int requested)
{
	struct v4l2_requestbuffers req = {
		.count = requested,
		.type = cap->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i, p;

	if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	if (req.count < 2) {
		fprintf(stderr, "driver returned only %u buffers\n", req.count);
		return -1;
	}
	printf("REQBUFS requested=%u actual=%u\n", requested, req.count);
	cap->buffers = calloc(req.count, sizeof(*cap->buffers));
	if (!cap->buffers) {
		perror("calloc buffers");
		return -1;
	}
	cap->buffer_count = req.count;

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer buf = {
			.type = cap->type,
			.memory = V4L2_MEMORY_MMAP,
			.index = i,
		};
		struct v4l2_plane planes[MAX_PLANES] = { 0 };

		if (cap->multiplanar) {
			buf.m.planes = planes;
			buf.length = cap->num_planes;
		}
		if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			return -1;
		}
		cap->buffers[i].num_planes = cap->num_planes;
		for (p = 0; p < cap->num_planes; p++) {
			off_t offset;
			size_t length;

			if (cap->multiplanar) {
				offset = planes[p].m.mem_offset;
				length = planes[p].length;
			} else {
				offset = buf.m.offset;
				length = buf.length;
			}
			cap->buffers[i].planes[p].length = length;
			cap->buffers[i].planes[p].addr = mmap(NULL, length,
				PROT_READ | PROT_WRITE, MAP_SHARED, cap->fd, offset);
			if (cap->buffers[i].planes[p].addr == MAP_FAILED) {
				cap->buffers[i].planes[p].addr = NULL;
				perror("mmap");
				return -1;
			}
			printf("MMAP buffer=%u plane=%u length=%zu offset=%lld\n",
			       i, p, length, (long long)offset);
		}
	}
	return 0;
}

static int queue_buffer(struct capture *cap, unsigned int index)
{
	struct v4l2_buffer buf = {
		.type = cap->type,
		.memory = V4L2_MEMORY_MMAP,
		.index = index,
	};
	struct v4l2_plane planes[MAX_PLANES] = { 0 };
	unsigned int p;

	if (cap->multiplanar) {
		buf.m.planes = planes;
		buf.length = cap->num_planes;
		for (p = 0; p < cap->num_planes; p++)
			planes[p].length = cap->buffers[index].planes[p].length;
	}
	if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
		perror("VIDIOC_QBUF");
		return -1;
	}
	return 0;
}

static int write_payload(struct capture *cap, const struct v4l2_buffer *buf,
			 const struct v4l2_plane planes[MAX_PLANES],
			 uint64_t *total_bytes)
{
	unsigned int p;

	for (p = 0; p < cap->num_planes; p++) {
		const uint8_t *base = cap->buffers[buf->index].planes[p].addr;
		size_t offset = cap->multiplanar ? planes[p].data_offset : 0;
		size_t used = cap->multiplanar ? planes[p].bytesused : buf->bytesused;

		if (used < offset || used > cap->buffers[buf->index].planes[p].length) {
			fprintf(stderr, "invalid payload buffer=%u plane=%u used=%zu "
				"offset=%zu mapped=%zu\n", buf->index, p, used, offset,
				cap->buffers[buf->index].planes[p].length);
			return -1;
		}
		used -= offset;
		*total_bytes += used;
		if (cap->output && fwrite(base + offset, 1, used, cap->output) != used) {
			perror("fwrite output");
			return -1;
		}
	}
	return 0;
}

static int run_capture(struct capture *cap, const struct options *opt)
{
	uint64_t frame_no = 0, forward_missing = 0, duplicates = 0;
	uint64_t regressions = 0, total_bytes = 0;
	uint64_t poll_timeouts = 0, dq_eagain = 0;
	uint32_t first_sequence = 0, previous_sequence = 0;
	uint64_t previous_driver_ns = 0;
	struct timespec first_mono = { 0 }, last_mono = { 0 };
	unsigned int i;

	for (i = 0; i < cap->buffer_count; i++)
		if (queue_buffer(cap, i) < 0)
			return -1;
	if (xioctl(cap->fd, VIDIOC_STREAMON, &cap->type) < 0) {
		perror("VIDIOC_STREAMON");
		return -1;
	}
	cap->streaming = true;
	printf("STREAMON success target_frames=%llu timeout_ms=%d\n",
	       (unsigned long long)opt->frames, opt->timeout_ms);
	if (cap->csv)
		fprintf(cap->csv, "frame,sequence,index,driver_timestamp_ns,"
			"dq_monotonic_ns,bytesused,inter_frame_ms,forward_missing,"
			"duplicate,regression,flags\n");

	while (!stop_requested && frame_no < opt->frames) {
		struct pollfd pfd = { .fd = cap->fd, .events = POLLIN | POLLPRI };
		struct v4l2_buffer buf = {
			.type = cap->type,
			.memory = V4L2_MEMORY_MMAP,
		};
		struct v4l2_plane planes[MAX_PLANES] = { 0 };
		struct timespec now;
		uint64_t driver_ns, now_ns, bytesused = 0, missing = 0;
		bool duplicate = false, regression = false;
		double interval_ms = 0.0;
		int ret;
		unsigned int p;

		do {
			ret = poll(&pfd, 1, opt->timeout_ms);
		} while (ret < 0 && errno == EINTR && !stop_requested);
		if (stop_requested)
			break;
		if (ret < 0) {
			perror("poll");
			return -1;
		}
		if (ret == 0) {
			poll_timeouts++;
			fprintf(stderr, "poll timeout #%llu after %d ms\n",
				(unsigned long long)poll_timeouts, opt->timeout_ms);
			continue;
		}
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			fprintf(stderr, "poll error revents=0x%x\n", pfd.revents);
			return -1;
		}
		if (cap->multiplanar) {
			buf.m.planes = planes;
			buf.length = cap->num_planes;
		}
		if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN) {
				dq_eagain++;
				continue;
			}
			if (errno == EINTR && stop_requested)
				break;
			perror("VIDIOC_DQBUF");
			return -1;
		}
		if (buf.index >= cap->buffer_count) {
			fprintf(stderr, "driver returned invalid buffer index %u\n", buf.index);
			return -1;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		now_ns = timespec_ns(&now);
		driver_ns = timeval_ns(&buf.timestamp);
		if (!frame_no) {
			first_mono = now;
			first_sequence = buf.sequence;
		}
		if (frame_no) {
			interval_ms = (driver_ns - previous_driver_ns) / 1000000.0;
			if (buf.sequence == previous_sequence) {
				duplicate = true;
				duplicates++;
			} else if (buf.sequence < previous_sequence) {
				regression = true;
				regressions++;
			} else if (buf.sequence > previous_sequence + 1) {
				missing = buf.sequence - previous_sequence - 1;
				forward_missing += missing;
			}
		}
		for (p = 0; p < cap->num_planes; p++)
			bytesused += cap->multiplanar ? planes[p].bytesused : buf.bytesused;
		if (write_payload(cap, &buf, planes, &total_bytes) < 0)
			return -1;
#ifdef HAVE_X11
		if (cap->preview.display) {
			const uint8_t *preview_data =
				cap->buffers[buf.index].planes[0].addr;
			size_t preview_offset = cap->multiplanar ?
				planes[0].data_offset : 0;
			size_t preview_used = cap->multiplanar ?
				planes[0].bytesused : buf.bytesused;
			if (preview_used < preview_offset ||
			    preview_frame(cap, preview_data + preview_offset,
				preview_used - preview_offset, frame_no + 1,
				buf.sequence, forward_missing, duplicates,
				regressions) < 0)
				return -1;
		}
#endif
		if (cap->csv)
			fprintf(cap->csv, "%llu,%u,%u,%llu,%llu,%llu,%.3f,%llu,%u,%u,0x%x\n",
				(unsigned long long)frame_no, buf.sequence, buf.index,
				(unsigned long long)driver_ns, (unsigned long long)now_ns,
				(unsigned long long)bytesused, interval_ms,
				(unsigned long long)missing, duplicate ? 1U : 0U,
				regression ? 1U : 0U, buf.flags);
		if (!frame_no || (opt->log_every && (frame_no + 1) % opt->log_every == 0) ||
		    frame_no + 1 == opt->frames || missing || duplicate || regression)
			printf("FRAME number=%llu sequence=%u index=%u driver_ts_ns=%llu "
			       "dq_mono_ns=%llu bytesused=%llu interval_ms=%.3f "
			       "forward_missing=%llu duplicate=%u regression=%u flags=0x%x\n",
			       (unsigned long long)frame_no, buf.sequence,
			       buf.index, (unsigned long long)driver_ns,
			       (unsigned long long)now_ns, (unsigned long long)bytesused,
			       interval_ms, (unsigned long long)missing, duplicate ? 1U : 0U,
			       regression ? 1U : 0U, buf.flags);
		previous_sequence = buf.sequence;
		previous_driver_ns = driver_ns;
		last_mono = now;
		frame_no++;
		if (queue_buffer(cap, buf.index) < 0)
			return -1;
	}

	{
		double seconds = 0.0, fps = 0.0;
		if (frame_no > 1)
			seconds = (timespec_ns(&last_mono) - timespec_ns(&first_mono)) /
				1000000000.0;
		if (seconds > 0.0)
			fps = (frame_no - 1) / seconds;
		printf("SUMMARY frames=%llu first_sequence=%u last_sequence=%u "
		       "forward_missing=%llu duplicates=%llu regressions=%llu "
		       "poll_timeouts=%llu dq_eagain=%llu "
		       "total_bytes=%llu elapsed_s=%.6f fps=%.6f interrupted=%u\n",
		       (unsigned long long)frame_no,
		       frame_no ? first_sequence : 0,
		       frame_no ? previous_sequence : 0,
		       (unsigned long long)forward_missing,
		       (unsigned long long)duplicates,
		       (unsigned long long)regressions,
		       (unsigned long long)poll_timeouts,
		       (unsigned long long)dq_eagain,
		       (unsigned long long)total_bytes, seconds, fps,
		       stop_requested ? 1U : 0U);
	}
	return 0;
}

static void cleanup(struct capture *cap)
{
	unsigned int i, p;

	if (cap->streaming) {
		if (xioctl(cap->fd, VIDIOC_STREAMOFF, &cap->type) < 0)
			perror("VIDIOC_STREAMOFF");
		else
			printf("STREAMOFF success\n");
		cap->streaming = false;
	}
	for (i = 0; i < cap->buffer_count; i++) {
		for (p = 0; p < cap->buffers[i].num_planes; p++) {
			if (cap->buffers[i].planes[p].addr &&
			    munmap(cap->buffers[i].planes[p].addr,
				   cap->buffers[i].planes[p].length) < 0)
				perror("munmap");
		}
	}
	free(cap->buffers);
#ifdef HAVE_X11
	if (cap->preview.image) {
		XDestroyImage(cap->preview.image);
		cap->preview.image = NULL;
		cap->preview.pixels = NULL;
	} else {
		free(cap->preview.pixels);
	}
	if (cap->preview.display) {
		if (cap->preview.gc)
			XFreeGC(cap->preview.display, cap->preview.gc);
		if (cap->preview.window)
			XDestroyWindow(cap->preview.display, cap->preview.window);
		XCloseDisplay(cap->preview.display);
		cap->preview.display = NULL;
	}
#endif
	if (cap->output && fclose(cap->output) != 0)
		perror("fclose output");
	if (cap->csv && fclose(cap->csv) != 0)
		perror("fclose csv");
	if (cap->fd >= 0 && close(cap->fd) < 0)
		perror("close video device");
}

int main(int argc, char **argv)
{
	struct options opt = parse_options(argc, argv);
	struct capture cap = { .fd = -1 };
	struct sigaction action = { .sa_handler = handle_signal };
	int status = EXIT_FAILURE;

	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	cap.fd = open(opt.device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (cap.fd < 0) {
		perror(opt.device);
		goto out;
	}
	if (query_capabilities(&cap) < 0)
		goto out;
	enumerate_capabilities(&cap);
	if (opt.enumerate_only) {
		status = EXIT_SUCCESS;
		goto out;
	}
	if (configure_format(&cap, &opt) < 0)
		goto out;
#ifdef HAVE_X11
	if (preview_init(&cap, &opt) < 0)
		goto out;
#endif
	configure_frame_rate(&cap, &opt);
	if (opt.output) {
		cap.output = fopen(opt.output, "wb");
		if (!cap.output) {
			perror(opt.output);
			goto out;
		}
	}
	if (opt.csv) {
		cap.csv = fopen(opt.csv, "w");
		if (!cap.csv) {
			perror(opt.csv);
			goto out;
		}
	}
	if (request_and_map_buffers(&cap, opt.buffers) < 0)
		goto out;
	status = run_capture(&cap, &opt) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
out:
	cleanup(&cap);
	return status;
}
