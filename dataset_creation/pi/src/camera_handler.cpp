// © 2024 Alec Fessler
// MIT License
// See LICENSE file in the project root for full license information.

#include <cstring>
#include <sstream>
#include <sys/mman.h>
#include "camera_handler.h"
#include "logger.h"

#include <iostream>

camera_handler_t::camera_handler_t(
  config_parser& config,
  logger_t& logger
) :
  config(config),
  logger(logger),
  next_req_idx_(0) {
  unsigned int frame_width = config.get_int("FRAME_WIDTH");
  unsigned int frame_height = config.get_int("FRAME_HEIGHT");
  unsigned int fps = config.get_int("FPS");
  unsigned int frame_buffers = config.get_int("FRAME_BUFFERS");
  unsigned int frame_duration_min = config.get_int("FRAME_DURATION_MIN");
  unsigned int frame_duration_max = config.get_int("FRAME_DURATION_MAX");
  unsigned int streaming_cpu = config.get_int("STREAMING_CPU");
  std::string server_ip = config.get_string("SERVER_IP");
  std::string port = config.get_string("PORT");

  y_plane_bytes_ = frame_width * frame_height;
  u_plane_bytes_ = y_plane_bytes_ / 4;
  v_plane_bytes_ = u_plane_bytes_;
  frame_bytes_ = y_plane_bytes_ + u_plane_bytes_ + v_plane_bytes_;

  cm_ = std::make_unique<libcamera::CameraManager>();
  if (cm_->start() < 0) {
    const char* err = "Failed to start camera manager";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  auto cameras = cm_->cameras();
  if (cameras.empty()) {
    const char* err = "No cameras available";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  camera_ = cm_->get(cameras[0]->id());
  if (!camera_) {
    const char* err = "Failed to retrieve camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
  if (camera_->acquire() < 0) {
    const char* err = "Failed to acquire camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  config_ = camera_->generateConfiguration({ libcamera::StreamRole::VideoRecording });
  if (!config_) {
    const char* err = "Failed to generate camera configuration";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  libcamera::StreamConfiguration& cfg = config_->at(0);
  cfg.pixelFormat = libcamera::formats::YUV420;
  cfg.size = { frame_width, frame_height };
  cfg.bufferCount = frame_buffers;

  if (config_->validate() == libcamera::CameraConfiguration::Invalid) {
    const char* err = "Invalid camera configuration, unable to adjust";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  } else if (config_->validate() == libcamera::CameraConfiguration::Adjusted) {
    const char* err = "Invalid camera configuration, adjusted";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  if (camera_->configure(config_.get()) < 0) {
    const char* err = "Failed to configure camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
  stream_ = cfg.stream();
  if (allocator_->allocate(stream_) < 0) {
    const char* err = "Failed to allocate buffers";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  uint64_t req_cookie = 0; // maps request to index in mmap_buffers_
  for (const std::unique_ptr<libcamera::FrameBuffer>& buffer : allocator_->buffers(stream_)) {
    std::unique_ptr<libcamera::Request> request = camera_->createRequest(req_cookie++);
    if (!request) {
      const char* err = "Failed to create request";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }
    if (request->addBuffer(stream_, buffer.get()) < 0) {
      const char* err = "Failed to add buffer to request";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }
    requests_.push_back(std::move(request));

    const libcamera::FrameBuffer::Plane& y_plane = buffer->planes()[0];
    const libcamera::FrameBuffer::Plane& u_plane = buffer->planes()[1];
    const libcamera::FrameBuffer::Plane& v_plane = buffer->planes()[2];

    if (y_plane.length != y_plane_bytes_ || u_plane.length != u_plane_bytes_ || v_plane.length != v_plane_bytes_) {
      const char* err = "Plane size does not match expected size";
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
      throw std::runtime_error(err);
    }

    void* data = mmap(
      nullptr,
      frame_bytes_,
      PROT_READ | PROT_WRITE,
      MAP_SHARED,
      y_plane.fd.get(),
      y_plane.offset
    );

    if (data == MAP_FAILED) {
      std::string err = "Failed to mmap plane data: " + std::string(strerror(errno));
      logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err.c_str());
      throw std::runtime_error(err);
    }

    mmap_buffers_.push_back(data);
  }

  camera_->requestCompleted.connect(this,& camera_handler_t::request_complete);

  // Configure some settings for more deterministic capture times
  // May need to be adjusted based on lighting conditions
  // and on a per device basis, but for development purposes, this is acceptable
  controls_ = std::make_unique<libcamera::ControlList>();

  // Fix exposure time to half the time between frames
  // May be able to remove frame duration limit control since we are setting exposure
  controls_->set(libcamera::controls::FrameDurationLimits, libcamera::Span<const std::int64_t, 2>({ frame_duration_min, frame_duration_max }));
  controls_->set(libcamera::controls::AeEnable, false);
  controls_->set(libcamera::controls::ExposureTime, frame_duration_min);

  // Fix focus to ~12 inches
  // Focus value should be reciprocal of distance in meters
  controls_->set(libcamera::controls::AfMode, libcamera::controls::AfModeManual);
  controls_->set(libcamera::controls::LensPosition, 3.33);

  // Fix white balance, gain, and disable HDR
  controls_->set(libcamera::controls::AwbEnable, false);
  controls_->set(libcamera::controls::AnalogueGain, 1.0);
  controls_->set(libcamera::controls::HdrMode, libcamera::controls::HdrModeOff);

  controls_->set(libcamera::controls::rpi::StatsOutputEnable, false);

  if (camera_->start(controls_.get()) < 0) {
    const char* err = "Failed to start camera";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }

  std::stringstream ffmpeg_cmd;
  ffmpeg_cmd << "taskset -c " << streaming_cpu
             << " ffmpeg -f rawvideo -pix_fmt yuv420p -video_size "
             << frame_width << "x" << frame_height
             << " -framerate " << fps
             << " -i - -c:v libx264 -f mpegts "
             << "tcp://" << server_ip << ":" << port;
  std::string cmd_str = ffmpeg_cmd.str();
  ffmpeg_ = popen(cmd_str.c_str(), "w");
  if (!ffmpeg_) {
    const char* err = "Failed to start ffmpeg";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
}

camera_handler_t::~camera_handler_t() {
  camera_->stop();
  for (void* data : mmap_buffers_)
    munmap(data, frame_bytes_);
  allocator_->free(stream_);
  allocator_.reset();
  camera_->release();
  camera_.reset();
  cm_->stop();
  if (ffmpeg_)
    pclose(ffmpeg_);
}

void camera_handler_t::queue_request() {
  /**
   * Queue the next request in the sequence.
   *
   * If requests are not returned at the same rate as they are queued,
   * this method will throw to signal that the camera is not keeping up,
   * and this should be handled by adjusting the configuration.
   * ie. framerate, exposure, gain, etc.
   *
   * Throws:
   *  - std::runtime_error: Buffer is not ready for requeuing
   *  - std::runtime_error: Failed to queue request
   */
  if (camera_->queueRequest(requests_[next_req_idx_].get()) < 0) {
    const char* err = "Failed to queue request";
    logger.log(logger_t::level_t::ERROR, __FILE__, __LINE__, err);
    throw std::runtime_error(err);
  }
  ++next_req_idx_;
  next_req_idx_ %= requests_.size();
}

void camera_handler_t::request_complete(libcamera::Request* request) {
  /**
   * Signal handler for when a request is completed.
   *
   * This method is called by the camera manager when a request is completed.
   * The frame buffer is piped to ffmpeg for streaming back to the server.
   * The request is then reused and the buffer is enqueued for transmission.
   *
   * Parameters:
   *  - request: The completed request
   */
  if (request->status() == libcamera::Request::RequestCancelled)
    return;

  const char* info = "Request completed";
  logger.log(logger_t::level_t::INFO, __FILE__, __LINE__, info);

  void* data = mmap_buffers_[request->cookie()];
  fwrite(data, 1, frame_bytes_, ffmpeg_);
  request->reuse(libcamera::Request::ReuseBuffers);
}
