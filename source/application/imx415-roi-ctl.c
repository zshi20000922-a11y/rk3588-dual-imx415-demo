// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/v4l2-controls.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define FULL_LEFT 12
#define FULL_TOP 16
#define FULL_WIDTH 3840
#define FULL_HEIGHT 2160
#define ROI_SENSOR_WIDTH 648
#define ROI_WIDTH 640
#define ROI_HEIGHT 640
#define ROI_SAFE_VMAX 1222
#define ROI_EXPERIMENTAL_VMAX 686

enum action {
	ACTION_NONE,
	ACTION_FULL,
	ACTION_ROI,
	ACTION_MOVE,
	ACTION_GET,
	ACTION_SWEEP,
	ACTION_VMAX,
};

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --mode full\n"
		"  %s --mode roi --left X --top Y\n"
		"  %s --move --left X --top Y\n"
		"  %s --get\n"
		"  %s --sweep-fps [--dwell-ms N]\n"
		"  %s --vmax N\n"
		"Coordinates use the visible 3840x2160 full-frame origin.\n",
		program, program, program, program, program, program);
}

static int find_subdev(char *path, size_t path_len, char *entity_name,
		       size_t entity_name_len)
{
	char media_path[64];
	int media_index;

	for (media_index = 0; media_index < 32; media_index++) {
		struct media_entity_desc entity = { .id = MEDIA_ENT_ID_FLAG_NEXT };
		int media_fd;

		snprintf(media_path, sizeof(media_path), "/dev/media%d", media_index);
		media_fd = open(media_path, O_RDWR | O_CLOEXEC);
		if (media_fd < 0)
			continue;

		while (ioctl(media_fd, MEDIA_IOC_ENUM_ENTITIES, &entity) == 0) {
			if (strstr(entity.name, "imx415")) {
				int index;

				for (index = 0; index < 64; index++) {
					struct stat st;

					snprintf(path, path_len, "/dev/v4l-subdev%d", index);
					if (stat(path, &st) == 0 &&
					    major(st.st_rdev) == entity.dev.major &&
					    minor(st.st_rdev) == entity.dev.minor) {
						snprintf(entity_name, entity_name_len, "%s", entity.name);
						close(media_fd);
						return 0;
					}
				}
			}
			entity.id |= MEDIA_ENT_ID_FLAG_NEXT;
		}
		close(media_fd);
	}

	errno = ENODEV;
	return -1;
}

static int set_format(int fd, unsigned int width, unsigned int height)
{
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.format = {
			.width = width,
			.height = height,
			.code = MEDIA_BUS_FMT_SGBRG10_1X10,
			.field = V4L2_FIELD_NONE,
		},
	};

	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &format) < 0)
		return -1;
	if (format.format.width != width || format.format.height != height) {
		errno = ERANGE;
		return -1;
	}
	return 0;
}

static int normalize_coordinate(int value, int maximum)
{
	if (value < 0)
		value = 0;
	if (value > maximum)
		value = maximum;
	return value & ~1;
}

static int set_roi(int fd, int left, int top)
{
	struct v4l2_subdev_selection selection = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.target = V4L2_SEL_TGT_CROP,
	};

	left = normalize_coordinate(left, FULL_WIDTH - ROI_WIDTH);
	top = normalize_coordinate(top, FULL_HEIGHT - ROI_HEIGHT);
	selection.r.left = left + FULL_LEFT;
	selection.r.top = top + FULL_TOP;
	selection.r.width = ROI_WIDTH;
	selection.r.height = ROI_HEIGHT;

	if (ioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &selection) < 0)
		return -1;

	printf("roi_visible=(%d,%d)/%dx%d roi_native=(%d,%d)/%dx%d drop_frames=1\n",
		selection.r.left - FULL_LEFT, selection.r.top - FULL_TOP,
		selection.r.width, selection.r.height, selection.r.left,
		selection.r.top, selection.r.width, selection.r.height);
	return 0;
}

static int get_control(int fd, unsigned int id, int *value)
{
	struct v4l2_control control = { .id = id };

	if (ioctl(fd, VIDIOC_G_CTRL, &control) < 0)
		return -1;
	*value = control.value;
	return 0;
}

static long long get_link_frequency(int fd)
{
	struct v4l2_control control = { .id = V4L2_CID_LINK_FREQ };
	struct v4l2_querymenu menu = { .id = V4L2_CID_LINK_FREQ };

	if (ioctl(fd, VIDIOC_G_CTRL, &control) < 0)
		return -1;
	menu.index = control.value;
	if (ioctl(fd, VIDIOC_QUERYMENU, &menu) < 0)
		return -1;
	return (long long)menu.value;
}

static int set_control(int fd, unsigned int id, int value)
{
	struct v4l2_control control = { .id = id, .value = value };

	return ioctl(fd, VIDIOC_S_CTRL, &control);
}

static double get_fps(int fd)
{
	struct v4l2_subdev_frame_interval interval = { .pad = 0 };

	if (ioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &interval) < 0 ||
	    interval.interval.numerator == 0)
		return 0.0;
	return (double)interval.interval.denominator /
	       (double)interval.interval.numerator;
}

