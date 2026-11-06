// KCD2-Synced — custom-menu OVERLAY (MVP of "replace the native menu").
//
// Injected into KingdomCome.exe. Because the game runs windowed, we put our real HTML
// server-browser (Client/ui/menu/index.html) in a borderless, top-most window sized exactly over
// the game's client area, hosted by WebView2 (the Edge/Chromium runtime shipped with Windows). When
// shown it covers the native main menu — the fast, low-risk path to a working custom menu before
// the full CEF-in-swapchain compositor.
//
// Behaviour: the menu AUTO-SHOWS on start — it is the player's entry point, so it comes up by
// itself over the native main menu as soon as the page paints, no key press required. F9 then
// toggles it hidden/shown for dev; F10 reloads the page from disk (iterate the HTML without
// re-injecting — do NOT inject a second overlay into a live process, it crashes the host). The
// overlay follows the game window as it moves/resizes. Log: %LOCALAPPDATA%\KCD2-Synced\overlay.log
//
// NUI bridge is wired here:
//   page -> host : fetch("https://kcd2s-menu/<cb>") is intercepted (WebResourceRequested) — we see
//                  ready / refresh / connect / direct / cancel / entered and answer (with CORS).
//   host -> page : window.postMessage({type,...}) via ExecuteScript — we push "self" + "servers".
// The directory served here is a STUB (see servers_message), standing in for the real FrontendMenu
// + MasterClient until the master-list client is linked in. connect/direct emit the intent to the
// log and report a "next milestone" failure rather than dialling — live KSTP dialling is the next
// step.
//
// This is NOT the shipped renderer — it is a separate top-most window, so it only works in
// windowed/ borderless mode and does not yet composite into the swapchain (CEF OSR -> DX12 in the
// Present hook).

#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <objbase.h>  // CreateStreamOnHGlobal — synthesized NUI fetch response bodies
#include <wrl.h>
#include <wrl/event.h>

#include <cstdio>
#include <string>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

using namespace Microsoft::WRL;

namespace {

FILE* g_log = nullptr;
void logf(const char* fmt, ...) {
  if (!g_log)
    return;
  SYSTEMTIME t;
  GetLocalTime(&t);
  fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fprintf(g_log, "\n");
  fflush(g_log);
}
void log_open() {
  char path[MAX_PATH];
  const char* base = getenv("LOCALAPPDATA");
  if (base) {
    char d[MAX_PATH];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "%s\\KCD2-Synced", base);
    CreateDirectoryA(d, nullptr);
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\overlay.log", d);
  } else
    _snprintf_s(path, sizeof(path), _TRUNCATE, "C:\\kcd2s_overlay.log");
  g_log = _fsopen(path, "w", 0x40 /*_SH_DENYNO*/);
}

// The menu page to load: the env var KCD2S_MENU_URL (a file:/// or http(s):// URL), or a blank
// page without it.
std::wstring menu_url() {
  wchar_t* over = nullptr;
  size_t n = 0;
  if (_wdupenv_s(&over, &n, L"KCD2S_MENU_URL") == 0 && over && *over) {
    std::wstring u = over;
    free(over);
    return u;
  }
  if (over)
    free(over);
  return L"about:blank";
}

HWND g_gameWnd = nullptr;  // KingdomCome.exe main window
HWND g_overlay = nullptr;  // our borderless popup
bool g_visible = false;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
ComPtr<ICoreWebView2Environment> g_env;  // kept so WebResourceRequested can synthesize responses
bool g_autoShown = false;                // the menu auto-shows exactly once, on first page load
int g_refresh = 0;                       // bumped per refresh so the stub list visibly changes

// Chroma key for see-through: anywhere the page is transparent, WebView2 paints this exact colour
// (its DefaultBackgroundColor) and the layered window keys it out, so the GAME shows through there.
// A near-black key keeps the thin anti-alias fringe around panels dark and unnoticeable. Keying a
// layered window only catches WebView2's pixels when GPU compositing is off (see --disable-gpu).
constexpr BYTE kKeyR = 10, kKeyG = 8, kKeyB = 6;

