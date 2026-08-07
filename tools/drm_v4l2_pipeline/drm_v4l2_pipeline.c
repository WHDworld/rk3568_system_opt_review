#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <dirent.h>
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

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_BUFFERS 8
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum backend { BACKEND_PATTERN, BACKEND_COPY, BACKEND_DMABUF };
enum buffer_state {
	BUF_FREE,
	BUF_CAPTURE_QUEUED,
	BUF_CAPTURE_DONE,
	BUF_DISPLAY_PENDING,
	BUF_DISPLAYED,
};

struct options {
	const char *drm_device;
	const char *video_device;
	enum backend backend;
	uint32_t width;
	uint32_t height;
	unsigned int buffers;
	uint64_t frames;
	unsigned int pattern_seconds;
	unsigned int log_every;
};

struct property_ids {
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t crtc_x;
	uint32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
};

struct dumb_buffer {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t pitch;
	uint64_t size;
	void *map;
};

struct drm_context {
	int fd;
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeCrtc *old_crtc;
	drmModeModeInfo mode;
	uint32_t connector_id;
	uint32_t crtc_id;
	unsigned int crtc_index;
	uint32_t primary_plane_id;
	uint32_t overlay_plane_id;
	uint32_t mode_blob_id;
	uint32_t conn_crtc_prop;
	uint32_t crtc_mode_prop;
	uint32_t crtc_active_prop;
	struct property_ids primary_props;
	struct property_ids overlay_props;
	struct dumb_buffer primary;
};

struct capture_buffer {
	void *map;
	size_t map_length;
	int dmabuf_fd;
	uint32_t gem_handle;
	uint32_t fb_id;
	enum buffer_state state;
};

struct copy_buffer {
	struct dumb_buffer dumb;
	bool pending;
};

struct video_context {
	int fd;
	enum v4l2_buf_type type;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t sizeimage;
	unsigned int count;
	struct capture_buffer buffers[MAX_BUFFERS];
	bool streaming;
};

struct app {
	struct options opt;
	struct drm_context drm;
	struct video_context video;
	struct copy_buffer copies[2];
	uint64_t flip_events;
};

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signo)
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

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int count_open_fds(void)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *entry;
	int count = 0;
	if (!dir)
		return -1;
	while ((entry = readdir(dir)) != NULL)
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			count++;
	closedir(dir);
	/* Exclude the directory descriptor used by this function itself. */
	return count - 1;
}

static const char *state_name(enum buffer_state state)
{
	static const char *const names[] = {
		"FREE", "CAPTURE_QUEUED", "CAPTURE_DONE",
		"DISPLAY_PENDING", "DISPLAYED",
	};
	return state < ARRAY_SIZE(names) ? names[state] : "INVALID";
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --backend pattern|copy|dmabuf [options]\n"
		"  -D, --drm-device PATH   default /dev/dri/card0\n"
		"  -d, --video-device PATH default /dev/video0\n"
		"  -W, --width N           default 1280\n"
		"  -H, --height N          default 720\n"
		"  -b, --buffers N         V4L2 buffers, default 4\n"
		"  -n, --frames N          default 300\n"
		"  -s, --seconds N         pattern duration, default 5\n"
		"  -l, --log-every N       default 300\n", program);
}

