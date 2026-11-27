#include "media/transcode.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <thread>

#include "core/log.h"
#include "core/strings.h"

extern char** environ;

namespace eos::media {
namespace {

struct Format {
  const char* suffix;
  const char* content_type;
  int min_kbps, max_kbps, default_kbps;
};

const std::map<std::string, Format>& formats() {
  static const std::map<std::string, Format> f = {
      {"opus", {"opus", "audio/ogg", 32, 256, 128}},
      {"mp3", {"mp3", "audio/mpeg", 96, 320, 192}},
  };
  return f;
}

class FfmpegSource : public StreamSource {
 public:
  FfmpegSource(pid_t pid, int fd, std::atomic<int>& running) : pid_(pid), fd_(fd), running_(running) {}
  ~FfmpegSource() override {
    if (fd_ >= 0) ::close(fd_);
    stop();
    --running_;
  }

  void run(const std::function<bool(const char*, size_t)>& sink) override {
    char buf[64 * 1024];
    while (true) {
      auto n = ::read(fd_, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      if (!sink(buf, static_cast<size_t>(n))) break;  // client went away
    }
    ::close(fd_);
    fd_ = -1;
    stop();
  }

 private:
  // Ends the process: wait if it already finished, else SIGTERM, then SIGKILL after 2 s.
  void stop() {
    if (pid_ <= 0) return;
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == 0) {
      ::kill(pid_, SIGTERM);
      for (int i = 0; i < 20; ++i) {
        if (::waitpid(pid_, &status, WNOHANG) != 0) {
          pid_ = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, &status, 0);
    }
    pid_ = -1;
  }

  pid_t pid_;
  int fd_;
  std::atomic<int>& running_;
};

}  // namespace

std::optional<TranscodePlan> plan_transcode(const Config& cfg, const std::string& source_suffix,
                                            int source_kbps, const std::optional<std::string>& format,
                                            const std::optional<int64_t>& max_kbps,
                                            int64_t time_offset_seconds) {
  auto fmt = format ? to_lower_ascii(trim(*format)) : std::string();
  int64_t limit = max_kbps.value_or(0);
  if (fmt == "raw") return std::nullopt;
  if (fmt.empty() && limit <= 0) return std::nullopt;
  const auto suffix = to_lower_ascii(source_suffix);
  if (!fmt.empty() && fmt == suffix && (limit <= 0 || limit >= source_kbps)) return std::nullopt;
  if (fmt.empty() && source_kbps > 0 && limit >= source_kbps) return std::nullopt;

  if (!formats().count(fmt)) fmt = to_lower_ascii(cfg.transcode_default_format);
  if (!formats().count(fmt)) fmt = "opus";
  const auto& f = formats().at(fmt);
  TranscodePlan plan;
  plan.format = fmt;
  plan.suffix = f.suffix;
  plan.content_type = f.content_type;
  plan.bitrate_kbps =
      limit > 0 ? static_cast<int>(std::clamp<int64_t>(limit, f.min_kbps, f.max_kbps)) : f.default_kbps;
  plan.offset_seconds = std::max<int64_t>(0, time_offset_seconds);
  return plan;
}

std::vector<std::string> ffmpeg_args(const Config& cfg, const TranscodePlan& plan,
                                     const std::filesystem::path& input) {
  std::vector<std::string> args = {cfg.transcode_ffmpeg, "-v", "error", "-nostdin"};
  if (plan.offset_seconds > 0) {
    args.push_back("-ss");
    args.push_back(std::to_string(plan.offset_seconds));
  }
  args.insert(args.end(), {"-i", input.string(), "-map", "0:a:0", "-vn", "-map_metadata", "-1"});
  if (plan.format == "opus") {
    args.insert(args.end(), {"-c:a", "libopus", "-b:a", std::to_string(plan.bitrate_kbps) + "k", "-vbr", "on",
                             "-f", "ogg"});
  } else {
    args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", std::to_string(plan.bitrate_kbps) + "k", "-f", "mp3"});
  }
  args.push_back("-");
  return args;
}

std::shared_ptr<StreamSource> Transcoder::start(const TranscodePlan& plan, const std::filesystem::path& input) {
  if (running_.fetch_add(1) >= cfg_.transcode_max_concurrent) {
    --running_;
    return nullptr;
  }
  auto args = ffmpeg_args(cfg_, plan, input);
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    --running_;
    throw std::runtime_error("pipe failed");
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  pid_t pid = -1;
  int rc = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    --running_;
    throw std::runtime_error(std::string("cannot start ffmpeg: ") + std::strerror(rc));
  }
  log::debug("transcode: {} {}k from {}s: {}", plan.format, plan.bitrate_kbps, plan.offset_seconds,
             input.filename().string());
  return std::make_shared<FfmpegSource>(pid, fds[0], running_);
}

}  // namespace eos::media