// Find this process's main top-level visible window.
HWND find_game_window() {
  struct Ctx {
    DWORD pid;
    HWND hwnd;
  } ctx{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != c->pid)
          return TRUE;
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr)
          return TRUE;
        RECT r;
        GetWindowRect(h, &r);
        if ((r.right - r.left) < 320 || (r.bottom - r.top) < 240)
          return TRUE;  // skip tiny/helper windows
        c->hwnd = h;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.hwnd;
}

// Place the overlay exactly over the game's client area (screen coords).
void reposition() {
  if (!g_overlay || !g_gameWnd)
    return;
  RECT rc;
  if (!GetClientRect(g_gameWnd, &rc))
    return;
  POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
  ClientToScreen(g_gameWnd, &tl);
  ClientToScreen(g_gameWnd, &br);
  SetWindowPos(g_overlay, HWND_TOPMOST, tl.x, tl.y, br.x - tl.x, br.y - tl.y,
               SWP_NOACTIVATE | (g_visible ? 0 : SWP_HIDEWINDOW));
  if (g_controller) {
    RECT b{0, 0, br.x - tl.x, br.y - tl.y};
    g_controller->put_Bounds(b);
  }
}

void set_visible(bool v) {
  g_visible = v;
  if (!g_overlay)
    return;
  if (v) {
    reposition();
    ShowWindow(g_overlay, SW_SHOW);
    SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_overlay);
    if (g_controller) {
      g_controller->put_IsVisible(TRUE);
      g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    logf("overlay shown");
  } else {
    if (g_controller)
      g_controller->put_IsVisible(FALSE);
    ShowWindow(g_overlay, SW_HIDE);
    if (g_gameWnd)
      SetForegroundWindow(g_gameWnd);  // hand input back to the game
    logf("overlay hidden");
  }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_HOTKEY:
      if (wp == 1)
        set_visible(!g_visible);  // F9  — toggle the menu
      else if (wp == 2 && g_webview) {
        g_webview->Reload();
        logf("menu reloaded (F10)");
      }  // F10 — reload the page from disk
      return 0;
    case WM_TIMER:
      if (g_visible)
        reposition();
      return 0;
    case WM_SIZE:
      if (g_controller) {
        RECT b;
        GetClientRect(h, &b);
        g_controller->put_Bounds(b);
      }
      return 0;
    case WM_CLOSE:
      set_visible(false);
      return 0;  // never actually destroy from a game-close
  }
  return DefWindowProcW(h, msg, wp, lp);
}

// ---- NUI bridge helpers -----------------------------------------------------

// UTF-8 -> UTF-16 for ExecuteScript / headers.
std::wstring widen(const std::string& s) {
  if (s.empty())
    return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

// host -> page: deliver a JSON object to the page's window 'message' listener. WebView2's
// PostWebMessage* channel targets window.chrome.webview, but the menu listens on the standard
// window 'message' event (the CEF/NUI contract), so we dispatch it via window.postMessage.
void post_to_page(const std::string& json_obj) {
  if (!g_webview)
    return;
  std::wstring script = L"try{window.postMessage(" + widen(json_obj) + L", '*');}catch(e){}";
  g_webview->ExecuteScript(
      script.c_str(),
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT, LPCWSTR) -> HRESULT {
        return S_OK;
      }).Get());
}

// ISO-8601 UTC timestamp `sec` seconds in the past (for the stub list's lastSeen).
std::string iso_ago(int sec) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  u.QuadPart -= (ULONGLONG)sec * 10000000ULL;
  ft.dwLowDateTime = u.LowPart;
  ft.dwHighDateTime = u.HighPart;
  SYSTEMTIME st;
  FileTimeToSystemTime(&ft, &st);
  char b[40];
  _snprintf_s(b, sizeof(b), _TRUNCATE, "%04d-%02d-%02dT%02d:%02d:%02d.000Z", st.wYear, st.wMonth,
              st.wDay, st.wHour, st.wMinute, st.wSecond);
  return b;
}