static struct options parse_options(int argc, char **argv)
{
	struct options opt = {
		.drm_device = "/dev/dri/card0",
		.video_device = "/dev/video0",
		.backend = BACKEND_PATTERN,
		.width = 1280,
		.height = 720,
		.buffers = 4,
		.frames = 300,
		.pattern_seconds = 5,
		.log_every = 300,
	};
	static const struct option long_opts[] = {
		{ "backend", required_argument, NULL, 'B' },
		{ "drm-device", required_argument, NULL, 'D' },
		{ "video-device", required_argument, NULL, 'd' },
		{ "width", required_argument, NULL, 'W' },
		{ "height", required_argument, NULL, 'H' },
		{ "buffers", required_argument, NULL, 'b' },
		{ "frames", required_argument, NULL, 'n' },
		{ "seconds", required_argument, NULL, 's' },
		{ "log-every", required_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int ch;

	while ((ch = getopt_long(argc, argv, "B:D:d:W:H:b:n:s:l:h", long_opts,
				 NULL)) != -1) {
		switch (ch) {
		case 'B':
			if (!strcmp(optarg, "pattern")) opt.backend = BACKEND_PATTERN;
			else if (!strcmp(optarg, "copy")) opt.backend = BACKEND_COPY;
			else if (!strcmp(optarg, "dmabuf")) opt.backend = BACKEND_DMABUF;
			else { usage(argv[0]); exit(EXIT_FAILURE); }
			break;
		case 'D': opt.drm_device = optarg; break;
		case 'd': opt.video_device = optarg; break;
		case 'W': opt.width = strtoul(optarg, NULL, 0); break;
		case 'H': opt.height = strtoul(optarg, NULL, 0); break;
		case 'b': opt.buffers = strtoul(optarg, NULL, 0); break;
		case 'n': opt.frames = strtoull(optarg, NULL, 0); break;
		case 's': opt.pattern_seconds = strtoul(optarg, NULL, 0); break;
		case 'l': opt.log_every = strtoul(optarg, NULL, 0); break;
		case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
		default: usage(argv[0]); exit(EXIT_FAILURE);
		}
	}
	if (!opt.width || !opt.height || opt.buffers < 3 ||
	    opt.buffers > MAX_BUFFERS || !opt.frames) {
		fprintf(stderr, "invalid dimensions, frames, or buffer count (3..8)\n");
		exit(EXIT_FAILURE);
	}
	return opt;
}

static uint32_t get_property_id(int fd, uint32_t object_id,
				uint32_t object_type, const char *name,
				uint64_t *current)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object_id,
		object_type);
	uint32_t result = 0;
	uint32_t i;

	if (!props)
		return 0;
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
		if (prop && !strcmp(prop->name, name)) {
			result = prop->prop_id;
			if (current)
				*current = props->prop_values[i];
		}
		drmModeFreeProperty(prop);
		if (result)
			break;
	}
	drmModeFreeObjectProperties(props);
	return result;
}

static int plane_type(int fd, uint32_t plane_id)
{
	uint64_t value = UINT64_MAX;
	if (!get_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "type", &value))
		return -1;
	return (int)value;
}

static bool plane_supports_format(const drmModePlane *plane, uint32_t format)
{
	uint32_t i;
	for (i = 0; i < plane->count_formats; i++)
		if (plane->formats[i] == format)
			return true;
	return false;
}

static int fill_plane_props(int fd, uint32_t plane_id,
			    struct property_ids *props)
{
#define GET_PLANE_PROP(field, text) \
	do { props->field = get_property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, \
		text, NULL); if (!props->field) return -1; } while (0)
	GET_PLANE_PROP(crtc_id, "CRTC_ID");
	GET_PLANE_PROP(fb_id, "FB_ID");
	GET_PLANE_PROP(src_x, "SRC_X");
	GET_PLANE_PROP(src_y, "SRC_Y");
	GET_PLANE_PROP(src_w, "SRC_W");
	GET_PLANE_PROP(src_h, "SRC_H");
	GET_PLANE_PROP(crtc_x, "CRTC_X");
	GET_PLANE_PROP(crtc_y, "CRTC_Y");
	GET_PLANE_PROP(crtc_w, "CRTC_W");
	GET_PLANE_PROP(crtc_h, "CRTC_H");
#undef GET_PLANE_PROP
	return 0;
}

