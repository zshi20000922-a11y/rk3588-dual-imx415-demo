// SPDX-License-Identifier: GPL-2.0-or-later
// Dual IMX415 demo: low-rate global person search + high-rate ROI person detect.

#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <sys/stat.h>
#include <linux/v4l2-subdev.h>

#define main imx415_single_camera_demo_main
#include "imx415-yolo-roi-demo.cc"
#undef main

struct DualOptions {
	std::string model = "/userdata/dual-person-demo/yolov5n.rknn";
	std::string log = "/tmp/dual-person-demo.jsonl";
	std::string global_video = "/dev/video44";
	std::string roi_video = "/dev/video53";
	int global_period_ms = 500;
	int duration_s = 0;
	float threshold = 0.30f;
	float roi_scale_x = 1.0f;
	float roi_scale_y = 1.0f;
	int roi_offset_x = 0;
	int roi_offset_y = 0;
	int roi_move_threshold = 48;
};

static DualOptions dual_options(int argc, char **argv)
{
	DualOptions o;
	static const option opts[] = {
		{ "model", required_argument, nullptr, 'm' },
		{ "log", required_argument, nullptr, 'l' },
		{ "global-period-ms", required_argument, nullptr, 'g' },
		{ "duration", required_argument, nullptr, 'd' },
		{ "threshold", required_argument, nullptr, 't' },
		{ "roi-scale-x", required_argument, nullptr, 1001 },
		{ "roi-scale-y", required_argument, nullptr, 1002 },
		{ "roi-offset-x", required_argument, nullptr, 1003 },
		{ "roi-offset-y", required_argument, nullptr, 1004 },
		{ "roi-move-threshold", required_argument, nullptr, 1005 },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};
	int c;
	while ((c = getopt_long(argc, argv, "m:l:g:d:t:h", opts, nullptr)) != -1) {
		switch (c) {
		case 'm': o.model = optarg; break;
		case 'l': o.log = optarg; break;
		case 'g': o.global_period_ms = std::max(50, std::atoi(optarg)); break;
		case 'd': o.duration_s = std::max(0, std::atoi(optarg)); break;
		case 't': o.threshold = std::strtof(optarg, nullptr); break;
		case 1001: o.roi_scale_x = std::strtof(optarg, nullptr); break;
		case 1002: o.roi_scale_y = std::strtof(optarg, nullptr); break;
		case 1003: o.roi_offset_x = std::atoi(optarg); break;
		case 1004: o.roi_offset_y = std::atoi(optarg); break;
		case 1005: o.roi_move_threshold = std::max(0, std::atoi(optarg)); break;
		case 'h':
			std::printf("Usage: %s [--model yolov5n.rknn] [--global-period-ms 500] "
				    "[--threshold 0.30] [--duration seconds]\n", argv[0]);
			std::exit(0);
		default: std::exit(2);
		}
	}
	return o;
}

class LatestCamera {
	Capture capture_;
	std::thread thread_;
	std::mutex mutex_;
	std::shared_ptr<const std::vector<unsigned char>> latest_;
	std::atomic<bool> running_ { false };
	std::atomic<unsigned long> sequence_ { 0 };
	std::atomic<double> fps_ { 0.0 };
	int width_ = 0;
	int height_ = 0;

public:
	bool start(const std::string &device, int width, int height)
	{
		width_ = width;
		height_ = height;
		if (!capture_.open_device(device, width, height))
			return false;
		running_ = true;
		thread_ = std::thread([this]() {
			unsigned count = 0;
			double measured = now_ms();
			while (running_ && !g_stop) {
				v4l2_buffer buf {};
				const unsigned char *data = nullptr;
				if (!capture_.dequeue(&buf, &data))
					continue;
				auto next = std::make_shared<std::vector<unsigned char>>(
					data, data + width_ * height_ * 3 / 2);
				{
					std::lock_guard<std::mutex> lock(mutex_);
					latest_ = std::move(next);
					sequence_++;
				}
				capture_.requeue(&buf);
				count++;
				double current = now_ms();
				if (current - measured >= 1000.0) {
					fps_ = count * 1000.0 / (current - measured);
					count = 0;
					measured = current;
				}
			}
		});
		return true;
	}

