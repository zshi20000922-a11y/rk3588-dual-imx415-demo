// SPDX-License-Identifier: GPL-2.0-or-later
//
// IMX415 full-frame / ROI closed-loop demo:
// V4L2 mmap NV12 capture -> latest-frame display -> YOLOv5 person detection
// -> sensor ROI switch -> return to full frame after person disappears.
//
// This file intentionally avoids OpenCV and JPEG I/O. The current RKNN YOLO
// sample API still consumes a CPU RGB image_buffer_t, so the only intentional
// copy in v1 is NV12 -> preallocated RGB input buffer.

#include <fcntl.h>
#include <getopt.h>
#ifdef HAVE_GSTREAMER
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#endif
#include <linux/videodev2.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "image_utils.h"
#include "yolov5.h"

using Clock = std::chrono::steady_clock;

static volatile sig_atomic_t g_stop = 0;

static double now_ms()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		       Clock::now().time_since_epoch())
		       .count() /
	       1000.0;
}

static int clamp_int(int value, int low, int high)
{
	return std::max(low, std::min(high, value));
}

static int even_clamp(int value, int low, int high)
{
	return clamp_int(value, low, high) & ~1;
}

static std::string json_escape(const std::string &input)
{
	std::ostringstream out;
	for (unsigned char c : input) {
		if (c == '"' || c == '\\')
			out << '\\' << c;
		else if (c == '\n')
			out << "\\n";
		else if (c == '\r')
			out << "\\r";
		else if (c == '\t')
			out << "\\t";
		else
			out << c;
	}
	return out.str();
}