static int select_drm_objects(struct drm_context *drm, const char *device)
{
	drmModePlaneRes *plane_res;
	uint32_t i;
	int ret;

	drm->fd = open(device, O_RDWR | O_CLOEXEC);
	if (drm->fd < 0) {
		perror("open DRM device");
		return -1;
	}
	ret = drmSetClientCap(drm->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret) { perror("DRM_CLIENT_CAP_UNIVERSAL_PLANES"); return -1; }
	ret = drmSetClientCap(drm->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) { perror("DRM_CLIENT_CAP_ATOMIC"); return -1; }
	printf("DRM_CAP universal_planes=1 atomic=1\n");

	drm->resources = drmModeGetResources(drm->fd);
	if (!drm->resources) { perror("drmModeGetResources"); return -1; }
	for (i = 0; i < (uint32_t)drm->resources->count_connectors; i++) {
		drmModeConnector *conn = drmModeGetConnector(drm->fd,
			drm->resources->connectors[i]);
		if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes) {
			drm->connector = conn;
			break;
		}
		drmModeFreeConnector(conn);
	}
	if (!drm->connector) { fprintf(stderr, "no connected connector\n"); return -1; }
	drm->connector_id = drm->connector->connector_id;
	drm->mode = drm->connector->modes[0];
	if (drm->connector->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(drm->fd,
			drm->connector->encoder_id);
		if (enc) {
			drm->crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	if (!drm->crtc_id) { fprintf(stderr, "connector has no active CRTC\n"); return -1; }
	for (i = 0; i < (uint32_t)drm->resources->count_crtcs; i++)
		if (drm->resources->crtcs[i] == drm->crtc_id)
			drm->crtc_index = i;
	drm->old_crtc = drmModeGetCrtc(drm->fd, drm->crtc_id);

	plane_res = drmModeGetPlaneResources(drm->fd);
	if (!plane_res) { perror("drmModeGetPlaneResources"); return -1; }
	for (i = 0; i < plane_res->count_planes; i++) {
		drmModePlane *plane = drmModeGetPlane(drm->fd, plane_res->planes[i]);
		int type;
		if (!plane || !(plane->possible_crtcs & (1U << drm->crtc_index))) {
			drmModeFreePlane(plane);
			continue;
		}
		type = plane_type(drm->fd, plane->plane_id);
		printf("DRM_PLANE id=%u type=%d possible_crtcs=0x%x nv12=%u\n",
		       plane->plane_id, type, plane->possible_crtcs,
		       plane_supports_format(plane, DRM_FORMAT_NV12));
		if (type == DRM_PLANE_TYPE_PRIMARY && !drm->primary_plane_id)
			drm->primary_plane_id = plane->plane_id;
		if (type == DRM_PLANE_TYPE_OVERLAY &&
		    plane_supports_format(plane, DRM_FORMAT_NV12) &&
		    !drm->overlay_plane_id)
			drm->overlay_plane_id = plane->plane_id;
		drmModeFreePlane(plane);
	}
	drmModeFreePlaneResources(plane_res);
	if (!drm->primary_plane_id || !drm->overlay_plane_id) {
		fprintf(stderr, "required primary/NV12 overlay plane missing\n");
		return -1;
	}
	drm->conn_crtc_prop = get_property_id(drm->fd, drm->connector_id,
		DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	drm->crtc_mode_prop = get_property_id(drm->fd, drm->crtc_id,
		DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	drm->crtc_active_prop = get_property_id(drm->fd, drm->crtc_id,
		DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	if (!drm->conn_crtc_prop || !drm->crtc_mode_prop ||
	    !drm->crtc_active_prop ||
	    fill_plane_props(drm->fd, drm->primary_plane_id, &drm->primary_props) ||
	    fill_plane_props(drm->fd, drm->overlay_plane_id, &drm->overlay_props)) {
		fprintf(stderr, "required atomic property missing\n");
		return -1;
	}
	printf("DRM_SELECT connector=%u mode=%s %ux%u crtc=%u crtc_index=%u "
	       "primary=%u overlay_nv12=%u\n", drm->connector_id, drm->mode.name,
	       drm->mode.hdisplay, drm->mode.vdisplay, drm->crtc_id,
	       drm->crtc_index, drm->primary_plane_id, drm->overlay_plane_id);
	return 0;
}

static int create_dumb(struct drm_context *drm, uint32_t width,
		       uint32_t height, uint32_t bpp, struct dumb_buffer *buffer)
{
	struct drm_mode_create_dumb create = { 0 };
	struct drm_mode_map_dumb map = { 0 };

	create.width = width;
	create.height = height;
	create.bpp = bpp;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}
	buffer->handle = create.handle;
	buffer->pitch = create.pitch;
	buffer->size = create.size;
	map.handle = create.handle;
	if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}
	buffer->map = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE,
		MAP_SHARED, drm->fd, map.offset);
	if (buffer->map == MAP_FAILED) {
		buffer->map = NULL;
		perror("mmap dumb");
		return -1;
	}
	return 0;
}

static void fill_color_bars(struct dumb_buffer *buffer, uint32_t width,
			    uint32_t height)
{
	static const uint32_t colors[] = {
		0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
		0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
	};
	uint32_t x, y;
	for (y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)buffer->map +
			(size_t)y * buffer->pitch);
		for (x = 0; x < width; x++)
			row[x] = colors[(uint64_t)x * ARRAY_SIZE(colors) / width];
	}
}