static int print_state(int fd)
{
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
	};
	struct v4l2_subdev_selection crop = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.target = V4L2_SEL_TGT_CROP,
	};
	struct v4l2_subdev_selection bounds = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
		.target = V4L2_SEL_TGT_CROP_BOUNDS,
	};
	int vblank = -1;
	int hmax;
	long long link_frequency;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &format) < 0)
		return -1;
	if (ioctl(fd, VIDIOC_SUBDEV_G_SELECTION, &crop) < 0)
		return -1;
	if (ioctl(fd, VIDIOC_SUBDEV_G_SELECTION, &bounds) < 0)
		return -1;
	get_control(fd, V4L2_CID_VBLANK, &vblank);
	hmax = format.format.width == ROI_SENSOR_WIDTH ? 365 : 550;
	link_frequency = get_link_frequency(fd);

	printf("format=%ux%u code=0x%x crop=(%d,%d)/%dx%d "
	       "crop_bounds=(%d,%d)/%dx%d link_frequency=%lld "
	       "hmax=%d vblank=%d vmax=%d fps=%.3f\n",
		format.format.width, format.format.height, format.format.code,
		crop.r.left, crop.r.top, crop.r.width, crop.r.height,
		bounds.r.left, bounds.r.top, bounds.r.width, bounds.r.height,
		link_frequency, hmax, vblank,
		vblank >= 0 ? vblank + (int)format.format.height : -1,
		get_fps(fd));
	return 0;
}

static void sleep_milliseconds(int milliseconds)
{
	struct timespec delay = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
	};

	nanosleep(&delay, NULL);
}

static int sweep_fps(int fd, int dwell_ms)
{
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
	};
	int vmax;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &format) < 0)
		return -1;
	if (format.format.width != ROI_SENSOR_WIDTH ||
	    format.format.height != ROI_HEIGHT) {
		errno = EINVAL;
		fprintf(stderr, "ROI mode must be active before --sweep-fps\n");
		return -1;
	}

	for (vmax = ROI_SAFE_VMAX; vmax >= ROI_EXPERIMENTAL_VMAX; vmax -= 16) {
		if (set_control(fd, V4L2_CID_VBLANK, vmax - ROI_HEIGHT) < 0)
			return -1;
		printf("vmax=%d requested_fps=%.3f\n", vmax, get_fps(fd));
		fflush(stdout);
		sleep_milliseconds(dwell_ms);
	}
	if ((vmax + 16) != ROI_EXPERIMENTAL_VMAX) {
		if (set_control(fd, V4L2_CID_VBLANK,
				ROI_EXPERIMENTAL_VMAX - ROI_HEIGHT) < 0)
			return -1;
		printf("vmax=%d requested_fps=%.3f\n",
		       ROI_EXPERIMENTAL_VMAX, get_fps(fd));
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const struct option options[] = {
		{ "mode", required_argument, NULL, 'm' },
		{ "move", no_argument, NULL, 'M' },
		{ "get", no_argument, NULL, 'g' },
		{ "sweep-fps", no_argument, NULL, 's' },
		{ "left", required_argument, NULL, 'l' },
		{ "top", required_argument, NULL, 't' },
		{ "dwell-ms", required_argument, NULL, 'd' },
		{ "vmax", required_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	char subdev_path[64];
	char entity_name[64];
	enum action action = ACTION_NONE;
	int left = 0;
	int top = 0;
	int dwell_ms = 1000;
	int requested_vmax = 0;
	int option;
	int fd;
	int ret = 0;

	while ((option = getopt_long(argc, argv, "m:Mgsl:t:d:v:h", options, NULL)) != -1) {
		switch (option) {
		case 'm':
			action = strcmp(optarg, "full") == 0 ? ACTION_FULL :
				 strcmp(optarg, "roi") == 0 ? ACTION_ROI : ACTION_NONE;
			break;
		case 'M': action = ACTION_MOVE; break;
		case 'g': action = ACTION_GET; break;
		case 's': action = ACTION_SWEEP; break;
		case 'l': left = atoi(optarg); break;
		case 't': top = atoi(optarg); break;
		case 'd': dwell_ms = atoi(optarg); break;
		case 'v':
			action = ACTION_VMAX;
			requested_vmax = atoi(optarg);
			break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	if (action == ACTION_NONE) {
		usage(argv[0]);
		return 2;
	}
	if (find_subdev(subdev_path, sizeof(subdev_path), entity_name,
			 sizeof(entity_name)) < 0) {
		perror("find IMX415 subdevice");
		return 1;
	}
	fd = open(subdev_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(subdev_path);
		return 1;
	}
	printf("entity=%s subdev=%s\n", entity_name, subdev_path);

	switch (action) {
	case ACTION_FULL:
		ret = set_format(fd, 3864, 2192);
		break;
	case ACTION_ROI:
		ret = set_format(fd, ROI_SENSOR_WIDTH, ROI_HEIGHT);
		if (!ret)
			ret = set_roi(fd, left, top);
		break;
	case ACTION_MOVE:
		ret = set_roi(fd, left, top);
		break;
	case ACTION_GET:
		ret = print_state(fd);
		break;
	case ACTION_SWEEP:
		ret = sweep_fps(fd, dwell_ms > 0 ? dwell_ms : 1);
		break;
	case ACTION_VMAX:
		if (requested_vmax < ROI_EXPERIMENTAL_VMAX ||
		    requested_vmax > ROI_SAFE_VMAX) {
			errno = ERANGE;
			ret = -1;
		} else {
			ret = set_control(fd, V4L2_CID_VBLANK,
					  requested_vmax - ROI_HEIGHT);
			if (!ret)
				ret = print_state(fd);
		}
		break;
	default:
		ret = -1;
		errno = EINVAL;
		break;
	}
	if (ret < 0)
		perror("IMX415 operation");
	close(fd);
	return ret < 0 ? 1 : 0;
}