static int run_command(const std::vector<std::string> &args)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		std::vector<char *> argv;
		for (const auto &arg : args)
			argv.push_back(const_cast<char *>(arg.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

struct Options {
	std::string yolo_model = "/userdata/yolov5-qwen35/models/yolov5s_relu.rknn";
	std::string roi_tool = "/userdata/imx415-roi-ctl";
	std::string video = "/dev/video44";
	std::string media = "/dev/media4";
	std::string log_path = "/tmp/imx415-yolo-roi-demo.jsonl";
	float person_threshold = 0.30f;
	int lost_frames = 15;
	int yolo_interval_ms = 80;
	double aiq_wait_s = 0.5;
};

struct ModeConfig {
	const char *name;
	const char *state;
	int width;
	int height;
	double sensor_fps;
	const char *cif;
	const char *isp_sink;
	const char *isp_source;
};

static const ModeConfig FULL = {
	"full",
	"FULL_MONITOR",
	960,
	540,
	60.0,
	"\"rkcif-mipi-lvds2\":0[fmt:SGBRG10_1X10/3840x2160]",
	"\"rkisp-isp-subdev\":0[fmt:SGBRG10_1X10/3840x2160 crop:(0,0)/3840x2160]",
	"\"rkisp-isp-subdev\":2[fmt:YUYV8_2X8/3840x2160 crop:(0,0)/3840x2160]",
};

static const ModeConfig ROI = {
	"roi",
	"ROI_TRACK",
	640,
	640,
	166.0,
	"\"rkcif-mipi-lvds2\":0[fmt:SGBRG10_1X10/640x640]",
	"\"rkisp-isp-subdev\":0[fmt:SGBRG10_1X10/640x640 crop:(0,0)/640x640]",
	"\"rkisp-isp-subdev\":2[fmt:YUYV8_2X8/640x640 crop:(0,0)/640x640]",
};

struct Logger {
	std::ofstream out;

	explicit Logger(const std::string &path) : out(path, std::ios::trunc) {}

	void write(const std::string &event, const std::string &fields)
	{
		out << "{\"t_ms\":" << now_ms() << ",\"event\":\"" << event << "\"";
		if (!fields.empty())
			out << "," << fields;
		out << "}\n";
		out.flush();
		std::cout << event << " " << fields << std::endl;
	}
};

struct V4L2Buffer {
	void *start = nullptr;
	size_t length = 0;
};

class Capture {
	int fd_ = -1;
	std::vector<V4L2Buffer> buffers_;
	v4l2_plane dequeue_planes_[VIDEO_MAX_PLANES] {};
	int width_ = 0;
	int height_ = 0;

public:
	~Capture() { close(); }

	bool open_device(const std::string &path, int width, int height)
	{
		close();
		width_ = width;
		height_ = height;
		fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd_ < 0) {
			perror(path.c_str());
			return false;
		}
		v4l2_format fmt {};
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		fmt.fmt.pix_mp.width = width;
		fmt.fmt.pix_mp.height = height;
		fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
		fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
		fmt.fmt.pix_mp.num_planes = 1;
		if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
			perror("VIDIOC_S_FMT");
			return false;
		}
		v4l2_requestbuffers req {};
		req.count = 4;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		req.memory = V4L2_MEMORY_MMAP;
		if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
			perror("VIDIOC_REQBUFS");
			return false;
		}
		buffers_.resize(req.count);
		for (unsigned int i = 0; i < req.count; ++i) {
			v4l2_buffer buf {};
			v4l2_plane planes[VIDEO_MAX_PLANES] {};
			buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			buf.memory = V4L2_MEMORY_MMAP;
			buf.index = i;
			buf.length = VIDEO_MAX_PLANES;
			buf.m.planes = planes;
			if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
				perror("VIDIOC_QUERYBUF");
				return false;
			}
			buffers_[i].length = planes[0].length;
			buffers_[i].start = mmap(nullptr, planes[0].length,
						 PROT_READ | PROT_WRITE, MAP_SHARED,
						 fd_, planes[0].m.mem_offset);
			if (buffers_[i].start == MAP_FAILED) {
				perror("mmap");
				return false;
			}
			if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
				perror("VIDIOC_QBUF");
				return false;
			}
		}
		int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
			perror("VIDIOC_STREAMON");
			return false;
		}
		return true;
	}

	bool dequeue(v4l2_buffer *buf, const unsigned char **data)
	{
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd_, &fds);
		timeval tv { 0, 500000 };
		int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
		if (ret <= 0)
			return false;
		std::memset(buf, 0, sizeof(*buf));
		std::memset(dequeue_planes_, 0, sizeof(dequeue_planes_));
		buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf->memory = V4L2_MEMORY_MMAP;
		buf->length = VIDEO_MAX_PLANES;
		buf->m.planes = dequeue_planes_;
		if (ioctl(fd_, VIDIOC_DQBUF, buf) < 0)
			return false;
		*data = static_cast<unsigned char *>(buffers_[buf->index].start);
		return true;
	}

	void requeue(v4l2_buffer *buf)
	{
		if (fd_ >= 0) {
			buf->m.planes = dequeue_planes_;
			ioctl(fd_, VIDIOC_QBUF, buf);
		}
	}

	void close()
	{
		if (fd_ >= 0) {
			int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			ioctl(fd_, VIDIOC_STREAMOFF, &type);
		}
		for (auto &buf : buffers_) {
			if (buf.start && buf.start != MAP_FAILED)
				munmap(buf.start, buf.length);
		}
		buffers_.clear();
		if (fd_ >= 0)
			::close(fd_);
		fd_ = -1;
	}

	int width() const { return width_; }
	int height() const { return height_; }
};

class Display {
#ifdef HAVE_GSTREAMER
	GstElement *pipeline_ = nullptr;
	GstElement *appsrc_ = nullptr;
#else
	pid_t gst_pid_ = -1;
#endif
	int width_ = 0;
	int height_ = 0;
	unsigned int frames_ = 0;
	double measured_at_ = 0;
	double fps_ = 0;

public:
	~Display() { stop(); }