static int add_atomic_plane(drmModeAtomicReq *req, uint32_t plane_id,
			    const struct property_ids *p, uint32_t crtc_id,
			    uint32_t fb_id, uint32_t src_w, uint32_t src_h,
			    uint32_t dst_w, uint32_t dst_h)
{
#define ADD(prop, value) do { if (drmModeAtomicAddProperty(req, plane_id, \
	(prop), (value)) < 0) return -1; } while (0)
	ADD(p->crtc_id, crtc_id); ADD(p->fb_id, fb_id);
	ADD(p->src_x, 0); ADD(p->src_y, 0);
	ADD(p->src_w, (uint64_t)src_w << 16);
	ADD(p->src_h, (uint64_t)src_h << 16);
	ADD(p->crtc_x, 0); ADD(p->crtc_y, 0);
	ADD(p->crtc_w, dst_w); ADD(p->crtc_h, dst_h);
#undef ADD
	return 0;
}

static int modeset_pattern(struct drm_context *drm)
{
	drmModeAtomicReq *req;
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	int ret;

	if (create_dumb(drm, drm->mode.hdisplay, drm->mode.vdisplay, 32,
			&drm->primary))
		return -1;
	fill_color_bars(&drm->primary, drm->mode.hdisplay, drm->mode.vdisplay);
	handles[0] = drm->primary.handle;
	pitches[0] = drm->primary.pitch;
	ret = drmModeAddFB2(drm->fd, drm->mode.hdisplay, drm->mode.vdisplay,
		DRM_FORMAT_XRGB8888, handles, pitches, offsets,
		&drm->primary.fb_id, 0);
	if (ret) { perror("drmModeAddFB2 pattern"); return -1; }
	if (drmModeCreatePropertyBlob(drm->fd, &drm->mode, sizeof(drm->mode),
				      &drm->mode_blob_id)) {
		perror("drmModeCreatePropertyBlob"); return -1;
	}
	req = drmModeAtomicAlloc();
	if (!req) return -1;
	if (drmModeAtomicAddProperty(req, drm->connector_id, drm->conn_crtc_prop,
				     drm->crtc_id) < 0 ||
	    drmModeAtomicAddProperty(req, drm->crtc_id, drm->crtc_mode_prop,
				     drm->mode_blob_id) < 0 ||
	    drmModeAtomicAddProperty(req, drm->crtc_id, drm->crtc_active_prop, 1) < 0 ||
	    add_atomic_plane(req, drm->primary_plane_id, &drm->primary_props,
			     drm->crtc_id, drm->primary.fb_id,
			     drm->mode.hdisplay, drm->mode.vdisplay,
			     drm->mode.hdisplay, drm->mode.vdisplay)) {
		drmModeAtomicFree(req); return -1;
	}
	ret = drmModeAtomicCommit(drm->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	if (ret) { perror("drmModeAtomicCommit modeset"); return -1; }
	printf("PATTERN displayed fb=%u handle=%u pitch=%u size=%llu\n",
	       drm->primary.fb_id, drm->primary.handle, drm->primary.pitch,
	       (unsigned long long)drm->primary.size);
	return 0;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
			      unsigned int usec, void *data)
{
	struct app *app = data;
	(void)fd; (void)frame; (void)sec; (void)usec;
	app->flip_events++;
}

static int commit_overlay(struct app *app, uint32_t fb_id)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	drmEventContext event = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
	};
	struct pollfd pfd = { .fd = app->drm.fd, .events = POLLIN };
	uint64_t before = app->flip_events;
	int ret;

	if (!req || add_atomic_plane(req, app->drm.overlay_plane_id,
		&app->drm.overlay_props, app->drm.crtc_id, fb_id,
		app->video.width, app->video.height,
		app->drm.mode.hdisplay, app->drm.mode.vdisplay)) {
		drmModeAtomicFree(req); return -1;
	}
	ret = drmModeAtomicCommit(app->drm.fd, req,
		DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, app);
	drmModeAtomicFree(req);
	if (ret) { perror("drmModeAtomicCommit overlay"); return -1; }
	/* Once submitted, always consume the completion event before releasing FBs. */
	while (app->flip_events == before) {
		ret = poll(&pfd, 1, 2000);
		if (ret < 0 && errno == EINTR) continue;
		if (ret <= 0) { fprintf(stderr, "DRM page-flip timeout/error\n"); return -1; }
		if (drmHandleEvent(app->drm.fd, &event)) {
			perror("drmHandleEvent"); return -1;
		}
	}
	return stop_requested ? 1 : 0;
}

