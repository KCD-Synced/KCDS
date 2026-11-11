// Our chat is drawn into the game's DX12 frame. No engine objects are accessed here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <deque>
#include <algorithm>
#include <vector>
#include "MinHook.h"
#include "../architect/editor_overlay.hpp"
#include "../branding/watermark.hpp"
#include "../branding/window_icon.hpp"
#include "../characters/pipeline_metrics.hpp"
#include "../nameplates/nameplate_paint.hpp"
#include "../render/render_view.hpp"
#include "../render/world_textures.hpp"
#include "../voice/voice_indicator.hpp"
#include "chat_overlay.hpp"
namespace kcd2s::native_chat {
using Microsoft::WRL::ComPtr;
namespace {
Logger log;
std::atomic<bool> ready{false}, open{false}, engine_console_open{false};
// A crafting minigame of the local player runs: T is its help key there.
std::atomic<bool> minigame_open{false};
// T presses while a resource hid the chat (Chat.setUIVisible(false)), for its own chat window.
std::atomic<unsigned> hidden_chat_keys{0};
// Text capture (Book.write): the input line takes keys without drawing; Enter adds a line break
// and Esc ends it. capture_changed/capture_ended are guarded by state_mutex.
std::atomic<bool> capture{false};
bool capture_changed = false;
bool capture_ended = false;
constexpr std::size_t kCaptureMax = 4000;
std::atomic<bool> overlay_visible{true};
// GetTickCount64 of the last damage flash; 0 when none.
std::atomic<ULONGLONG> damage_flash{0};
// The wheel turned since take_wheel last read it (WM_MOUSEWHEEL deltas).
std::atomic<int> wheel_turned{0};
constexpr int kMostWheel = 120 * 1000;
constexpr ULONGLONG kDamageFlashMs = 1200;
std::mutex state_mutex, graphics_mutex;
struct Line {
  std::wstring text;
  std::uint32_t color;
  ULONGLONG time;
};
std::deque<Line> lines;
std::vector<std::string> submissions;
std::wstring draft;
HWND game_window{};
WNDPROC previous_proc{};
bool skip_open_character{};
wchar_t pending_surrogate{};
using Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Resize = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using CreateChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                IDXGISwapChain**);
using CreateChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                       const DXGI_SWAP_CHAIN_DESC1*,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                       IDXGIOutput*, IDXGISwapChain1**);
Present original_present{};
Resize original_resize{};
Execute original_execute{};
CreateChain original_create_chain{};
CreateChainForHwnd original_create_chain_for_hwnd{};
thread_local bool drawing{};
ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D12Device> queue_device;
// The queue each swap chain was made with: the queue its frames are presented on, which the
// overlay draws on too (another direct queue has no order with the frame's work).
struct ChainQueue {
  IDXGISwapChain* chain;
  ComPtr<ID3D12CommandQueue> queue;
};
std::vector<ChainQueue> chain_queues;
// The first direct queue the game used: the overlay's queue when the swap chain was made before
// the hooks.
ComPtr<ID3D12CommandQueue> first_queue;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
ComPtr<ID3D11On12Device> on12;
ComPtr<ID2D1Factory1> factory;
ComPtr<ID2D1Device> d2device;
ComPtr<ID2D1DeviceContext> canvas;
ComPtr<IDWriteFactory> text_factory;
ComPtr<IDWriteTextFormat> font;
ComPtr<ID2D1SolidColorBrush> brush;
struct Buffer {
  ComPtr<ID3D12Resource> back;
  ComPtr<ID3D11Resource> wrapped;
  ComPtr<ID2D1Bitmap1> bitmap;
};
std::vector<Buffer> buffers;
IDXGISwapChain* active_chain{};
std::atomic<bool (*)()> view_layer_active{nullptr};
std::atomic<void (*)(ID2D1DeviceContext*, float, float, void*)> view_layer_paint{nullptr};
std::atomic<void (*)(int, int)> frame_size_observer{nullptr};
std::atomic<InputSink> input_sink{nullptr};
std::atomic<CloseHandler> close_handler{nullptr};
bool initialized{};
std::wstring wide(const std::string& s) {
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
  if (size <= 0)
    return {};
  std::wstring out(std::size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), size);
  return out;
}
std::string utf8(const std::wstring& s) {
  const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (size <= 0)
    return {};
  std::string out(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), out.data(), size,
                      nullptr, nullptr);
  return out;
}
wchar_t dead_accent{};
void append_key(WPARAM key, LPARAM key_data) {
  if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU)
    return;
  BYTE state[256]{};
  if (!GetKeyboardState(state))
    return;
  wchar_t chars[8]{};
  const auto layout = GetKeyboardLayout(GetWindowThreadProcessId(game_window, nullptr));
  const auto scan = UINT((key_data >> 16) & 255);
  const int count = ToUnicodeEx(UINT(key), scan, state, chars, 8, 4, layout);
  if (count < 0) {
    dead_accent = chars[0];
    return;
  }
  if (count <= 0)
    return;
  std::wstring addition(chars, chars + std::min(count, 8));
  if (dead_accent) {
    wchar_t combining = 0;
    switch (dead_accent) {
      case L'\u00b4':
        combining = L'\u0301';
        break;
      case L'`':
        combining = L'\u0300';
        break;
      case L'^':
        combining = L'\u0302';
        break;
      case L'~':
        combining = L'\u0303';
        break;
      case L'\u02c7':
        combining = L'\u030c';
        break;
      case L'\u00a8':
        combining = L'\u0308';
        break;
      case L'\u02da':
        combining = L'\u030a';
        break;
      case L'\u02dd':
        combining = L'\u030b';
        break;
    }
    if (addition == L" ")
      addition = std::wstring(1, dead_accent);
    else if (combining) {
      auto composed = addition + combining;
      wchar_t out[16]{};
      auto n = FoldStringW(MAP_PRECOMPOSED, composed.data(), int(composed.size()), out, 16);
      if (n > 0)
        addition.assign(out, out + n);
    } else
      addition = std::wstring(1, dead_accent) + addition;
    dead_accent = 0;
  }
  auto candidate = draft + addition;
  if (capture.load()) {
    if (candidate.size() <= kCaptureMax) {
      draft = std::move(candidate);
      capture_changed = true;
    }
    return;
  }
  auto text = utf8(candidate);
  if (integration::valid_chat_text(text))
    draft = std::move(candidate);
}
// Ctrl held without Alt: AltGr (Ctrl+Alt on layouts such as the Slovak one) types characters
// (AltGr+V is "@") and stays a character key.
bool shortcut_held() {
  return (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
}

// The clipboard's text, "" when it holds none.
std::wstring clipboard_text(HWND window) {
  std::wstring text;
  if (!OpenClipboard(window)) {
    return text;
  }
  if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
      text = chars;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  return text;
}

void set_clipboard_text(HWND window, const std::wstring& text) {
  if (!OpenClipboard(window)) {
    return;
  }
  EmptyClipboard();
  const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* chars = GlobalLock(data)) {
      std::memcpy(chars, text.c_str(), bytes);
      GlobalUnlock(data);
      if (!SetClipboardData(CF_UNICODETEXT, data)) {
        GlobalFree(data);
      }
    } else {
      GlobalFree(data);
    }
  }
  CloseClipboard();
}

