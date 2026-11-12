#pragma once
// The resource host child process on Linux and other POSIX systems: the same
// interface as script_process.hpp. The child is started with fork/exec, talks over its stdin and
// stdout (hex lines, see script_process.hpp) and dies with the server (PR_SET_PDEATHSIG on Linux,
// the job object's role on Windows).
#ifdef _WIN32
#error "script_process_posix.hpp is for POSIX systems; Windows uses script_process.hpp"
#endif

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "kcd2s/sdk/marshal.hpp"
#include "script_inbox.hpp"
#include "script_outbox.hpp"

namespace kcd2s::local {

// Text of a command line or path handed to a child process (UTF-8 on POSIX).
using ProcessText = std::string;
inline ProcessText process_text(const std::string& utf8) {
  return utf8;
}

// Bridge traffic to and from the child process (instrumentation).
struct ScriptBridgeCounters {
  std::uint64_t messages_sent = 0;
  std::uint64_t bytes_sent = 0;    // IPC line bytes written to the child
  double write_seconds = 0.0;      // time the caller waited because the outbox was full
  std::uint64_t queued_bytes = 0;  // given to send(), not yet taken by the child
  std::uint64_t messages_received = 0;
  std::uint64_t inbox_drops = 0;  // compatibility counter: healthy sessions never drop commands
};

// Own trusted resource host, isolated from game pointers and socket authority.
class ScriptProcess {
 public:
  // The longest line of the child's output, in hex digits (2 MiB of message): a recipe's picture
  // in the recipe book is 1 MiB. A longer line is dropped.
  static constexpr std::size_t kMaxOutputLine = 4u << 20;
  static constexpr std::size_t kInboxLimit = ScriptInbox::kMaxMessages;
  // The longest message to the resource host; a longer one is refused.
  static constexpr std::size_t kMaxMessageBytes = 65536;
  // How long the destructor lets the child finish after its stdin closed.
  static constexpr int kStopWaitMilliseconds = 11000;

  ScriptProcess(const ProcessText& node, const ProcessText& entry, const ProcessText& resources)
      : ScriptProcess(node, std::vector<ProcessText>{entry, resources}) {
  }
  ScriptProcess(const ProcessText& executable, const std::vector<ProcessText>& arguments) {
    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (!cloexec_pipe(to_child) || !cloexec_pipe(from_child)) {
      close_all({to_child[0], to_child[1], from_child[0], from_child[1]});
      throw std::runtime_error("Resource host pipe creation failed");
    }
    // Everything the child needs is prepared before fork: after fork only async-signal-safe calls.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t parent = getpid();
    const pid_t child = fork();
    if (child < 0) {
      close_all({to_child[0], to_child[1], from_child[0], from_child[1]});
      throw std::runtime_error("Resource host start failed");
    }
    if (child == 0) {
#ifdef __linux__
      // Die with the server, and do not outlive a server that died before this line.
      prctl(PR_SET_PDEATHSIG, SIGKILL);
      if (getppid() != parent) {
        _exit(127);
      }
#else
      (void)parent;
#endif
      dup2(to_child[0], STDIN_FILENO);
      dup2(from_child[1], STDOUT_FILENO);
      dup2(from_child[1], STDERR_FILENO);
      // A bare name ("node") is looked up on PATH, as a shell would; a path is taken as it is.
      execvp(executable.c_str(), argv.data());
      _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    input_ = to_child[1];
    output_ = from_child[0];
    process_ = child;
    reader_ = std::thread([this] { read_output(); });
    outbox_.start([this](const std::string& line) { return write_all(line); },
                  [this] {
                    close(input_);
                    input_ = -1;
                  });
  }
  ~ScriptProcess() {
    // Closing stdin, after what is queued, asks the resource host to stop; it gets
    // kStopWaitMilliseconds to do so. Ending a host that does not read frees the writing thread.
    outbox_.finish();
    if (process_ > 0) {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopWaitMilliseconds);
      while (!reaped() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!reaped()) {
        kill(process_, SIGKILL);
        waitpid(process_, nullptr, 0);
        exited_ = true;
      }
    }
    outbox_.join();
    stop_reader_.store(true, std::memory_order_relaxed);
    if (reader_.joinable()) {
      reader_.join();
    }
    if (output_ >= 0) {
      close(output_);
    }
  }
  ScriptProcess(const ScriptProcess&) = delete;
  ScriptProcess& operator=(const ScriptProcess&) = delete;