static int disable_overlay(struct drm_context *drm)
{
	drmModeAtomicReq *req = drmModeAtomicAlloc();
	int ret;
	if (!req) return -1;
	if (drmModeAtomicAddProperty(req, drm->overlay_plane_id,
			drm->overlay_props.fb_id, 0) < 0 ||
	    drmModeAtomicAddProperty(req, drm->overlay_plane_id,
			drm->overlay_props.crtc_id, 0) < 0) {
		drmModeAtomicFree(req); return -1;
	}
	ret = drmModeAtomicCommit(drm->fd, req, 0, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static int video_init(struct app *app)
{
	struct video_context *video = &app->video;
	struct v4l2_capability cap = { 0 };
	struct v4l2_format fmt = { 0 };
	struct v4l2_requestbuffers req = { 0 };
	unsigned int i;

	video->fd = open(app->opt.video_device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (video->fd < 0) { perror("open video"); return -1; }
	if (xioctl(video->fd, VIDIOC_QUERYCAP, &cap)) { perror("QUERYCAP"); return -1; }
	video->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.type = video->type;
	fmt.fmt.pix_mp.width = app->opt.width;
	fmt.fmt.pix_mp.height = app->opt.height;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
	if (xioctl(video->fd, VIDIOC_S_FMT, &fmt)) { perror("S_FMT"); return -1; }
	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 ||
	    fmt.fmt.pix_mp.num_planes != 1) {
		fprintf(stderr, "expected one physical plane NV12\n"); return -1;
	}
	video->width = fmt.fmt.pix_mp.width;
	video->height = fmt.fmt.pix_mp.height;
	video->stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	video->sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	printf("V4L2_FORMAT width=%u height=%u format=NV12 physical_planes=1 "
	       "drm_image_planes=2 stride=%u sizeimage=%u uv_offset=%u\n",
	       video->width, video->height, video->stride, video->sizeimage,
	       video->stride * video->height);
	req.count = app->opt.buffers;
	req.type = video->type;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(video->fd, VIDIOC_REQBUFS, &req)) { perror("REQBUFS"); return -1; }
	video->count = req.count;
	if (video->count < 3 || video->count > MAX_BUFFERS) return -1;
	for (i = 0; i < video->count; i++) {
		struct v4l2_buffer buf = { 0 };
		struct v4l2_plane plane = { 0 };
		buf.type = video->type; buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i; buf.m.planes = &plane; buf.length = 1;
		if (xioctl(video->fd, VIDIOC_QUERYBUF, &buf)) { perror("QUERYBUF"); return -1; }
		video->buffers[i].map_length = plane.length;
		video->buffers[i].map = mmap(NULL, plane.length, PROT_READ | PROT_WRITE,
			MAP_SHARED, video->fd, plane.m.mem_offset);
		if (video->buffers[i].map == MAP_FAILED) { perror("mmap V4L2"); return -1; }
		video->buffers[i].dmabuf_fd = -1;
		video->buffers[i].state = BUF_FREE;
	}
	return 0;
}

static int create_nv12_dumb(struct app *app, struct copy_buffer *copy)
{
	uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
	if (create_dumb(&app->drm, app->video.width,
			app->video.height * 3 / 2, 8, &copy->dumb))
		return -1;
	handles[0] = handles[1] = copy->dumb.handle;
	pitches[0] = pitches[1] = copy->dumb.pitch;
	offsets[1] = copy->dumb.pitch * app->video.height;
	if (drmModeAddFB2(app->drm.fd, app->video.width, app->video.height,
		DRM_FORMAT_NV12, handles, pitches, offsets,
		&copy->dumb.fb_id, 0)) {
		perror("drmModeAddFB2 copy NV12"); return -1;
	}
	printf("COPY_INIT fb=%u handle=%u pitch=%u uv_offset=%u size=%llu\n",
	       copy->dumb.fb_id, copy->dumb.handle, copy->dumb.pitch,
	       offsets[1], (unsigned long long)copy->dumb.size);
	return 0;
}

static int export_import_buffers(struct app *app)
{
	unsigned int i;
	for (i = 0; i < app->video.count; i++) {
		struct v4l2_exportbuffer exp = { 0 };
		uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };
		struct capture_buffer *buffer = &app->video.buffers[i];
		exp.type = app->video.type; exp.index = i; exp.plane = 0;
		exp.flags = O_CLOEXEC | O_RDWR;
		if (xioctl(app->video.fd, VIDIOC_EXPBUF, &exp)) { perror("EXPBUF"); return -1; }
		buffer->dmabuf_fd = exp.fd;
		if (drmPrimeFDToHandle(app->drm.fd, exp.fd, &buffer->gem_handle)) {
			perror("drmPrimeFDToHandle"); return -1;
		}
		handles[0] = handles[1] = buffer->gem_handle;
		pitches[0] = pitches[1] = app->video.stride;
		offsets[1] = app->video.stride * app->video.height;
		if (drmModeAddFB2(app->drm.fd, app->video.width, app->video.height,
			DRM_FORMAT_NV12, handles, pitches, offsets, &buffer->fb_id, 0)) {
			perror("drmModeAddFB2 imported NV12"); return -1;
		}
		printf("DMABUF_INIT index=%u exp_fd=%d gem_handle=%u fb=%u "
		       "pitches=%u/%u offsets=0/%u modifier=LINEAR\n", i,
		       exp.fd, buffer->gem_handle, buffer->fb_id, pitches[0],
		       pitches[1], offsets[1]);
	}
	return 0;
}