// The STUB server directory (real ServerEntry shape) — stands in for FrontendMenu + MasterClient
// until the master-list client is linked into the real client. Shaped exactly like the rows the
// menu renders (Client/ui/menu/index.html, Backend/platform/server_browser.hpp).
std::string servers_message() {
  struct S {
    const char* id;
    const char* name;
    const char* addr;
    const char* mode;
    int pl;
    int mx;
    const char* tags;
    int proto;
    int seen;
  };
  static const S base[] = {
      {"trosky", "Trosky Roleplay", "rp.example.com:27015", "roleplay", 148, 200,
       "\"eu\",\"rp\",\"whitelist\"", 1, 2},
      {"kutt", "Kuttenberg Tournament", "eu2.example.com:27015", "tournament", 31, 32,
       "\"eu\",\"pvp\"", 1, 5},
      {"eu-1", "Rattay Freeroam", "play.example.com:27015", "freeroam", 12, 32, "\"eu\",\"pve\"", 1,
       9},
      {"sase", "Sasau Monastery RP", "eu3.example.com:27015", "roleplay", 66, 100, "\"eu\",\"rp\"",
       1, 3},
      {"old-1", "Old-Build Testbed", "test.example.com:27015", "freeroam", 3, 16, "\"eu\"", 2, 44},
  };
  std::string out = "{\"type\":\"servers\",\"servers\":[";
  const int n = (int)(sizeof(base) / sizeof(base[0]));
  for (int i = 0; i < n; ++i) {
    const S& s = base[i];
    int jit = s.proto == 1 ? ((g_refresh * (i + 1)) % 7) - 3 : 0;
    int pl = s.pl + jit;
    if (pl < 0)
      pl = 0;
    if (pl > s.mx)
      pl = s.mx;
    char buf[640];
    _snprintf_s(
        buf, sizeof(buf), _TRUNCATE,
        "%s{\"id\":\"%s\",\"name\":\"%s\",\"address\":\"%s\",\"gamemode\":\"%s\","
        "\"players\":%d,\"maxPlayers\":%d,\"tags\":[%s],\"protocol\":%d,\"lastSeen\":\"%s\"}",
        i ? "," : "", s.id, s.name, s.addr, s.mode, pl, s.mx, s.tags, s.proto,
        iso_ago(s.seen).c_str());
    out += buf;
  }
  out += "]}";
  return out;
}

// Crude "key":"value" string extractor for the tiny inbound NUI bodies (logging/echo only).
std::string json_field(const std::string& body, const char* key) {
  std::string pat = std::string("\"") + key + "\"";
  auto p = body.find(pat);
  if (p == std::string::npos)
    return "";
  p = body.find(':', p + pat.size());
  if (p == std::string::npos)
    return "";
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '"'))
    ++p;
  auto e = p;
  while (e < body.size() && body[e] != '"' && body[e] != ',' && body[e] != '}')
    ++e;
  return body.substr(p, e - p);
}

std::string read_stream(IStream* s) {
  std::string out;
  if (!s)
    return out;
  char buf[4096];
  ULONG r = 0;
  while (SUCCEEDED(s->Read(buf, sizeof(buf), &r)) && r) {
    out.append(buf, r);
    if (r < sizeof(buf))
      break;
  }
  return out;
}

// A CORS-permissive JSON response so the page's cross-origin fetch (file:// -> https://kcd2s-menu)
// clears preflight and the POST actually reaches us.
ComPtr<ICoreWebView2WebResourceResponse> make_response(int status, const std::string& body) {
  ComPtr<IStream> stream;
  CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (stream && !body.empty()) {
    ULONG w = 0;
    stream->Write(body.data(), (ULONG)body.size(), &w);
  }
  if (stream) {
    LARGE_INTEGER z{};
    stream->Seek(z, STREAM_SEEK_SET, nullptr);
  }
  const wchar_t* headers =
      L"Access-Control-Allow-Origin: *\r\n"
      L"Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
      L"Access-Control-Allow-Headers: Content-Type\r\n"
      L"Content-Type: application/json; charset=UTF-8";
  ComPtr<ICoreWebView2WebResourceResponse> resp;
  if (g_env)
    g_env->CreateWebResourceResponse(stream.Get(), status, status == 200 ? L"OK" : L"No Content",
                                     headers, &resp);
  return resp;
}

