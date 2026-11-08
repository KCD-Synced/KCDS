// Crash-time copy of a log's tail (Windows). See kcds/launcher/crash_logs.hpp.
//
// This runs inside the crash handler of a crashing process, so it allocates
// no heap memory and uses only Win32 file calls: one static buffer (guarded
// against a concurrent second crash) and fixed-size stack text.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwchar>

#include "kcds/launcher/crash_logs.hpp"

namespace kcds::launcher::crash {

namespace {

constexpr DWORD kBufferBytes = 16 * 1024;
char tail_buffer[kBufferBytes];
std::atomic_flag tail_buffer_busy = ATOMIC_FLAG_INIT;

bool write_all(HANDLE file, const char* data, DWORD length) {
  while (length > 0) {
    DWORD written = 0;
    if (!WriteFile(file, data, length, &written, nullptr) || written == 0) {
      return false;
    }
    data += written;
    length -= written;
  }
  return true;
}

// Copies [offset, offset + count) of source into target. When skip_partial_line
// is set, everything up to and including the first line break is dropped.
bool copy_range(HANDLE source, HANDLE target, std::uint64_t offset, std::uint64_t count,
                bool skip_partial_line) {
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(source, position, nullptr, FILE_BEGIN)) {
    return false;
  }

  bool skipping = skip_partial_line;
  while (count > 0) {
    DWORD wanted = count < kBufferBytes ? static_cast<DWORD>(count) : kBufferBytes;
    DWORD read = 0;
    if (!ReadFile(source, tail_buffer, wanted, &read, nullptr)) {
      return false;
    }
    if (read == 0) {
      break;  // the log shrank meanwhile; keep what was copied
    }
    count -= read;

    DWORD start = 0;
    if (skipping) {
      while (start < read && tail_buffer[start] != '\n') {
        ++start;
      }
      if (start == read) {
        continue;  // still inside the partial first line
      }
      ++start;
      skipping = false;
    }
    if (!write_all(target, tail_buffer + start, read - start)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool copy_log_tail(const wchar_t* source, const wchar_t* snapshot,
                   std::uint64_t max_bytes) noexcept {
  if (source == nullptr || snapshot == nullptr || max_bytes == 0) {
    return false;
  }
  if (tail_buffer_busy.test_and_set()) {
    return false;
  }

  bool copied = false;
  // The process still holds the log open for writing, so share everything.
  HANDLE input =
      CreateFileW(source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  LARGE_INTEGER size;
  if (input != INVALID_HANDLE_VALUE && GetFileSizeEx(input, &size) && size.QuadPart >= 0) {
    HANDLE output = CreateFileW(snapshot, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output != INVALID_HANDLE_VALUE) {
      std::uint64_t total = static_cast<std::uint64_t>(size.QuadPart);
      bool truncated = total > max_bytes;
      std::uint64_t offset = truncated ? total - max_bytes : 0;
      std::uint64_t count = truncated ? max_bytes : total;

      bool header_ok = true;
      if (truncated) {
        char header[160];
        int length = _snprintf_s(
            header, sizeof(header), _TRUNCATE, "[kcds: log truncated, last %llu of %llu bytes]\r\n",
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(total));
        header_ok = length > 0 && write_all(output, header, static_cast<DWORD>(length));
      }
      copied = header_ok && copy_range(input, output, offset, count, truncated);
      CloseHandle(output);
      if (!copied) {
        DeleteFileW(snapshot);
      }
    }
  }
  if (input != INVALID_HANDLE_VALUE) {
    CloseHandle(input);
  }

  tail_buffer_busy.clear();
  return copied;
}

bool copy_newest_file(const wchar_t* pattern, const wchar_t* snapshot, std::uint64_t max_bytes,
                      std::uint64_t within_seconds) noexcept {
  if (pattern == nullptr || snapshot == nullptr) {
    return false;
  }
  // The folder part of the pattern, for the found file's full path.
  constexpr std::size_t kPathChars = 1024;
  wchar_t path[kPathChars];
  std::size_t folder_length = 0;
  for (std::size_t i = 0; pattern[i] != L'\0'; ++i) {
    if (i + 1 >= kPathChars) {
      return false;
    }
    if (pattern[i] == L'\\' || pattern[i] == L'/') {
      folder_length = i + 1;
    }
  }
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW(pattern, &found);
  if (search == INVALID_HANDLE_VALUE) {
    DeleteFileW(snapshot);
    return false;
  }
  ULARGE_INTEGER newest{};
  wchar_t newest_name[MAX_PATH] = L"";
  std::uint64_t newest_size = 0;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    ULARGE_INTEGER written;
    written.LowPart = found.ftLastWriteTime.dwLowDateTime;
    written.HighPart = found.ftLastWriteTime.dwHighDateTime;
    if (written.QuadPart > newest.QuadPart) {
      newest = written;
      newest_size = (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
      wcsncpy_s(newest_name, found.cFileName, _TRUNCATE);
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);

  FILETIME now_time;
  GetSystemTimeAsFileTime(&now_time);
  ULARGE_INTEGER now;
  now.LowPart = now_time.dwLowDateTime;
  now.HighPart = now_time.dwHighDateTime;
  // File times count 100-nanosecond steps.
  const std::uint64_t window = within_seconds * 10000000ull;
  const bool recent = newest.QuadPart != 0 && now.QuadPart >= newest.QuadPart &&
                      now.QuadPart - newest.QuadPart <= window;
  const std::size_t name_length = wcsnlen_s(newest_name, MAX_PATH);
  if (!recent || newest_size == 0 || newest_size > max_bytes ||
      folder_length + name_length + 1 > kPathChars) {
    DeleteFileW(snapshot);
    return false;
  }
  wmemcpy(path, pattern, folder_length);
  wmemcpy(path + folder_length, newest_name, name_length);
  path[folder_length + name_length] = L'\0';
  if (!CopyFileW(path, snapshot, FALSE)) {
    DeleteFileW(snapshot);
    return false;
  }
  return true;
}

}  // namespace kcds::launcher::crash