static int queue_capture(struct video_context *video, unsigned int index)
{
	struct v4l2_buffer buf = { 0 };
	struct v4l2_plane plane = { 0 };
	struct capture_buffer *buffer = &video->buffers[index];
	if (buffer->state == BUF_DISPLAY_PENDING || buffer->state == BUF_DISPLAYED) {
		fprintf(stderr, "LIFECYCLE VIOLATION QBUF index=%u state=%s\n",
			index, state_name(buffer->state));
		return -1;
	}
	buf.type = video->type; buf.memory = V4L2_MEMORY_MMAP;
	buf.index = index; buf.m.planes = &plane; buf.length = 1;
	if (xioctl(video->fd, VIDIOC_QBUF, &buf)) { perror("QBUF"); return -1; }
	buffer->state = BUF_CAPTURE_QUEUED;
	return 0;
}

static int copy_nv12(struct app *app, unsigned int capture_index,
		     struct copy_buffer *copy)
{
	const uint8_t *src = app->video.buffers[capture_index].map;
	uint8_t *dst = copy->dumb.map;
	uint32_t y;
	for (y = 0; y < app->video.height; y++)
		memcpy(dst + (size_t)y * copy->dumb.pitch,
		       src + (size_t)y * app->video.stride, app->video.width);
	for (y = 0; y < app->video.height / 2; y++)
		memcpy(dst + (size_t)(app->video.height + y) * copy->dumb.pitch,
		       src + (size_t)(app->video.height + y) * app->video.stride,
		       app->video.width);
	return 0;
}