	bool snapshot(std::shared_ptr<const std::vector<unsigned char>> &out,
		      unsigned long *seq)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!latest_)
			return false;
		out = latest_;
		*seq = sequence_;
		return true;
	}

	double fps() const { return fps_; }
	void stop()
	{
		running_ = false;
		if (thread_.joinable())
			thread_.join();
		capture_.close();
	}
	~LatestCamera() { stop(); }
};

class DynamicRoi {
	int fd_ = -1;
	int left_ = 1600;
	int top_ = 760;
	std::mutex mutex_;

public:
	bool open_sensor(const char *path)
	{
		fd_ = ::open(path, O_RDWR | O_CLOEXEC);
		return fd_ >= 0;
	}

	bool move_from_global(const Detection &box, const DualOptions &options)
	{
		// Detection coordinates are already mapped back to the 960x540 source.
		double nx = ((box.left + box.right) * 0.5) / 960.0;
		double ny = ((box.top + box.bottom) * 0.5) / 540.0;
		nx = (nx - 0.5) * options.roi_scale_x + 0.5;
		ny = (ny - 0.5) * options.roi_scale_y + 0.5;
		int next_left = even_clamp(static_cast<int>(nx * 3840.0) - 320 +
					   options.roi_offset_x, 0, 3200);
		int next_top = even_clamp(static_cast<int>(ny * 2160.0) - 320 +
					  options.roi_offset_y, 0, 1520);
		std::lock_guard<std::mutex> lock(mutex_);
		if (std::abs(next_left - left_) < options.roi_move_threshold &&
		    std::abs(next_top - top_) < options.roi_move_threshold)
			return true;
		v4l2_subdev_selection selection {};
		selection.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		selection.pad = 0;
		selection.target = V4L2_SEL_TGT_CROP;
		selection.r.left = next_left + 12;
		selection.r.top = next_top + 16;
		selection.r.width = 640;
		selection.r.height = 640;
		if (ioctl(fd_, VIDIOC_SUBDEV_S_SELECTION, &selection) < 0)
			return false;
		left_ = selection.r.left - 12;
		top_ = selection.r.top - 16;
		return true;
	}

	int left() { std::lock_guard<std::mutex> lock(mutex_); return left_; }
	int top() { std::lock_guard<std::mutex> lock(mutex_); return top_; }
	~DynamicRoi() { if (fd_ >= 0) ::close(fd_); }
};

struct PersonResult {
	bool found = false;
	float confidence = 0;
	Detection box;
};