  bool alive() {
    return process_ > 0 && !reaped();
  }
  bool send(const sdk::Value& value) {
    inbox_.check();
    if (input_ < 0 || !alive()) {
      return false;
    }
    const auto bytes = sdk::encode(value);
    if (bytes.size() > kMaxMessageBytes) {
      return false;
    }
    static const char* hex = "0123456789abcdef";
    std::string line(bytes.size() * 2 + 1, '\n');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      line[2 * i] = hex[bytes[i] >> 4];
      line[2 * i + 1] = hex[bytes[i] & 15];
    }
    const std::size_t line_bytes = line.size();
    const bool ok = outbox_.push(std::move(line), counters_.write_seconds);
    inbox_.check();
    if (ok) {
      counters_.messages_sent += 1;
      counters_.bytes_sent += line_bytes;
    }
    counters_.queued_bytes = outbox_.queued_bytes();
    return ok;
  }
  std::vector<sdk::Value> take() {
    auto values = inbox_.take();
    counters_.messages_received += values.size();
    return values;
  }
  ScriptInbox::State inbox_state() const {
    return inbox_.state();
  }
  // Counters of the bridge, updated by send() and take() on the caller's thread.
  const ScriptBridgeCounters& counters() const {
    return counters_;
  }

 private:
  // A pipe whose ends close on exec (the child keeps only the ends dup2 gives it). macOS has no
  // pipe2: there the flag is set right after (nothing forks in between: the server starts its
  // resource host from one thread).
  static bool cloexec_pipe(int descriptors[2]) {
#ifdef __APPLE__
    if (pipe(descriptors) != 0) {
      return false;
    }
    return fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == 0;
#else
    return pipe2(descriptors, O_CLOEXEC) == 0;
#endif
  }
  static void close_all(std::initializer_list<int> descriptors) {
    for (int descriptor : descriptors) {
      if (descriptor >= 0) {
        close(descriptor);
      }
    }
  }
  // True once the child has exited (and was reaped here).
  bool reaped() {
    if (exited_) {
      return true;
    }
    const pid_t result = waitpid(process_, nullptr, WNOHANG);
    if (result == process_ || (result < 0 && errno == ECHILD)) {
      exited_ = true;
    }
    return exited_;
  }
  // Writes the whole line (blocking while the pipe is full, like WriteFile on Windows). A child
  // that closed its stdin makes it fail with EPIPE (the server ignores SIGPIPE).
  bool write_all(const std::string& line) {
    std::size_t done = 0;
    while (done < line.size()) {
      const ssize_t n = write(input_, line.data() + done, line.size() - done);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      done += std::size_t(n);
    }
    return true;
  }
  void read_output() {
    std::string line;
    char buffer[4096];
    bool overflow = false;
    pollfd readable{output_, POLLIN, 0};
    while (!stop_reader_.load(std::memory_order_relaxed)) {
      readable.revents = 0;
      const int ready = poll(&readable, 1, 100);
      if (ready < 0 && errno != EINTR) {
        return;
      }
      if (ready <= 0) {
        continue;
      }
      const ssize_t size = read(output_, buffer, sizeof(buffer));
      if (size == 0) {
        return;  // the child closed its stdout
      }
      if (size < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        return;
      }
      for (ssize_t i = 0; i < size; ++i) {
        const char c = buffer[i];
        if (c == '\n') {
          if (!overflow && !line.empty() && line.size() % 2 == 0) {
            deliver(line);
          }
          line.clear();
          overflow = false;
        } else if (!overflow) {
          if (line.size() < kMaxOutputLine) {
            line += c;
          } else {
            line.clear();
            overflow = true;
          }
        }
      }
    }
  }
  void deliver(const std::string& line) {
    auto nibble = [](char x) -> int {
      if (x >= '0' && x <= '9') {
        return x - '0';
      }
      if (x >= 'a' && x <= 'f') {
        return x - 'a' + 10;
      }
      return -1;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(line.size() / 2);
    for (std::size_t j = 0; j < line.size(); j += 2) {
      const auto a = nibble(line[j]);
      const auto b = nibble(line[j + 1]);
      if (a < 0 || b < 0) {
        return;
      }
      bytes.push_back(std::uint8_t((a << 4) | b));
    }
    inbox_.push(std::move(bytes));
  }

  pid_t process_ = -1;
  bool exited_ = false;
  int input_ = -1;
  int output_ = -1;
  std::thread reader_;
  std::atomic<bool> stop_reader_{false};
  ScriptInbox inbox_;
  ScriptOutbox outbox_;
  ScriptBridgeCounters counters_;
};

}  // namespace kcd2s::local