// page -> host: the NUI callback router. Drives the matching host -> page pushes.
void handle_nui(const std::wstring& cb, const std::string& body) {
  logf("page->host: %ls %s", cb.c_str(), body.c_str());
  if (cb == L"ready" || cb == L"refresh") {
    if (cb == L"refresh")
      ++g_refresh;
    post_to_page("{\"type\":\"self\",\"protocol\":1}");
    post_to_page(servers_message());
    logf("  host->page: pushed self + servers (refresh#%d)", g_refresh);
  } else if (cb == L"connect" || cb == L"direct") {
    std::string who = cb == L"connect" ? json_field(body, "id") : json_field(body, "address");
    logf("  connect intent -> %s (not dialled: next bridge milestone)", who.c_str());
    post_to_page("{\"type\":\"state\",\"state\":\"failed\",\"name\":\"" + who +
                 "\",\"reason\":\"Connect intent reached the host (see overlay.log). Live KSTP "
                 "dialling + on_connect_result land in the next bridge milestone.\"}");
  }
  // cancel / entered: nothing to drive from the host yet.
}

void create_webview() {
  std::wstring userData;
  wchar_t* la = nullptr;
  size_t n = 0;
  if (_wdupenv_s(&la, &n, L"LOCALAPPDATA") == 0 && la) {
    userData = la;
    free(la);
  }
  userData += L"\\KCD2-Synced\\wv2";

  // --disable-gpu routes WebView2 through the GDI redirection surface instead of a direct DComp
  // visual, which is what makes the layered-window colour key actually catch its pixels (GPU
  // compositing bypasses the key, leaving the overlay opaque). Fine for a static menu page.
  auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
  options->put_AdditionalBrowserArguments(L"--disable-gpu");

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userData.c_str(), options.Get(),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(r) || !env) {
              logf("env create FAILED hr=0x%lx", r);
              return r;
            }
            g_env = env;  // kept so WebResourceRequested can synthesize responses
            env->CreateCoreWebView2Controller(
                g_overlay,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                      if (FAILED(r2) || !ctrl) {
                        logf("controller create FAILED hr=0x%lx", r2);
                        return r2;
                      }
                      g_controller = ctrl;
                      g_controller->get_CoreWebView2(&g_webview);
                      RECT b;
                      GetClientRect(g_overlay, &b);
                      g_controller->put_Bounds(b);
                      if (!g_webview)
                        return S_OK;

                      // Paint the page's transparent regions in the chroma key so the layered
                      // window keys them out (-> game shows through). Needs
                      // ICoreWebView2Controller2.
                      ComPtr<ICoreWebView2Controller2> ctrl2;
                      if (SUCCEEDED(g_controller.As(&ctrl2)) && ctrl2)
                        ctrl2->put_DefaultBackgroundColor(
                            COREWEBVIEW2_COLOR{255, kKeyR, kKeyG, kKeyB});

                      // page -> host: intercept the NUI pseudo-URL fetches.
                      EventRegistrationToken tok{};
                      g_webview->AddWebResourceRequestedFilter(
                          L"https://kcd2s-menu/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                      g_webview->add_WebResourceRequested(
                          Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                              [](ICoreWebView2*,
                                 ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                ComPtr<ICoreWebView2WebResourceRequest> req;
                                if (!args || FAILED(args->get_Request(&req)) || !req)
                                  return S_OK;
                                LPWSTR uriRaw = nullptr;
                                req->get_Uri(&uriRaw);
                                std::wstring uri = uriRaw ? uriRaw : L"";
                                if (uriRaw)
                                  CoTaskMemFree(uriRaw);
                                LPWSTR methRaw = nullptr;
                                req->get_Method(&methRaw);
                                std::wstring method = methRaw ? methRaw : L"";
                                if (methRaw)
                                  CoTaskMemFree(methRaw);

                                // CORS preflight: allow it so the real POST is delivered to us.
                                if (method == L"OPTIONS") {
                                  args->put_Response(make_response(200, "").Get());
                                  return S_OK;
                                }

                                std::wstring cb;
                                auto key = uri.find(L"kcd2s-menu/");
                                if (key != std::wstring::npos) {
                                  cb = uri.substr(key + 11);
                                  auto q = cb.find_first_of(L"?#");
                                  if (q != std::wstring::npos)
                                    cb.resize(q);
                                  while (!cb.empty() && cb.back() == L'/')
                                    cb.pop_back();
                                }
                                std::string body;
                                ComPtr<IStream> content;
                                if (SUCCEEDED(req->get_Content(&content)) && content)
                                  body = read_stream(content.Get());

                                handle_nui(cb, body);
                                args->put_Response(make_response(200, "{}").Get());
                                return S_OK;
                              })
                              .Get(),
                          &tok);

                      // Auto-show once the menu has actually painted — no black flash, no key
                      // press.
                      g_webview->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [](ICoreWebView2*,
                                 ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                if (!g_autoShown) {
                                  g_autoShown = true;
                                  set_visible(true);
                                  logf("menu loaded -> auto-shown");
                                }
                                return S_OK;
                              })
                              .Get(),
                          &tok);

                      std::wstring url = menu_url();
                      g_webview->Navigate(url.c_str());
                      logf("navigating to menu: %ls", url.c_str());
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr))
    logf("CreateCoreWebView2EnvironmentWithOptions FAILED hr=0x%lx", hr);
}

