#pragma once
#include <iostream>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <stdexcept>
#include "kcd2s/sdk/marshal.hpp"
#include "script_inbox.hpp"
#include "script_outbox.hpp"
namespace kcd2s::local {
// Text of a command line or path handed to a child process (UTF-16 on Windows).
using ProcessText = std::wstring;
inline ProcessText process_text(const std::string& utf8) {
  const auto n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  std::wstring result(std::size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), n);
  result.resize(std::size_t(n - 1));
  return result;
}
// Whether the child is the resource runtime (WebView2 host): it is started so that only
// Microsoft-signed DLLs load into it. Steam injects its overlay (gameoverlayrenderer64.dll) into
// the game's children through its process-start hook, and from there into the runtime's WebView2
// browser processes, where its own hook crashed them (5 kept dumps, access violation at
// gameoverlayrenderer64 + 0x94886): the runtime went down with World Architect's
// undo history. The runtime loads only Microsoft DLLs (its imports and the WebView2 runtime).
inline bool is_resource_runtime(const std::wstring& executable) {
  constexpr wchar_t kRuntime[] = L"kcd2s-resource-runtime.exe";
  const std::size_t length = wcslen(kRuntime);
  return executable.size() >= length &&
         _wcsicmp(executable.c_str() + executable.size() - length, kRuntime) == 0;
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
  // Pipe buffer requested in both directions. A larger buffer lets the caller hand over a
  // report without waiting for the child to read it.
  static constexpr DWORD kPipeBufferBytes = 1024 * 1024;
  // The longest line of the child's output, in hex digits (2 MiB of message): a recipe's picture
  // in the recipe book is 1 MiB. A longer line is dropped.
  static constexpr std::size_t kMaxOutputLine = 4u << 20;
  static constexpr std::size_t kInboxLimit = ScriptInbox::kMaxMessages;
  // The longest message to the resource host; a longer one is refused.
  static constexpr std::size_t kMaxMessageBytes = 65536;
  ScriptProcess(const std::wstring& node, const std::wstring& entry, const std::wstring& resources)
      : ScriptProcess(node, std::vector<std::wstring>{entry, resources}) {
  }
  ScriptProcess(const std::wstring& executable, const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE child_in = nullptr, child_out = nullptr;
    if (!CreatePipe(&child_in, &input_, &security, kPipeBufferBytes) ||
        !CreatePipe(&output_, &child_out, &security, kPipeBufferBytes))
      throw std::runtime_error("Resource host pipe creation failed");
    SetHandleInformation(input_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(output_, HANDLE_FLAG_INHERIT, 0);
    const bool microsoft_only = is_resource_runtime(executable);
    const DWORD attribute_count = microsoft_only ? 2 : 1;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &size);
    std::vector<std::uint8_t> attributes(size);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_in;
    startup.StartupInfo.hStdOutput = child_out;
    startup.StartupInfo.hStdError = child_out;
    startup.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    HANDLE inherited[]{child_in, child_out};
    const bool initialized = InitializeProcThreadAttributeList(startup.lpAttributeList,
                                                               attribute_count, 0, &size) != FALSE;
    const bool handles_set =
        initialized &&
        UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), nullptr, nullptr) != FALSE;
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BLOCK_NON_MICROSOFT_BINARIES_ALWAYS_ON;
    const bool policy_set =
        !microsoft_only || UpdateProcThreadAttribute(
                               startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
                               &policy, sizeof(policy), nullptr, nullptr) != FALSE;
    const bool updated = handles_set && policy_set;
    auto quoted = [](const std::wstring& value) {
      if (value.find(L'"') != std::wstring::npos)
        throw std::runtime_error("Invalid resource host path");
      return L"\"" + value + L"\"";
    };
    auto command = quoted(executable);
    for (const auto& argument : arguments)
      command += L" " + quoted(argument);
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool job_ready = job_ && SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                                           &limits, sizeof(limits));
    PROCESS_INFORMATION process{};
    const bool created =
        updated && job_ready &&
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                       nullptr, &startup.StartupInfo, &process) != FALSE;
    if (initialized)
      DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(child_in);
    CloseHandle(child_out);
    if (!created) {
      CloseHandle(input_);
      CloseHandle(output_);
      if (job_)
        CloseHandle(job_);
      input_ = output_ = job_ = nullptr;
      throw std::runtime_error("Resource host start failed");
    }
    if (!AssignProcessToJobObject(job_, process.hProcess) ||
        ResumeThread(process.hThread) == DWORD(-1)) {
      TerminateProcess(process.hProcess, 1);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
      CloseHandle(input_);
      CloseHandle(output_);
      CloseHandle(job_);
      input_ = output_ = job_ = nullptr;
      throw std::runtime_error("Resource host job setup failed");
    }
    process_ = process.hProcess;
    CloseHandle(process.hThread);
    reader_ = std::thread([this] { read_output(); });
    outbox_.start([this](const std::string& line) { return write_line(line); },
                  [this] {
                    CloseHandle(input_);
                    input_ = nullptr;
                  });
  }
  ~ScriptProcess() {
    // Closing stdin, after what is queued, asks the host to stop. A host that does not read is
    // ended below, which also frees the writing thread.
    outbox_.finish();
    if (process_ && WaitForSingleObject(process_, 11000) == WAIT_TIMEOUT)
      TerminateProcess(process_, 1);
    outbox_.join();
    if (job_)
      CloseHandle(job_);
    job_ = nullptr;
    if (reader_.joinable()) {
      CancelSynchronousIo(reader_.native_handle());
      reader_.join();
    }
    if (output_)
      CloseHandle(output_);
    if (process_)
      CloseHandle(process_);
  }
  ScriptProcess(const ScriptProcess&) = delete;
  ScriptProcess& operator=(const ScriptProcess&) = delete;
  bool alive() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
  }
  bool send(const sdk::Value& value) {
    inbox_.check();
    if (!input_ || !alive())
      return false;
    const auto bytes = sdk::encode(value);
    if (bytes.size() > kMaxMessageBytes) {
      std::cerr << "RESOURCE message of " << bytes.size() << " bytes too long for the host"
                << std::endl;
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
  bool write_line(const std::string& line) {
    DWORD written = 0;
    return WriteFile(input_, line.data(), DWORD(line.size()), &written, nullptr) &&
           written == line.size();
  }
  void read_output() {
    std::string line;
    char buffer[4096];
    DWORD size = 0;
    bool overflow = false;
    while (ReadFile(output_, buffer, sizeof(buffer), &size, nullptr) && size) {
      for (DWORD i = 0; i < size; ++i) {
        const char c = buffer[i];
        if (c == '\n') {
          if (!overflow && !line.empty() && line.size() % 2 == 0) {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(line.size() / 2);
            bool valid = true;
            auto nibble = [](char x) -> int {
              if (x >= '0' && x <= '9')
                return x - '0';
              if (x >= 'a' && x <= 'f')
                return x - 'a' + 10;
              return -1;
            };
            for (std::size_t j = 0; j < line.size(); j += 2) {
              const auto a = nibble(line[j]), b = nibble(line[j + 1]);
              if (a < 0 || b < 0) {
                valid = false;
                break;
              }
              bytes.push_back(std::uint8_t((a << 4) | b));
            }
            if (valid) {
              inbox_.push(std::move(bytes));
            }
          }
          line.clear();
          overflow = false;
        } else if (!overflow) {
          if (line.size() < kMaxOutputLine)
            line += c;
          else {
            line.clear();
            overflow = true;
          }
        }
      }
    }
  }
  HANDLE process_ = nullptr, input_ = nullptr, output_ = nullptr, job_ = nullptr;
  std::thread reader_;
  ScriptInbox inbox_;
  ScriptOutbox outbox_;
  ScriptBridgeCounters counters_;
};
}  // namespace kcd2s::local