class PreviewWriter {
	static constexpr size_t HEADER_SIZE = 1024;
	int fd_ = -1;
	unsigned char *mapping_ = nullptr;
	size_t mapping_size_ = 0;

public:
	bool open_file(const char *path, int width, int height)
	{
		mapping_size_ = HEADER_SIZE + width * height * 3 / 2;
		fd_ = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
		if (fd_ < 0 || ftruncate(fd_, mapping_size_) < 0)
			return false;
		mapping_ = static_cast<unsigned char *>(mmap(nullptr, mapping_size_,
							 PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
		if (mapping_ == MAP_FAILED) {
			mapping_ = nullptr;
			return false;
		}
		std::memset(mapping_, 0, HEADER_SIZE);
		reinterpret_cast<uint32_t *>(mapping_)[0] = 0x44554c31; // DUL1
		reinterpret_cast<uint32_t *>(mapping_)[2] = width;
		reinterpret_cast<uint32_t *>(mapping_)[3] = height;
		reinterpret_cast<uint32_t *>(mapping_)[4] = width * height * 3 / 2;
		return true;
	}

	void write(const std::vector<unsigned char> &frame,
		   const std::vector<Detection> &detections, double camera_fps,
		   double infer_fps, bool active)
	{
		if (!mapping_ || frame.size() + HEADER_SIZE > mapping_size_)
			return;
		uint32_t *words = reinterpret_cast<uint32_t *>(mapping_);
		__atomic_add_fetch(&words[1], 1, __ATOMIC_RELEASE); // odd: writing
		uint32_t count = std::min<size_t>(16, detections.size());
		words[5] = count;
		words[6] = active ? 1 : 0;
		float *values = reinterpret_cast<float *>(mapping_ + 24);
		values[0] = camera_fps;
		values[1] = infer_fps;
		for (uint32_t i = 0; i < count; ++i) {
			values[2 + i * 5 + 0] = detections[i].left;
			values[2 + i * 5 + 1] = detections[i].top;
			values[2 + i * 5 + 2] = detections[i].right;
			values[2 + i * 5 + 3] = detections[i].bottom;
			values[2 + i * 5 + 4] = detections[i].confidence;
		}
		std::memcpy(mapping_ + HEADER_SIZE, frame.data(), frame.size());
		__atomic_add_fetch(&words[1], 1, __ATOMIC_RELEASE); // even: ready
	}

	~PreviewWriter()
	{
		if (mapping_)
			munmap(mapping_, mapping_size_);
		if (fd_ >= 0)
			::close(fd_);
	}
};

static PersonResult best_person(const std::vector<Detection> &detections, float threshold)
{
	PersonResult result;
	for (const auto &det : detections) {
		if (det.cls == "person" && det.confidence >= threshold &&
		    (!result.found || det.confidence > result.confidence)) {
			result.found = true;
			result.confidence = det.confidence;
			result.box = det;
		}
	}
	return result;
}

static std::vector<Detection> person_detections(
	const std::vector<Detection> &detections, float threshold)
{
	std::vector<Detection> people;
	for (const auto &det : detections)
		if (det.cls == "person" && det.confidence >= threshold)
			people.push_back(det);
	return people;
}

int main(int argc, char **argv)
{
	DualOptions options = dual_options(argc, argv);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	Logger log(options.log);
	LatestCamera global_camera;
	LatestCamera roi_camera;
	if (!global_camera.start(options.global_video, 960, 540) ||
	    !roi_camera.start(options.roi_video, 640, 480)) {
		std::fprintf(stderr, "Cannot start both camera streams\n");
		return 4;
	}
	PreviewWriter global_preview;
	PreviewWriter roi_preview;
	global_preview.open_file("/dev/shm/dual-person-global", 960, 540);
	roi_preview.open_file("/dev/shm/dual-person-roi", 640, 480);
	DynamicRoi dynamic_roi;
	if (!dynamic_roi.open_sensor("/dev/v4l-subdev7"))
		std::fprintf(stderr, "Warning: dynamic ROI control is unavailable\n");

	log.write("start", "\"scheduler\":\"3-core-triggered-ROI\","
		  "\"core0\":\"global+ROI\",\"core1\":\"ROI\",\"core2\":\"ROI\"");
	double started = now_ms();
	std::atomic<double> roi_active_until { 0.0 };
	if (std::getenv("DUAL_FORCE_ROI"))
		roi_active_until = now_ms() + 24.0 * 60.0 * 60.0 * 1000.0;
	std::atomic<double> next_global { started };
	std::atomic<unsigned> global_count { 0 };
	std::atomic<unsigned> roi_count { 0 };
	std::atomic<unsigned> global_total { 0 };
	std::atomic<unsigned> roi_total { 0 };
	std::atomic<unsigned long> claimed_global_sequence { 0 };
	std::atomic<unsigned long> claimed_roi_sequence { 0 };
	std::atomic<double> global_yolo_fps { 0.0 };
	std::atomic<double> roi_yolo_fps { 0.0 };
	std::mutex result_mutex;
	std::vector<Detection> global_detections;
	std::vector<Detection> roi_detections;
	bool global_person = false;
	bool roi_person = false;
	float global_confidence = 0;
	float roi_confidence = 0;

	auto worker = [&](int index, rknn_core_mask mask) {
		Yolo yolo;
		if (!yolo.init(options.model, mask)) {
			std::fprintf(stderr, "NPU worker %d initialization failed\n", index);
			g_stop = 1;
			return;
		}
		unsigned long last_roi_sequence = 0;
		while (!g_stop) {
			double current = now_ms();
			bool roi_active = current < roi_active_until.load();
			bool full_rate_global = !roi_active;
			bool global_job = full_rate_global;
			if (roi_active && index == 0 && current >= next_global.load()) {
				double expected = next_global.load();
				global_job = next_global.compare_exchange_strong(
					expected, current + options.global_period_ms);
			}
			std::shared_ptr<const std::vector<unsigned char>> frame;
			unsigned long sequence = 0;
			LatestCamera &camera = global_job ? global_camera : roi_camera;
			if (!camera.snapshot(frame, &sequence) ||
			    (!global_job && sequence == last_roi_sequence)) {
				usleep(1000);
				continue;
			}
			if (global_job && full_rate_global) {
				unsigned long claimed = claimed_global_sequence.load();
				if (sequence <= claimed ||
				    !claimed_global_sequence.compare_exchange_strong(claimed, sequence))
					continue;
			}
			if (!global_job) {
				last_roi_sequence = sequence;
				unsigned long claimed = claimed_roi_sequence.load();
				if (sequence <= claimed ||
				    !claimed_roi_sequence.compare_exchange_strong(claimed, sequence))
					continue;
			}
			double inference_ms = 0;
			auto detections = yolo.infer(frame->data(), global_job ? 960 : 640,
						  global_job ? 540 : 480, &inference_ms);
			PersonResult person = best_person(detections, options.threshold);
			auto people = person_detections(detections, options.threshold);
			{
				std::lock_guard<std::mutex> lock(result_mutex);
				if (global_job) {
					global_detections = people;
					global_person = person.found;
					global_confidence = person.confidence;
				} else {
					roi_detections = people;
					roi_person = person.found;
					roi_confidence = person.confidence;
				}
			}
			if (global_job) {
				global_count++;
				global_total++;
				if (person.found)
					roi_active_until = now_ms() + 2000.0;
				if (person.found)
					dynamic_roi.move_from_global(person.box, options);
				if (person.found)
					log.write("global_person",
						  "\"core\":" + std::to_string(index) +
						  ",\"confidence\":" + std::to_string(person.confidence) +
						  ",\"inference_ms\":" + std::to_string(inference_ms) +
						  ",\"roi_left\":" + std::to_string(dynamic_roi.left()) +
						  ",\"roi_top\":" + std::to_string(dynamic_roi.top()));
			} else {
				roi_count++;
				roi_total++;
			}
		}
	};

	std::thread workers[3] = {
		std::thread(worker, 0, RKNN_NPU_CORE_0),
		std::thread(worker, 1, RKNN_NPU_CORE_1),
		std::thread(worker, 2, RKNN_NPU_CORE_2),
	};

	double measured = started;
	while (!g_stop &&
	       (!options.duration_s || now_ms() - started < options.duration_s * 1000.0)) {
		double current = now_ms();
		bool active = current < roi_active_until.load();
		std::shared_ptr<const std::vector<unsigned char>> global_frame, roi_frame;
		unsigned long sequence = 0;
		std::vector<Detection> global_boxes, roi_boxes;
		{
			std::lock_guard<std::mutex> lock(result_mutex);
			global_boxes = global_detections;
			roi_boxes = roi_detections;
		}
		if (global_camera.snapshot(global_frame, &sequence))
			global_preview.write(*global_frame, global_boxes, global_camera.fps(),
					     global_yolo_fps.load(), active);
		if (roi_camera.snapshot(roi_frame, &sequence))
			roi_preview.write(*roi_frame, roi_boxes, roi_camera.fps(),
					  roi_yolo_fps.load(), active);
		if (current - measured >= 1000.0) {
			double seconds = (current - measured) / 1000.0;
			global_yolo_fps = global_count.exchange(0) / seconds;
			roi_yolo_fps = roi_count.exchange(0) / seconds;
			log.write("status",
				  "\"global_camera_fps\":" + std::to_string(global_camera.fps()) +
				  ",\"roi_camera_fps\":" + std::to_string(roi_camera.fps()) +
				  ",\"global_infer_fps\":" + std::to_string(global_yolo_fps.load()) +
				  ",\"roi_infer_fps\":" + std::to_string(roi_yolo_fps.load()) +
				  ",\"roi_active\":" + std::string(active ? "true" : "false") +
				  ",\"global_person\":" + std::string(global_person ? "true" : "false") +
				  ",\"global_confidence\":" + std::to_string(global_confidence) +
				  ",\"roi_person\":" + std::string(roi_person ? "true" : "false") +
				  ",\"roi_confidence\":" + std::to_string(roi_confidence));
			measured = current;
		}
		usleep(30000);
	}
	g_stop = 1;
	for (auto &thread : workers)
		thread.join();

	global_camera.stop();
	roi_camera.stop();
	log.write("stop", "\"global_inferences\":" + std::to_string(global_total.load()) +
		  ",\"roi_inferences\":" + std::to_string(roi_total.load()));
	return 0;
}