DWORD WINAPI overlay_thread(LPVOID) {
  log_open();
  logf("=== KCD2-Synced menu overlay ===");
  HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  logf("CoInitializeEx hr=0x%lx", co);

  for (int i = 0; i < 100 && !g_gameWnd; ++i) {
    g_gameWnd = find_game_window();
    if (!g_gameWnd)
      Sleep(100);
  }
  if (!g_gameWnd) {
    logf("FATAL: game window not found");
    return 1;
  }
  RECT gr;
  GetWindowRect(g_gameWnd, &gr);
  logf("game window %p rect=%ld,%ld %ldx%ld", g_gameWnd, gr.left, gr.top, gr.right - gr.left,
       gr.bottom - gr.top);

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"KCD2SyncedMenuOverlay";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground =
      CreateSolidBrush(RGB(kKeyR, kKeyG, kKeyB));  // key colour -> unpainted area is see-through
  RegisterClassExW(&wc);

  // WS_EX_LAYERED makes the window a candidate for colour-key transparency so the game shows
  // through. No WS_EX_NOACTIVATE: the menu must be able to take activation, or Chromium ignores
  // mouse/keyboard.
  g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"", WS_POPUP,
                              gr.left, gr.top, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
  if (!g_overlay) {
    logf("FATAL: CreateWindowEx failed %lu", GetLastError());
    return 1;
  }
  // Key out the chroma colour: those pixels (the page's transparent regions, plus the class brush)
  // become see-through to the game behind.
  SetLayeredWindowAttributes(g_overlay, RGB(kKeyR, kKeyG, kKeyB), 0, LWA_COLORKEY);
  reposition();

  RegisterHotKey(g_overlay, 1, 0, VK_F9);  // F9  toggles the menu
  RegisterHotKey(g_overlay, 2, 0,
                 VK_F10);  // F10 reloads the page from disk (iterate UI without re-injecting)
  SetTimer(g_overlay, 1, 250, nullptr);  // follow the game window
  logf("overlay window %p created; creating WebView2…", g_overlay);
  create_webview();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(inst);
    CreateThread(nullptr, 0, overlay_thread, nullptr, 0, nullptr);
  }
  return TRUE;
}

#endif  // _WIN32