// Ctrl+V: the clipboard's text at the end of the draft, as much of it as the line takes. A chat
// line has no line breaks (they and tabs become spaces); a book's capture keeps them.
void paste(HWND window) {
  constexpr wchar_t kTab = 9;
  constexpr wchar_t kLineFeed = 10;
  constexpr wchar_t kCarriageReturn = 13;
  std::wstring text = clipboard_text(window);
  for (wchar_t& c : text) {
    const bool line_break = c == kLineFeed || c == kCarriageReturn;
    if (c == kTab || (!capture.load() && line_break)) {
      c = L' ';
    }
  }
  for (const wchar_t c : text) {
    if (c == kCarriageReturn) {
      continue;
    }
    auto candidate = draft + c;
    if (capture.load()) {
      if (candidate.size() > kCaptureMax) {
        break;
      }
      draft = std::move(candidate);
      capture_changed = true;
      continue;
    }
    // A high surrogate waits for its low half; the line stops before what it cannot take.
    if (c >= 0xd800 && c <= 0xdbff) {
      draft = std::move(candidate);
      continue;
    }
    if (!integration::valid_chat_text(utf8(candidate))) {
      if (!draft.empty() && draft.back() >= 0xd800 && draft.back() <= 0xdbff) {
        draft.pop_back();
      }
      break;
    }
    draft = std::move(candidate);
  }
}

