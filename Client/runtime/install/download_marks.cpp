#include "download_marks.hpp"

#include <cwctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kcds::runtime::install {

namespace {

bool ends_with(const std::wstring& text, const wchar_t* ending) {
  const std::wstring tail(ending);
  if (text.size() < tail.size()) {
    return false;
  }
  for (std::size_t index = 0; index < tail.size(); ++index) {
    const wchar_t got = text[text.size() - tail.size() + index];
    if (wchar_t(std::towlower(wint_t(got))) != tail[index]) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool takes_mark_off(const std::wstring& file_name) {
  // A name made of only the extension (".exe") is no program of ours.
  if (file_name.size() <= 4) {
    return false;
  }
  return ends_with(file_name, L".exe") || ends_with(file_name, L".dll");
}

#ifdef _WIN32
std::string clear_download_marks(const std::wstring& folder) {
  std::wstring base = folder;
  if (!base.empty() && base.back() != L'\\' && base.back() != L'/') {
    base += L'\\';
  }
  WIN32_FIND_DATAW found = {};
  HANDLE search = FindFirstFileW((base + L"*").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) {
    return "INSTALL download marks: the client's folder could not be listed";
  }
  int programs = 0;
  int marked = 0;
  int kept = 0;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    const std::wstring name = found.cFileName;
    if (!takes_mark_off(name)) {
      continue;
    }
    ++programs;
    const std::wstring mark = base + name + L":Zone.Identifier";
    if (DeleteFileW(mark.c_str())) {
      ++marked;
    } else {
      const DWORD error = GetLastError();
      // No mark is the usual case (the launcher wrote the file); anything else is noted.
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        ++kept;
      }
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  return "INSTALL download marks: " + std::to_string(marked) + " of " + std::to_string(programs) +
         " programs had one and lost it" +
         (kept > 0 ? ", " + std::to_string(kept) + " could not be changed" : std::string());
}
#else
std::string clear_download_marks(const std::wstring&) {
  return "INSTALL download marks: not on this system";
}
#endif

}  // namespace kcds::runtime::install