	bool start(int width, int height)
	{
		stop();
		width_ = width;
		height_ = height;
#ifdef HAVE_GSTREAMER
		GError *error = nullptr;
		const char *desc =
			"appsrc name=src is-live=true format=time do-timestamp=true "
			"caps=video/x-raw,format=NV12,width=%d,height=%d,framerate=0/1 ! "
			"queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
			"waylandsink sync=false fullscreen=true";
		char pipeline_desc[512];
		std::snprintf(pipeline_desc, sizeof(pipeline_desc), desc, width, height);
		pipeline_ = gst_parse_launch(pipeline_desc, &error);
		if (!pipeline_) {
			std::fprintf(stderr, "gst_parse_launch failed: %s\n",
				     error ? error->message : "unknown");
			if (error)
				g_error_free(error);
			return false;
		}
		appsrc_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
		gst_element_set_state(pipeline_, GST_STATE_PLAYING);
		measured_at_ = now_ms();
		frames_ = 0;
		fps_ = 0;
		return true;
#else
		gst_pid_ = fork();
		if (gst_pid_ < 0) {
			perror("display fork");
			return false;
		}
		if (gst_pid_ == 0) {
			int error_fd = ::open("/tmp/imx415-gst-display.log",
					      O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (error_fd >= 0) {
				dup2(error_fd, STDERR_FILENO);
				::close(error_fd);
			}
			std::string caps = "video/x-raw,format=NV12,width=" +
					   std::to_string(width) + ",height=" +
					   std::to_string(height);
			setenv("XDG_RUNTIME_DIR", "/var/run", 0);
			setenv("WAYLAND_DISPLAY", "wayland-0", 0);
			execlp("gst-launch-1.0", "gst-launch-1.0", "-q",
			       "v4l2src", "device=/dev/video45", "io-mode=mmap",
			       "do-timestamp=true", "!", caps.c_str(), "!",
			       "queue", "max-size-buffers=1",
			       "max-size-bytes=0", "max-size-time=0",
			       "leaky=downstream", "!", "waylandsink",
			       "display=wayland-0", "layer=top", "alpha=1.0",
			       "sync=false", "fullscreen=true",
			       static_cast<char *>(nullptr));
			_exit(127);
		}
		measured_at_ = now_ms();
		frames_ = 0;
		fps_ = 0;
		return true;
#endif
	}

	void push(const unsigned char *nv12, size_t size)
	{
#ifdef HAVE_GSTREAMER
		if (!appsrc_)
			return;
		GstBuffer *buffer = gst_buffer_new_wrapped_full(
			GST_MEMORY_FLAG_READONLY, const_cast<unsigned char *>(nv12),
			size, 0, size, nullptr, nullptr);
		gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
		frames_++;
		double current = now_ms();
		if (current - measured_at_ >= 500.0) {
			fps_ = frames_ * 1000.0 / (current - measured_at_);
			frames_ = 0;
			measured_at_ = current;
		}
#else
		if (gst_pid_ <= 0)
			return;
		(void)nv12;
		(void)size;
		frames_++;
		double current = now_ms();
		if (current - measured_at_ >= 500.0) {
			fps_ = frames_ * 1000.0 / (current - measured_at_);
			frames_ = 0;
			measured_at_ = current;
		}
#endif
	}

	double fps() const { return fps_; }

	void stop()
	{
#ifdef HAVE_GSTREAMER
		if (appsrc_) {
			gst_object_unref(appsrc_);
			appsrc_ = nullptr;
		}
		if (pipeline_) {
			gst_element_set_state(pipeline_, GST_STATE_NULL);
			gst_object_unref(pipeline_);
			pipeline_ = nullptr;
		}
#else
		if (gst_pid_ > 0) {
			kill(gst_pid_, SIGTERM);
			waitpid(gst_pid_, nullptr, 0);
			gst_pid_ = -1;
		}
#endif
	}
};

struct Detection {
	std::string cls;
	float confidence = 0;
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

static unsigned char clip_byte(int value)
{
	return static_cast<unsigned char>(clamp_int(value, 0, 255));
}

static void nv12_to_rgb_letterbox(const unsigned char *nv12, int src_w, int src_h,
				  unsigned char *rgb, int dst_w, int dst_h)
{
	std::memset(rgb, 114, dst_w * dst_h * 3);
	double scale = std::min(dst_w / static_cast<double>(src_w),
				dst_h / static_cast<double>(src_h));
	int new_w = static_cast<int>(std::round(src_w * scale));
	int new_h = static_cast<int>(std::round(src_h * scale));
	int pad_x = (dst_w - new_w) / 2;
	int pad_y = (dst_h - new_h) / 2;
	const unsigned char *y_plane = nv12;
	const unsigned char *uv_plane = nv12 + src_w * src_h;

	for (int dy = 0; dy < new_h; ++dy) {
		int sy = std::min(src_h - 1, static_cast<int>(dy / scale));
		for (int dx = 0; dx < new_w; ++dx) {
			int sx = std::min(src_w - 1, static_cast<int>(dx / scale));
			int y = y_plane[sy * src_w + sx];
			int uv_index = (sy / 2) * src_w + (sx & ~1);
			int u = uv_plane[uv_index] - 128;
			int v = uv_plane[uv_index + 1] - 128;
			int c = y - 16;
			int r = (298 * c + 409 * v + 128) >> 8;
			int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
			int b = (298 * c + 516 * u + 128) >> 8;
			unsigned char *out = rgb + ((dy + pad_y) * dst_w + dx + pad_x) * 3;
			out[0] = clip_byte(r);
			out[1] = clip_byte(g);
			out[2] = clip_byte(b);
		}
	}
}

class Yolo {
	rknn_app_context_t context_ {};
	std::vector<unsigned char> input_;
	bool ready_ = false;

public:
	bool init(const std::string &model,
		  rknn_core_mask core_mask = RKNN_NPU_CORE_0_1_2)
	{
		init_post_process();
		if (init_yolov5_model(model.c_str(), &context_) != 0)
			return false;
		int core_ret = rknn_set_core_mask(context_.rknn_ctx, core_mask);
		if (core_ret != RKNN_SUCC) {
			std::fprintf(stderr,
				     "rknn_set_core_mask failed: %d\n",
				     core_ret);
			release_yolov5_model(&context_);
			return false;
		}
		input_.resize(640 * 640 * 3);
		ready_ = true;
		return true;
	}