// Ctrl+C, Ctrl+V and Ctrl+X on the input line; true when the key was one of them.
bool clipboard_key(HWND window, WPARAM key) {
  if (!shortcut_held()) {
    return false;
  }
  if (key == 'V') {
    paste(window);
    return true;
  }
  if (key == 'C' || key == 'X') {
    set_clipboard_text(window, draft);
    if (key == 'X') {
      draft.clear();
      if (capture.load()) {
        capture_changed = true;
      }
    }
    return true;
  }
  return false;
}

void close_input() {
  open.store(false);
  pending_surrogate = 0;
  skip_open_character = false;
  dead_accent = 0;
}
// A game minimized while it starts (Alt+Tab from a fullscreen game) comes back in front with no
// keyboard focus on its thread: the game reads its keys as raw input and still plays, but no key
// message reaches the chat (T), the web views or the native screens. Window thread only.
void keep_keyboard_focus(HWND window) {
  if (GetFocus() || GetForegroundWindow() != window || IsIconic(window)) {
    return;
  }
  SetFocus(window);
  log("CHAT game window in front without the keyboard focus; focus given back");
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
  keep_keyboard_focus(window);
  if (message == WM_MOUSEWHEEL) {
    // Counted for the editors' scrolling (take_wheel); kept small when nobody reads it.
    const int turned = wheel_turned.load() + GET_WHEEL_DELTA_WPARAM(w);
    wheel_turned.store(turned > kMostWheel || turned < -kMostWheel ? 0 : turned);
  }
  // Alt+F4 and the close button: the game ignores the WM_CLOSE they would send.
  if (message == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE) {
    if (auto handler = close_handler.load()) {
      handler();
      return 0;
    }
  }
  if (auto sink = input_sink.load()) {
    std::intptr_t result = 0;
    if (sink(window, message, static_cast<std::uintptr_t>(w), static_cast<std::intptr_t>(l),
             result)) {
      return static_cast<LRESULT>(result);
    }
  }
  if (message == WM_KILLFOCUS || message == WM_ACTIVATEAPP && !w) {
    // Switching away (Alt+Tab) keeps the chat open with its draft; only a half-typed character
    // (a surrogate pair or a dead accent) is dropped.
    std::lock_guard<std::mutex> lock(state_mutex);
    pending_surrogate = 0;
    dead_accent = 0;
  }
  if (message == WM_KEYDOWN && w == 'T' && !ready.load() && !(l & (1LL << 30)) && log) {
    log("CHAT key refused: the player's world is not ready");
  }
  // The chat a resource hid opens nothing: the press is told to the resource's scripts
  // (chatKey) and left to the game.
  if (message == WM_KEYDOWN && w == 'T' && ready.load() && !overlay_visible.load() &&
      !engine_console_open.load() && !minigame_open.load() && !(l & (1LL << 30))) {
    hidden_chat_keys.fetch_add(1);
  }
  if (message == WM_KEYDOWN && w == 'T' && ready.load() && overlay_visible.load() && !open.load() &&
      !engine_console_open.load() && !minigame_open.load() && !(l & (1LL << 30))) {
    std::lock_guard<std::mutex> lock(state_mutex);
    draft.clear();
    open.store(true);
    skip_open_character = true;
    pending_surrogate = 0;
    return 0;
  }
  if (open.load()) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (message == WM_KEYDOWN && capture.load()) {
      if (clipboard_key(window, w)) {
        return 0;
      }
      if (w == VK_ESCAPE) {
        close_input();
        capture.store(false);
        capture_ended = true;
        return 0;
      }
      if (w == VK_RETURN) {
        if (draft.size() < kCaptureMax) {
          draft += L'\n';
          capture_changed = true;
        }
        return 0;
      }
      if (w == VK_BACK) {
        pending_surrogate = 0;
        if (!draft.empty()) {
          auto c = draft.back();
          draft.pop_back();
          if (c >= 0xdc00 && c <= 0xdfff && !draft.empty())
            draft.pop_back();
          capture_changed = true;
        }
        return 0;
      }
    }
    if (message == WM_KEYDOWN) {
      if (w == VK_ESCAPE) {
        draft.clear();
        close_input();
        return 0;
      }
      if (w == VK_RETURN) {
        auto text = utf8(draft);
        if (integration::valid_chat_text(text) && submissions.size() < 16)
          submissions.push_back(std::move(text));
        draft.clear();
        close_input();
        return 0;
      }
      if (w == VK_BACK) {
        pending_surrogate = 0;
        if (!draft.empty()) {
          auto c = draft.back();
          draft.pop_back();
          if (c >= 0xdc00 && c <= 0xdfff && !draft.empty())
            draft.pop_back();
        }
        return 0;
      }
      if (clipboard_key(window, w)) {
        return 0;
      }
      append_key(w, l);
      return 0;
    }
    if (message == WM_CHAR || message == WM_DEADCHAR || message == WM_SYSCHAR ||
        message == WM_SYSDEADCHAR)
      return 0;
    // Raw input still reaches the engine: the verified native OnAction gate handles it,
    // including releases, so held controls cannot remain latched on chat focus changes.
    if (message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP ||
        message == WM_SYSCHAR || message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
      return 0;
  }
  return CallWindowProcW(previous_proc, window, message, w, l);
}
void reset_buffers() {
  if (canvas)
    canvas->SetTarget(nullptr);
  buffers.clear();
  if (context)
    context->Flush();
}
bool initialize_graphics(IDXGISwapChain* chain) {
  if (initialized)
    return active_chain == chain;
  ComPtr<ID3D12Device> d12;
  if (FAILED(chain->GetDevice(IID_PPV_ARGS(&d12))))
    return false;
  // The queue the swap chain was made with; the first direct queue when it was made before the
  // hooks.
  const char* chosen_by = "the swap chain";
  ComPtr<ID3D12CommandQueue> candidate;
  for (auto it = chain_queues.rbegin(); it != chain_queues.rend(); ++it) {
    if (it->chain == chain) {
      candidate = it->queue;
      break;
    }
  }
  if (!candidate) {
    if (!first_queue)
      return false;
    candidate = first_queue;
    chosen_by = "the first direct queue";
  }
  ComPtr<ID3D12Device> candidate_device;
  if (FAILED(candidate->GetDevice(IID_PPV_ARGS(&candidate_device))) ||
      candidate_device.Get() != d12.Get())
    return false;
  queue = candidate;
  queue_device = candidate_device;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(chain->GetDesc(&desc)) || !desc.OutputWindow)
    return false;
  // Select only this process's game window, not auxiliary swap chains.
  DWORD pid{};
  GetWindowThreadProcessId(desc.OutputWindow, &pid);
  if (pid != GetCurrentProcessId())
    return false;
  IUnknown* q = queue.Get();
  D3D_FEATURE_LEVEL feature = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11On12CreateDevice(d12.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, &feature, 1, &q, 1,
                                   0, &device, &context, nullptr)))
    return false;
  if (FAILED(device.As(&on12)))
    return false;
  D2D1_FACTORY_OPTIONS options{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void**>(factory.GetAddressOf()))))
    return false;
  ComPtr<IDXGIDevice> dxgi;
  if (FAILED(device.As(&dxgi)) || FAILED(factory->CreateDevice(dxgi.Get(), &d2device)) ||
      FAILED(d2device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &canvas)))
    return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(text_factory.GetAddressOf()))))
    return false;
  if (FAILED(text_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                            17, L"sk-SK", &font)))
    return false;
  font->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
  if (FAILED(canvas->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)))
    return false;
  SetLastError(0);
  auto old =
      SetWindowLongPtrW(desc.OutputWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(window_proc));
  if (!old)
    return false;
  previous_proc = reinterpret_cast<WNDPROC>(old);
  game_window = desc.OutputWindow;
  active_chain = chain;
  initialized = true;
  log("CHAT renderer ready window=%p queue=%p (from %s)", game_window, queue.Get(), chosen_by);
  // The textures of web views drawn into the world (render/world_textures.hpp).
  kcds::runtime::render::world_textures::watch(d12.Get(), log);
  // The KCDS icon on the game's window while the client runs (no game file changes).
  kcds::runtime::branding::window_icon::apply(game_window, log);
  return true;
}
bool prepare_buffers(IDXGISwapChain3* chain) {
  if (!buffers.empty())
    return true;
  DXGI_SWAP_CHAIN_DESC desc{};
  if (FAILED(chain->GetDesc(&desc)))
    return false;
  std::vector<Buffer> next(desc.BufferCount);
  for (UINT i = 0; i < desc.BufferCount; ++i) {
    ComPtr<ID3D12Resource> back;
    if (FAILED(chain->GetBuffer(i, IID_PPV_ARGS(&back))))
      return false;
    D3D11_RESOURCE_FLAGS flags{};
    flags.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(on12->CreateWrappedResource(back.Get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_PRESENT,
                                           IID_PPV_ARGS(&next[i].wrapped))))
      return false;
    next[i].back = back;
    ComPtr<IDXGISurface> surface;
    if (FAILED(next[i].wrapped.As(&surface)))
      return false;
    // UNORM surface format is inherited from the real back buffer, including BGRA/RGBA.
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(canvas->CreateBitmapFromDxgiSurface(surface.Get(), &props, &next[i].bitmap)))
      return false;
  }
  buffers = std::move(next);
  return true;
}
void report_frame_size(IDXGISwapChain* chain) {
  auto observer = frame_size_observer.load();
  DXGI_SWAP_CHAIN_DESC description;
  if (observer && SUCCEEDED(chain->GetDesc(&description))) {
    observer(static_cast<int>(description.BufferDesc.Width),
             static_cast<int>(description.BufferDesc.Height));
  }
}
void draw(IDXGISwapChain* chain) {
  report_frame_size(chain);
  std::deque<Line> history;
  std::wstring input;
  const auto typing = open.load() && !capture.load();
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    history = lines;
    input = draft;
  }
  const auto flash = damage_flash.load();
  const auto flash_age = flash ? GetTickCount64() - flash : kDamageFlashMs;
  const bool flashing = flash_age < kDamageFlashMs;
  const auto views_visible = view_layer_active.load();
  const bool views = views_visible && views_visible() && view_layer_paint.load();
  // Every frame is drawn into: the KCDS watermark is always shown.
  if (!initialize_graphics(chain))
    return;
  ComPtr<IDXGISwapChain3> sc;
  if (FAILED(chain->QueryInterface(IID_PPV_ARGS(&sc))) || !prepare_buffers(sc.Get()))
    return;
  const auto index = sc->GetCurrentBackBufferIndex();
  if (index >= buffers.size())
    return;
  auto& b = buffers[index];
  // A render of another camera takes its pixels or shows the kept frame here, before
  // KCDS's own overlays (its D3D12 copies go on the queue before the overlay's work).
  kcds::runtime::render::on_present(queue_device.Get(), queue.Get(), b.back.Get());
  ID3D11Resource* resources[]{b.wrapped.Get()};
  on12->AcquireWrappedResources(resources, 1);
  canvas->SetTarget(b.bitmap.Get());
  canvas->BeginDraw();
  const auto size = canvas->GetSize();
  if (views)
    view_layer_paint.load()(canvas.Get(), size.width, size.height, game_window);
  if (flashing) {
    // Red edges fading towards a clear centre, fading out over kDamageFlashMs.
    const float strength = 0.75f * (1.f - float(flash_age) / float(kDamageFlashMs));
    const D2D1_GRADIENT_STOP stops[] = {
        {0.f, D2D1::ColorF(0.45f, 0.f, 0.f, 0.f)},
        {0.55f, D2D1::ColorF(0.45f, 0.f, 0.f, 0.f)},
        {1.f, D2D1::ColorF(0.45f, 0.f, 0.f, strength)},
    };
    ComPtr<ID2D1GradientStopCollection> collection;
    ComPtr<ID2D1RadialGradientBrush> vignette;
    const auto centre = D2D1::Point2F(size.width / 2, size.height / 2);
    if (SUCCEEDED(canvas->CreateGradientStopCollection(stops, 3, &collection)) &&
        SUCCEEDED(canvas->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(0, 0), size.width * 0.75f,
                                                size.height * 0.75f),
            collection.Get(), &vignette))) {
      canvas->FillRectangle(D2D1::RectF(0, 0, size.width, size.height), vignette.Get());
    }
  }
  const float width = std::min(570.f, std::max(180.f, size.width - 40));
  float y = 26;
  const auto now = GetTickCount64();
  const auto first = history.size() > 8 ? history.size() - 8 : 0;
  const auto shown = overlay_visible.load() ? history.size() : first;
  for (std::size_t i = first; i < shown; ++i) {
    const auto& line = history[i];
    if (!typing && now - line.time > 18000)
      continue;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(text_factory->CreateTextLayout(line.text.data(), UINT32(line.text.size()),
                                              font.Get(), width - 20, 100, &layout)))
      continue;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const auto height = std::min(100.f, metrics.height) + 8;
    brush->SetColor(D2D1::ColorF(0, 0, 0, 0.62f));
    canvas->FillRectangle(D2D1::RectF(18, y, 18 + width, y + height), brush.Get());
    auto color =
        line.color
            ? D2D1::ColorF(float(line.color >> 24) / 255, float((line.color >> 16) & 255) / 255,
                           float((line.color >> 8) & 255) / 255, float(line.color & 255) / 255)
            : D2D1::ColorF(0.94f, 0.94f, 0.9f, 1);
    brush->SetColor(color);
    canvas->DrawTextLayout(D2D1::Point2F(28, y + 4), layout.Get(), brush.Get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    y += height + 2;
  }
  if (typing) {
    brush->SetColor(D2D1::ColorF(0.05f, 0.06f, 0.07f, 0.94f));
    canvas->FillRectangle(D2D1::RectF(18, y + 6, 18 + width, y + 70), brush.Get());
    brush->SetColor(D2D1::ColorF(0.93f, 0.83f, 0.57f, 1));
    auto text = L"> " + input + L"|";
    canvas->DrawText(text.data(), UINT32(text.size()), font.Get(),
                     D2D1::RectF(28, y + 12, 8 + width, y + 48), brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    // Escaped: the compiler reads this file in the ANSI code page.
    const wchar_t* hint = L"Enter \u2014 odosla\u0165    Esc \u2014 zavrie\u0165";
    canvas->DrawText(hint, UINT32(wcslen(hint)), font.Get(),
                     D2D1::RectF(28, y + 47, 8 + width, y + 70), brush.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  // The players' nameplates, then last the watermark, over the resources' web views and
  // whether the UI is shown or not.
  kcds::runtime::nameplates::paint(canvas.Get(), size.width, size.height);
  // World Architect's text and pictures (architect/editor_overlay.hpp), under the watermark.
  kcds::runtime::architect::overlay::paint(canvas.Get(), size.width, size.height);
  kcds::runtime::branding::watermark::paint(canvas.Get(), size.width, size.height);
  // The voice indicator right of the watermark.
  kcds::runtime::voice::indicator::paint(canvas.Get(), size.width, size.height);
  auto hr = canvas->EndDraw();
  canvas->SetTarget(nullptr);
  on12->ReleaseWrappedResources(resources, 1);
  context->Flush();
  if (FAILED(hr)) {
    log("CHAT draw failure=0x%lx", hr);
    reset_buffers();
  }
}
void draw_present_locked(IDXGISwapChain* chain) {
  graphics_mutex.lock();
  __try {
    draw(chain);
  } __finally {
    // /EHsc does not guarantee lock_guard destruction on a native SEH fault.
    graphics_mutex.unlock();
  }
}
void draw_present_overlay(IDXGISwapChain* chain) {
  try {
    draw_present_locked(chain);
  } catch (...) {
    log("CHAT draw exception; skipped frame");
  }
}
HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* chain, UINT sync, UINT flags) {
  namespace metrics = kcds::runtime::characters::pipeline_metrics;
  auto timing = metrics::begin_present((flags & DXGI_PRESENT_TEST) != 0);
  const bool draw_overlay = !(flags & DXGI_PRESENT_TEST) && !drawing;
  const bool previous_drawing = drawing;
  bool completed = false;
  HRESULT result = S_OK;
  __try {
    if (draw_overlay) {
      drawing = true;
      __try {
        draw_present_overlay(chain);
      } __finally {
        drawing = previous_drawing;
      }
    }
    metrics::overlay_finished(timing, draw_overlay);
    result = original_present(chain, sync, flags);
    metrics::native_present_finished(timing);
    completed = true;
  } __finally {
    metrics::leave_present(timing, completed);
  }
  metrics::publish_present(timing);
  return result;
}
HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* chain, UINT count, UINT width, UINT height,
                                      DXGI_FORMAT format, UINT flags) {
  const bool was_drawing = drawing;
  drawing = true;
  std::lock_guard<std::mutex> lock(graphics_mutex);
  if (chain == active_chain)
    reset_buffers();
  auto result = original_resize(chain, count, width, height, format, flags);
  drawing = was_drawing;
  return result;
}
void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue* q, UINT count,
                                    ID3D12CommandList* const* lists) {
  if (!drawing && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    if (!initialized && !first_queue) {
      std::lock_guard<std::mutex> lock(graphics_mutex);
      if (!first_queue) {
        first_queue = q;
        log("CHAT first direct queue=%p", q);
      }
    }
  }
  original_execute(q, count, lists);
}
// Keeps the queue a new swap chain was made with (a D3D12 swap chain is made on its queue).
void remember_chain_queue(IUnknown* maker, IUnknown* chain) {
  ComPtr<ID3D12CommandQueue> q;
  ComPtr<IDXGISwapChain> made;
  if (!maker || !chain || FAILED(maker->QueryInterface(IID_PPV_ARGS(&q))) ||
      FAILED(chain->QueryInterface(IID_PPV_ARGS(&made))))
    return;
  std::lock_guard<std::mutex> lock(graphics_mutex);
  if (chain_queues.size() >= 16)
    chain_queues.erase(chain_queues.begin());
  chain_queues.push_back({made.Get(), q});
  log("CHAT swap chain=%p made on queue=%p", made.Get(), q.Get());
}
HRESULT STDMETHODCALLTYPE create_chain_hook(IDXGIFactory* maker, IUnknown* device,
                                            DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** chain) {
  HRESULT result = original_create_chain(maker, device, desc, chain);
  if (SUCCEEDED(result) && chain)
    remember_chain_queue(device, *chain);
  return result;
}
HRESULT STDMETHODCALLTYPE create_chain_for_hwnd_hook(
    IDXGIFactory2* maker, IUnknown* device, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output,
    IDXGISwapChain1** chain) {
  HRESULT result =
      original_create_chain_for_hwnd(maker, device, window, desc, fullscreen, output, chain);
  if (SUCCEEDED(result) && chain)
    remember_chain_queue(device, *chain);
  return result;
}
}  // namespace
bool install_hooks(Logger logger);
// Runs once: the server browser installs the frame hooks as soon as it starts, so its page is
// painted with the first main menu; the adapter's later call returns that result.
bool install(Logger logger) {
  static std::mutex once;
  static int result = -1;
  std::lock_guard<std::mutex> lock(once);
  if (result < 0) {
    result = install_hooks(logger) ? 1 : 0;
  }
  return result == 1;
}
bool install_hooks(Logger logger) {
  log = logger;
  WNDCLASSW wc{};
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpfnWndProc = DefWindowProcW;
  wc.lpszClassName = L"KCD2S.Chat.Probe";
  RegisterClassW(&wc);
  auto window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 16, 16, nullptr,
                                nullptr, wc.hInstance, nullptr);
  if (!window)
    return false;
  ComPtr<ID3D12Device> d12;
  ComPtr<ID3D12CommandQueue> q;
  ComPtr<IDXGIFactory4> dxgi;
  ComPtr<IDXGISwapChain1> chain;
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 16;
  desc.Height = 16;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  // The probe only reads the swap chain's and the queue's function tables, which D3D12 and DXGI
  // share between adapters: it is made on Windows' software adapter, so it puts no work on the
  // graphics card while the game is making its own device there.
  ComPtr<IDXGIAdapter> software;
  bool okay =
      SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi))) &&
      SUCCEEDED(dxgi->EnumWarpAdapter(IID_PPV_ARGS(&software))) &&
      SUCCEEDED(D3D12CreateDevice(software.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12))) &&
      SUCCEEDED(d12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&q))) &&
      SUCCEEDED(dxgi->CreateSwapChainForHwnd(q.Get(), window, &desc, nullptr, nullptr, &chain));
  void* p{};
  void* r{};
  void* e{};
  void* c{};
  void* ch{};
  if (okay) {
    auto v = *reinterpret_cast<void***>(chain.Get());
    p = v[8];
    r = v[13];
    e = (*reinterpret_cast<void***>(q.Get()))[10];
    // The factory's CreateSwapChain and CreateSwapChainForHwnd.
    auto f = *reinterpret_cast<void***>(dxgi.Get());
    c = f[10];
    ch = f[15];
  }
  chain.Reset();
  DestroyWindow(window);
  if (!okay)
    return false;
  // Only our own DXGI/D3D hooks; all WHGame signatures remain verified separately.
  void* targets[]{p, r, e, c, ch};
  void* hooks[]{reinterpret_cast<void*>(present_hook), reinterpret_cast<void*>(resize_hook),
                reinterpret_cast<void*>(execute_hook), reinterpret_cast<void*>(create_chain_hook),
                reinterpret_cast<void*>(create_chain_for_hwnd_hook)};
  void** originals[]{
      reinterpret_cast<void**>(&original_present), reinterpret_cast<void**>(&original_resize),
      reinterpret_cast<void**>(&original_execute), reinterpret_cast<void**>(&original_create_chain),
      reinterpret_cast<void**>(&original_create_chain_for_hwnd)};
  constexpr int kHooks = 5;
  for (int i = 0; i < kHooks; ++i) {
    if (MH_CreateHook(targets[i], hooks[i], originals[i]) != MH_OK) {
      for (int j = 0; j < i; ++j)
        MH_RemoveHook(targets[j]);
      return false;
    }
  }
  for (int i = 0; i < kHooks; ++i)
    if (MH_EnableHook(targets[i]) != MH_OK) {
      for (auto target : targets) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
      }
      return false;
    }
  log("CHAT frame hooks installed");
  return true;
}
bool editing() {
  return open.load();
}
void set_view_layer(FrameLayer layer) {
  view_layer_paint.store(layer.paint);
  view_layer_active.store(layer.active);
}
void set_frame_size_observer(void (*observer)(int width, int height)) {
  frame_size_observer.store(observer);
}
void set_close_handler(CloseHandler handler) {
  close_handler.store(handler);
}

