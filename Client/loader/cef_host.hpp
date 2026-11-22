// The process's one CEF (Chromium Embedded Framework), owned by the loader: Chromium can be
// initialized once per process, and a client of another KCDS release loaded later (the swap) keeps
// using it. The first client starts it with its settings; every client then opens and closes its
// own browser views, and registers its own kcds:// scheme handlers once CEF is ready.
#pragma once

#include "kcds/loader/kcds_abi.h"

namespace kcds::loader::cef {

// Starts CEF on its own thread with these settings, once; later calls change nothing. 0 when it
// starts or has started.
int start(const KcdsCefSettings* settings, void (*logger)(const char*));

// 1 once CEF is ready, -1 when it failed, 0 before.
int ready();

}  // namespace kcds::loader::cef