	std::vector<Detection> infer(const unsigned char *nv12, int width, int height,
				     double *inference_ms)
	{
		std::vector<Detection> detections;
		if (!ready_)
			return detections;
		image_buffer_t image {};
		image.width = width;
		image.height = height;
		image.width_stride = width;
		image.height_stride = height;
		image.format = IMAGE_FORMAT_YUV420SP_NV12;
		image.virt_addr = const_cast<unsigned char *>(nv12);
		image.size = width * height * 3 / 2;

		object_detect_result_list results {};
		double begin = now_ms();
		int ret = inference_yolov5_model(&context_, &image, &results);
		*inference_ms = now_ms() - begin;
		if (ret != 0)
			return detections;
		for (int i = 0; i < results.count; ++i) {
			object_detect_result *det = &results.results[i];
			Detection item;
			item.cls = coco_cls_to_name(det->cls_id);
			item.confidence = det->prop;
			item.left = det->box.left;
			item.top = det->box.top;
			item.right = det->box.right;
			item.bottom = det->box.bottom;
			detections.push_back(item);
		}
		return detections;
	}

	~Yolo()
	{
		if (ready_)
			release_yolov5_model(&context_);
		deinit_post_process();
	}
};

static bool configure_mode(const Options &options, const ModeConfig &mode,
			   int roi_left, int roi_top, Logger &log)
{
	double begin = now_ms();
	run_command({ "killall", "rkaiq_3A_server" });
	usleep(150000);
	int ret;
	if (std::strcmp(mode.name, "roi") == 0) {
		ret = run_command({ options.roi_tool, "--mode", "roi", "--left",
				    std::to_string(roi_left), "--top", std::to_string(roi_top) });
	} else {
		ret = run_command({ options.roi_tool, "--mode", "full" });
	}
	if (ret != 0)
		return false;
	for (const char *fmt : { mode.cif, mode.isp_sink, mode.isp_source }) {
		if (run_command({ "media-ctl", "-d", options.media, "--set-v4l2", fmt }) != 0)
			return false;
	}
	std::string size = "width=" + std::to_string(mode.width) + ",height=" +
			   std::to_string(mode.height) + ",pixelformat=NV12";
	if (run_command({ "v4l2-ctl", "-d", options.video, "--set-fmt-video=" + size }) != 0)
		return false;
	if (run_command({ "v4l2-ctl", "-d", "/dev/video45", "--set-fmt-video=" + size }) != 0)
		return false;
	pid_t pid = fork();
	if (pid == 0) {
		execl("/usr/bin/rkaiq_3A_server", "rkaiq_3A_server", nullptr);
		_exit(127);
	}
	usleep(static_cast<useconds_t>(options.aiq_wait_s * 1000000.0));
	log.write("camera_configured",
		  "\"mode\":\"" + std::string(mode.name) + "\",\"configuration_ms\":" +
			  std::to_string(now_ms() - begin));
	return true;
}

static void restore_full(const Options &options)
{
	run_command({ "killall", "rkaiq_3A_server" });
	usleep(150000);
	run_command({ options.roi_tool, "--mode", "full" });
	pid_t pid = fork();
	if (pid == 0) {
		execl("/usr/bin/rkaiq_3A_server", "rkaiq_3A_server", nullptr);
		_exit(127);
	}
}

static void usage(const char *program)
{
	std::fprintf(stderr,
		     "Usage: %s --yolo-model model.rknn [--person-threshold 0.30] "
		     "[--lost-frames 15]\n",
		     program);
}

static Options parse_options(int argc, char **argv)
{
	Options options;
	static const option long_options[] = {
		{ "yolo-model", required_argument, nullptr, 'm' },
		{ "roi-tool", required_argument, nullptr, 'r' },
		{ "video", required_argument, nullptr, 'v' },
		{ "media", required_argument, nullptr, 'd' },
		{ "log", required_argument, nullptr, 'l' },
		{ "person-threshold", required_argument, nullptr, 'p' },
		{ "lost-frames", required_argument, nullptr, 'f' },
		{ "yolo-interval-ms", required_argument, nullptr, 'i' },
		{ "aiq-wait", required_argument, nullptr, 'a' },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "m:r:v:d:l:p:f:i:a:h", long_options, nullptr)) != -1) {
		switch (c) {
		case 'm': options.yolo_model = optarg; break;
		case 'r': options.roi_tool = optarg; break;
		case 'v': options.video = optarg; break;
		case 'd': options.media = optarg; break;
		case 'l': options.log_path = optarg; break;
		case 'p': options.person_threshold = std::strtof(optarg, nullptr); break;
		case 'f': options.lost_frames = std::atoi(optarg); break;
		case 'i': options.yolo_interval_ms = std::atoi(optarg); break;
		case 'a': options.aiq_wait_s = std::strtod(optarg, nullptr); break;
		case 'h': usage(argv[0]); std::exit(0);
		default: usage(argv[0]); std::exit(2);
		}
	}
	return options;
}