static int run_camera(struct app *app)
{
	struct video_context *video = &app->video;
	int displayed_capture = -1;
	uint64_t start_ns = 0, end_ns = 0, copy_ns = 0;
	uint64_t frames = 0, forward = 0, duplicate = 0, regression = 0;
	uint32_t first_seq = 0, last_seq = 0;
	unsigned int i, copy_index = 0;
	int steady_fds_start, steady_fds_end;

	if (app->opt.backend == BACKEND_COPY) {
		if (create_nv12_dumb(app, &app->copies[0]) ||
		    create_nv12_dumb(app, &app->copies[1])) return -1;
	} else if (export_import_buffers(app)) return -1;
	for (i = 0; i < video->count; i++)
		if (queue_capture(video, i)) return -1;
	if (xioctl(video->fd, VIDIOC_STREAMON, &video->type)) {
		perror("STREAMON"); return -1;
	}
	video->streaming = true;
	steady_fds_start = count_open_fds();
	printf("STREAMON backend=%s buffers=%u target_frames=%llu steady_fds=%d ",
	       app->opt.backend == BACKEND_COPY ? "copy" : "dmabuf",
	       video->count, (unsigned long long)app->opt.frames,
	       steady_fds_start);
	printf("pid=%d\n", getpid());

	while (!stop_requested && frames < app->opt.frames) {
		struct pollfd pfd = { .fd = video->fd, .events = POLLIN };
		struct v4l2_buffer buf = { 0 };
		struct v4l2_plane plane = { 0 };
		int ret;
		ret = poll(&pfd, 1, 2000);
		if (ret < 0 && errno == EINTR) continue;
		if (ret <= 0) { fprintf(stderr, "V4L2 poll timeout/error\n"); return -1; }
		buf.type = video->type; buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = &plane; buf.length = 1;
		if (xioctl(video->fd, VIDIOC_DQBUF, &buf)) {
			if (errno == EAGAIN) continue;
			perror("DQBUF"); return -1;
		}
		video->buffers[buf.index].state = BUF_CAPTURE_DONE;
		if (!frames) { first_seq = buf.sequence; start_ns = monotonic_ns(); }
		else if (buf.sequence == last_seq) duplicate++;
		else if (buf.sequence < last_seq) regression++;
		else if (buf.sequence > last_seq + 1) forward += buf.sequence-last_seq-1;

		if (app->opt.backend == BACKEND_COPY) {
			uint64_t before = monotonic_ns();
			int commit_ret;
			copy_nv12(app, buf.index, &app->copies[copy_index]);
			copy_ns += monotonic_ns() - before;
			if (queue_capture(video, buf.index)) return -1;
			commit_ret = commit_overlay(app, app->copies[copy_index].dumb.fb_id);
			if (commit_ret < 0) return -1;
			copy_index ^= 1U;
			if (commit_ret > 0) break;
		} else {
			int old = displayed_capture;
			int commit_ret;
			video->buffers[buf.index].state = BUF_DISPLAY_PENDING;
			commit_ret = commit_overlay(app, video->buffers[buf.index].fb_id);
			if (commit_ret < 0) return -1;
			video->buffers[buf.index].state = BUF_DISPLAYED;
			displayed_capture = buf.index;
			if (old >= 0) {
				video->buffers[old].state = BUF_FREE;
				if (queue_capture(video, old)) return -1;
			}
			if (commit_ret > 0) {
				last_seq = buf.sequence;
				frames++;
				end_ns = monotonic_ns();
				break;
			}
		}
		last_seq = buf.sequence;
		frames++;
		end_ns = monotonic_ns();
		if (frames == 1 || (app->opt.log_every && frames % app->opt.log_every == 0))
			printf("FRAME count=%llu sequence=%u index=%u bytesused=%u "
			       "flip_events=%llu F/D/R=%llu/%llu/%llu\n",
			       (unsigned long long)frames, buf.sequence, buf.index,
			       plane.bytesused, (unsigned long long)app->flip_events,
			       (unsigned long long)forward, (unsigned long long)duplicate,
			       (unsigned long long)regression);
	}
	if (disable_overlay(&app->drm)) perror("disable overlay");
	if (displayed_capture >= 0)
		video->buffers[displayed_capture].state = BUF_FREE;
	steady_fds_end = count_open_fds();
	printf("SUMMARY backend=%s frames=%llu first_sequence=%u last_sequence=%u "
	       "forward=%llu duplicate=%llu regression=%llu flip_events=%llu "
	       "elapsed_s=%.6f fps=%.6f copy_total_ms=%.3f copy_avg_us=%.3f "
	       "steady_fds_start=%d steady_fds_end=%d fd_growth=%d interrupted=%u\n",
	       app->opt.backend == BACKEND_COPY ? "copy" : "dmabuf",
	       (unsigned long long)frames, first_seq, last_seq,
	       (unsigned long long)forward, (unsigned long long)duplicate,
	       (unsigned long long)regression, (unsigned long long)app->flip_events,
	       frames > 1 ? (end_ns-start_ns)/1e9 : 0.0,
	       frames > 1 ? (frames-1)*1e9/(end_ns-start_ns) : 0.0,
	       copy_ns/1e6, frames ? copy_ns/1e3/frames : 0.0,
	       steady_fds_start, steady_fds_end,
	       steady_fds_end - steady_fds_start,
	       stop_requested ? 1U : 0U);
	return 0;
}