void set_input_sink(InputSink sink) {
  input_sink.store(sink);
}
void* window() {
  return game_window;
}
int take_wheel() {
  return wheel_turned.exchange(0);
}
void flash_damage() {
  damage_flash.store(GetTickCount64());
}
void set_engine_console_open(bool value) {
  engine_console_open.store(value);
}
void set_minigame_open(bool value) {
  minigame_open.store(value);
}
void set_ready(bool value) {
  ready.store(value);
  if (!value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    close_input();
    draft.clear();
    submissions.clear();
    lines.clear();
  }
}
void receive(integration::ChatMessage message) {
  auto text = wide(message.author.empty() ? message.text : message.author + ": " + message.text);
  if (text.empty())
    return;
  std::lock_guard<std::mutex> lock(state_mutex);
  lines.push_back({std::move(text), message.color, GetTickCount64()});
  while (lines.size() > 128)
    lines.pop_front();
}
void set_ui_visible(bool visible) {
  overlay_visible.store(visible);
  if (!visible) {
    std::lock_guard<std::mutex> lock(state_mutex);
    draft.clear();
    close_input();
  }
}
bool ui_visible() {
  return overlay_visible.load();
}
unsigned take_hidden_chat_keys() {
  return hidden_chat_keys.exchange(0);
}
bool open_input() {
  if (!ready.load() || !overlay_visible.load() || engine_console_open.load()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  if (!open.load()) {
    draft.clear();
    pending_surrogate = 0;
    open.store(true);
  }
  return true;
}
void close_input_line() {
  std::lock_guard<std::mutex> lock(state_mutex);
  draft.clear();
  close_input();
}
bool begin_capture(const std::string& initial) {
  if (!ready.load() || engine_console_open.load()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  draft = wide(initial);
  if (draft.size() > kCaptureMax) {
    draft.resize(kCaptureMax);
  }
  pending_surrogate = 0;
  dead_accent = 0;
  skip_open_character = false;
  capture_changed = false;
  capture_ended = false;
  capture.store(true);
  open.store(true);
  return true;
}
void end_capture() {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (capture.load()) {
    close_input();
    draft.clear();
    capture.store(false);
  }
  capture_changed = false;
  capture_ended = false;
}
CaptureUpdate take_capture() {
  std::lock_guard<std::mutex> lock(state_mutex);
  CaptureUpdate update;
  update.changed = capture_changed;
  update.ended = capture_ended;
  if (capture_changed) {
    update.text = utf8(draft);
  }
  capture_changed = false;
  capture_ended = false;
  return update;
}
std::vector<std::string> take_submissions() {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto out = std::move(submissions);
  submissions.clear();
  return out;
}
}  // namespace kcd2s::native_chat