static void on_signal(int)
{
	g_stop = 1;
}

int main(int argc, char **argv)
{
	Options options = parse_options(argc, argv);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
#ifdef HAVE_GSTREAMER
	gst_init(&argc, &argv);
#else
	(void)argc;
	(void)argv;
#endif

	Logger log(options.log_path);
	log.write("demo_start",
		  "\"copy_mode\":\"nv12_to_rgb_preallocated\","
		  "\"display_backend\":\"rkisp_selfpath_v4l2src_waylandsink\","
		  "\"npu_core_mask\":\"RKNN_NPU_CORE_0_1_2\","
		  "\"zero_copy_level\":\"dual_rkisp_v4l2_mmap_no_jpeg_no_opencv\"");

	Yolo yolo;
	if (!yolo.init(options.yolo_model)) {
		log.write("error", "\"message\":\"init_yolov5_model failed\"");
		return 3;
	}

	const ModeConfig *mode = &FULL;
	int roi_left = 1600;
	int roi_top = 760;
	int stream_failures = 0;
	int suspicious_frames = 0;
	int dropped_after_roi_move_frames = 0;
	int lost = 0;
	int sequence = 0;
	double last_frame_ms = 0;
	double yolo_measured_at = now_ms();
	int yolo_count = 0;
	double yolo_fps = 0;
	double camera_measured_at = now_ms();
	int camera_count = 0;
	double camera_fps = 0;

	Capture capture;
	Display display;
	bool has_pending_buffer = false;
	v4l2_buffer pending_buffer {};
	if (!configure_mode(options, *mode, roi_left, roi_top, log) ||
	    !capture.open_device(options.video, mode->width, mode->height) ||
	    !display.start(mode->width, mode->height)) {
		log.write("error", "\"message\":\"initial full mode failed\"");
		restore_full(options);
		return 4;
	}

	double last_yolo_ms = 0;
	std::string state = mode->state;

	while (!g_stop) {
		if (has_pending_buffer) {
			capture.requeue(&pending_buffer);
			has_pending_buffer = false;
		}
		v4l2_buffer buf {};
		const unsigned char *data = nullptr;
		if (!capture.dequeue(&buf, &data)) {
			stream_failures++;
			continue;
		}
		double frame_ms = now_ms();
		size_t frame_size = mode->width * mode->height * 3 / 2;
		display.push(data, frame_size);
		camera_count++;
		if (frame_ms - camera_measured_at >= 500.0) {
			camera_fps = camera_count * 1000.0 / (frame_ms - camera_measured_at);
			camera_count = 0;
			camera_measured_at = frame_ms;
		}
		bool suspicious = false;
		long luma_sum = 0;
		for (int i = 0; i < std::min(4096, mode->width * mode->height); i += 16)
			luma_sum += data[i];
		if (luma_sum / 256 < 3)
			suspicious = true;
		if (suspicious)
			suspicious_frames++;

		bool run_yolo = frame_ms - last_yolo_ms >= options.yolo_interval_ms;
		Detection best_person;
		bool has_person = false;
		double inference_ms = 0;
		if (run_yolo) {
			last_yolo_ms = frame_ms;
			auto detections = yolo.infer(data, mode->width, mode->height, &inference_ms);
			for (const auto &det : detections) {
				if (det.cls == "person" && det.confidence >= options.person_threshold &&
				    (!has_person || det.confidence > best_person.confidence)) {
					best_person = det;
					has_person = true;
				}
			}
			yolo_count++;
			if (frame_ms - yolo_measured_at >= 1000.0) {
				yolo_fps = yolo_count * 1000.0 / (frame_ms - yolo_measured_at);
				yolo_count = 0;
				yolo_measured_at = frame_ms;
			}
		}

		if (run_yolo) {
			log.write("frame",
				  "\"sequence\":" + std::to_string(sequence) +
					  ",\"mode\":\"" + mode->name + "\",\"state\":\"" + state +
					  "\",\"camera_fps\":" + std::to_string(camera_fps) +
					  ",\"display_fps\":" + std::to_string(display.fps()) +
					  ",\"yolo_fps\":" + std::to_string(yolo_fps) +
					  ",\"yolo_inference_ms\":" + std::to_string(inference_ms) +
					  ",\"has_person\":" + std::string(has_person ? "true" : "false") +
					  ",\"roi_left\":" + std::to_string(roi_left) +
					  ",\"roi_top\":" + std::to_string(roi_top) +
					  ",\"stream_failures\":" + std::to_string(stream_failures) +
					  ",\"suspicious_frames\":" + std::to_string(suspicious_frames) +
					  ",\"dropped_after_roi_move_frames\":" +
					  std::to_string(dropped_after_roi_move_frames));
		}

		pending_buffer = buf;
		has_pending_buffer = true;
		sequence++;

		if (!run_yolo)
			continue;

		if (mode == &FULL && has_person) {
			int cx = (best_person.left + best_person.right) / 2;
			int cy = (best_person.top + best_person.bottom) / 2;
			int global_x = cx * 3840 / mode->width;
			int global_y = cy * 2160 / mode->height;
			roi_left = even_clamp(global_x - 320, 0, 3200);
			roi_top = even_clamp(global_y - 320, 0, 1520);
			log.write("person_detected",
				  "\"confidence\":" + std::to_string(best_person.confidence) +
					  ",\"person_bbox\":[" + std::to_string(best_person.left) +
					  "," + std::to_string(best_person.top) + "," +
					  std::to_string(best_person.right) + "," +
					  std::to_string(best_person.bottom) + "],\"roi_left\":" +
					  std::to_string(roi_left) + ",\"roi_top\":" +
					  std::to_string(roi_top));
			state = "SWITCH_TO_ROI";
			log.write("switch_to_roi",
				  "\"roi_left\":" + std::to_string(roi_left) +
					  ",\"roi_top\":" + std::to_string(roi_top));
			log.write("switch_begin", "\"from\":\"full\",\"to\":\"roi\"");
			if (has_pending_buffer) {
				capture.requeue(&pending_buffer);
				has_pending_buffer = false;
			}
			capture.close();
			display.stop();
			double switch_begin = now_ms();
			if (!configure_mode(options, ROI, roi_left, roi_top, log) ||
			    !capture.open_device(options.video, ROI.width, ROI.height) ||
			    !display.start(ROI.width, ROI.height)) {
				state = "ERROR_RECOVER";
				log.write("error", "\"message\":\"switch_to_roi failed\"");
				configure_mode(options, FULL, roi_left, roi_top, log);
				capture.open_device(options.video, FULL.width, FULL.height);
				display.start(FULL.width, FULL.height);
				mode = &FULL;
			} else {
				mode = &ROI;
				state = "ROI_TRACK";
				lost = 0;
				log.write("switch_done",
					  "\"from\":\"full\",\"to\":\"roi\",\"last_old_frame_to_first_new_frame_ms\":" +
						  std::to_string(now_ms() - last_frame_ms) +
						  ",\"command_to_stream_ms\":" +
						  std::to_string(now_ms() - switch_begin));
			}
		} else if (mode == &ROI) {
			if (has_person) {
				lost = 0;
			} else {
				lost++;
			}
			if (lost >= options.lost_frames) {
				state = "SWITCH_TO_FULL";
				log.write("switch_to_full",
					  "\"lost_frames\":" + std::to_string(lost));
				log.write("switch_begin",
					  "\"from\":\"roi\",\"to\":\"full\",\"lost_frames\":" +
						  std::to_string(lost));
				if (has_pending_buffer) {
					capture.requeue(&pending_buffer);
					has_pending_buffer = false;
				}
				capture.close();
				display.stop();
				double switch_begin = now_ms();
				if (!configure_mode(options, FULL, roi_left, roi_top, log) ||
				    !capture.open_device(options.video, FULL.width, FULL.height) ||
				    !display.start(FULL.width, FULL.height)) {
					state = "ERROR_RECOVER";
					log.write("error", "\"message\":\"switch_to_full failed\"");
				} else {
					mode = &FULL;
					state = "FULL_MONITOR";
					lost = 0;
					log.write("switch_done",
						  "\"from\":\"roi\",\"to\":\"full\",\"last_old_frame_to_first_new_frame_ms\":" +
							  std::to_string(now_ms() - last_frame_ms) +
							  ",\"command_to_stream_ms\":" +
							  std::to_string(now_ms() - switch_begin));
				}
			}
		}
		last_frame_ms = frame_ms;
	}

	log.write("shutdown", "\"message\":\"restoring full mode\"");
	if (has_pending_buffer)
		capture.requeue(&pending_buffer);
	capture.close();
	display.stop();
	restore_full(options);
	return 0;
}