static void destroy_dumb(struct drm_context *drm, struct dumb_buffer *buffer)
{
	struct drm_mode_destroy_dumb destroy = { 0 };
	if (buffer->fb_id) drmModeRmFB(drm->fd, buffer->fb_id);
	if (buffer->map) munmap(buffer->map, buffer->size);
	if (buffer->handle) {
		destroy.handle = buffer->handle;
		drmIoctl(drm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	memset(buffer, 0, sizeof(*buffer));
}

static void cleanup(struct app *app)
{
	unsigned int i;
	if (app->video.streaming) {
		xioctl(app->video.fd, VIDIOC_STREAMOFF, &app->video.type);
		app->video.streaming = false;
		printf("STREAMOFF success\n");
	}
	for (i = 0; i < app->video.count; i++) {
		struct capture_buffer *buffer = &app->video.buffers[i];
		if (buffer->fb_id) drmModeRmFB(app->drm.fd, buffer->fb_id);
		if (buffer->gem_handle) {
			struct drm_gem_close close_arg = { .handle = buffer->gem_handle };
			drmIoctl(app->drm.fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		}
		if (buffer->dmabuf_fd >= 0) close(buffer->dmabuf_fd);
		if (buffer->map && buffer->map != MAP_FAILED)
			munmap(buffer->map, buffer->map_length);
	}
	if (app->video.fd >= 0) close(app->video.fd);
	destroy_dumb(&app->drm, &app->copies[0].dumb);
	destroy_dumb(&app->drm, &app->copies[1].dumb);
	destroy_dumb(&app->drm, &app->drm.primary);
	if (app->drm.mode_blob_id)
		drmModeDestroyPropertyBlob(app->drm.fd, app->drm.mode_blob_id);
	drmModeFreeCrtc(app->drm.old_crtc);
	drmModeFreeConnector(app->drm.connector);
	drmModeFreeResources(app->drm.resources);
	if (app->drm.fd >= 0) close(app->drm.fd);
}

int main(int argc, char **argv)
{
	struct app app;
	struct sigaction action = { .sa_handler = signal_handler };
	int status = EXIT_FAILURE;

	memset(&app, 0, sizeof(app));
	app.drm.fd = -1;
	app.video.fd = -1;
	app.opt = parse_options(argc, argv);
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	/* The test platform uses card0 for rockchip-drm. */
	if (select_drm_objects(&app.drm, app.opt.drm_device) || modeset_pattern(&app.drm))
		goto out;
	if (app.opt.backend == BACKEND_PATTERN) {
		unsigned int elapsed = 0;
		while (!stop_requested && elapsed < app.opt.pattern_seconds) {
			sleep(1); elapsed++;
		}
		printf("SUMMARY backend=pattern seconds=%u interrupted=%u\n",
		       elapsed, stop_requested ? 1U : 0U);
		status = EXIT_SUCCESS;
		goto out;
	}
	if (video_init(&app) || run_camera(&app))
		goto out;
	status = EXIT_SUCCESS;
out:
	cleanup(&app);
	return status;
}
