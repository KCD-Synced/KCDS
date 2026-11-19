// The download mark on the client's own programs. A KCDS client unpacked from a downloaded zip
// keeps the browser's "from the internet" mark (the Zone.Identifier stream) on every file, and
// Windows SmartScreen then stops kcds-web-helper.exe, which the web views start, as an
// unrecognised download ("Windows protected your PC"), and the web views stay empty. The client's
// files are checked by the launcher before they run (the release's signatures), so the client
// takes the mark off its own programs in its own folder before anything starts them.
#pragma once

#include <string>

namespace kcds::runtime::install {

// Whether a file of the client's folder is a program whose mark is taken off: an .exe or a .dll,
// in any case. Portable: the unit test checks it.
bool takes_mark_off(const std::wstring& file_name);

// Takes the download mark off the programs in `folder` (not its subfolders); the log line says
// how many had one. Windows only.
std::string clear_download_marks(const std::wstring& folder);

}  // namespace kcds::runtime::install
