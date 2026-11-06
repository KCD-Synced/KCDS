#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <objbase.h>
#include <wrl.h>
#include <wrl/event.h>
#include <shlwapi.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <io.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <thread>
#include "WebView2.h"
#include "kcd2s/sdk/marshal.hpp"
#include "kcd2s/sdk/nui.hpp"
#include "resource_runtime_html.hpp"
#include "runtime_files.hpp"
#include "runtime_profiles.hpp"

using namespace Microsoft::WRL;
using kcd2s::sdk::Value;

namespace {
constexpr UINT kInputMessage = WM_APP + 1;
constexpr const wchar_t* kRuntimeUrl = L"https://runtime.kcds.invalid/";
ComPtr<ICoreWebView2Environment> environment;
ComPtr<ICoreWebView2Controller> controller;
ComPtr<ICoreWebView2> webview;
std::mutex output_mutex;
std::atomic<bool> running{true};
HWND window;

std::wstring wide(const std::string& value) {
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       int(value.size()), nullptr, 0);
  if (!size && !value.empty())
    throw std::runtime_error("Invalid UTF-8 runtime message");
  std::wstring result(std::size_t(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(),
                      size);
  return result;
}

std::string utf8(const wchar_t* value) {
  if (!value)
    return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (!size)
    throw std::runtime_error("Invalid UTF-16 runtime message");
  std::string result(std::size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

void send(const Value& value) {
  const auto bytes = kcd2s::sdk::encode(value);
  if (bytes.size() > 65536)
    return;
  static constexpr char hex[] = "0123456789abcdef";
  std::string line;
  line.reserve(bytes.size() * 2 + 1);
  for (const auto byte : bytes) {
    line += hex[byte >> 4];
    line += hex[byte & 15];
  }
  std::lock_guard lock(output_mutex);
  std::cout << line << '\n' << std::flush;
}

void fail(const char* message) {
  send(Value(Value::Map{{"op", Value("runtimeFailed")}, {"text", Value(message)}}));
  PostMessageW(window, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kInputMessage) {
    std::unique_ptr<std::wstring> json(reinterpret_cast<std::wstring*>(lparam));
    if (webview)
      webview->PostWebMessageAsJson(json->c_str());
    return 0;
  }
  if (message == WM_DESTROY) {
    running = false;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(handle, message, wparam, lparam);
}

void read_input() {
  auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    return -1;
  };
  std::string line;
  char buffer[4096];
  DWORD count;
  bool overflow = false;
  while (running &&
         ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &count, nullptr) &&
         count) {
    for (DWORD i = 0; i < count; ++i) {
      if (buffer[i] != '\n') {
        if (line.size() < 131072 && !overflow)
          line += buffer[i];
        else
          overflow = true;
        continue;
      }
      if (!overflow && line.size() % 2 == 0) {
        try {
          std::vector<std::uint8_t> bytes;
          bool valid = true;
          for (std::size_t index = 0; index < line.size(); index += 2) {
            const auto a = nibble(line[index]), b = nibble(line[index + 1]);
            if (a < 0 || b < 0) {
              valid = false;
              break;
            }
            bytes.push_back(std::uint8_t((a << 4) | b));
          }
          auto decoded = valid ? kcd2s::sdk::decode(bytes) : kcd2s::sdk::DecodeResult{};
          if (decoded.ok && decoded.consumed == bytes.size()) {
            auto json =
                std::make_unique<std::wstring>(wide(kcd2s::sdk::value_to_json(decoded.value)));
            if (PostMessageW(window, kInputMessage, 0, reinterpret_cast<LPARAM>(json.get())))
              json.release();
          }
        } catch (const std::exception&) {
          // Invalid parent IPC frames are discarded; never evaluate strings as scripts.
        }
      }
      line.clear();
      overflow = false;
    }
  }
  PostMessageW(window, WM_CLOSE, 0, 0);
}

// Why the process ended, as far as WebView2 knows: its reason, exit code and the
// module it blames, for the log. Empty on a runtime without these fields.
std::string failure_detail(ICoreWebView2ProcessFailedEventArgs* args) {
  Microsoft::WRL::ComPtr<ICoreWebView2ProcessFailedEventArgs2> details;
  if (FAILED(args->QueryInterface(IID_PPV_ARGS(&details))) || !details) {
    return {};
  }
  COREWEBVIEW2_PROCESS_FAILED_REASON reason{};
  int exit_code = 0;
  details->get_Reason(&reason);
  details->get_ExitCode(&exit_code);
  std::string text =
      " (reason " + std::to_string(int(reason)) + ", exit code " + std::to_string(exit_code);
  return text + ")";
}

// WebView2's browser or page process ended or hangs: the scripts are gone while this
// process would live on, so it ends too and the game starts a new runtime.
HRESULT process_failed(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) {
  COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
  args->get_ProcessFailedKind(&kind);
  const std::string detail = failure_detail(args);
  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
    fail(("WebView2 browser process exited" + detail).c_str());
  } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
    fail(("WebView2 page process exited" + detail).c_str());
  } else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
    fail(("WebView2 page process does not respond" + detail).c_str());
  }
  return S_OK;
}

// The folder of this program's .NET runtime files (runtime_files.hpp).
std::filesystem::path dotnet_folder() {
  std::wstring path(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
  path.resize(length);
  return std::filesystem::path(path).parent_path() / L"dotnet";
}

// Answers a request for one of the runtime's own files; false for any other URL.
bool serve_runtime_file(const std::wstring& url, ICoreWebView2WebResourceRequestedEventArgs* args) {
  static const std::filesystem::path folder = dotnet_folder();
  const auto file = kcds::resources::files::find(url, kRuntimeUrl, folder);
  if (!file) {
    return false;
  }
  ComPtr<IStream> stream;
  const HRESULT opened =
      SHCreateStreamOnFileEx(file->path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                             FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
  if (FAILED(opened)) {
    return false;
  }
  const std::wstring headers = L"Content-Type: " + file->content_type +
                               L"\r\n"
                               L"Cross-Origin-Resource-Policy: same-origin\r\n";
  ComPtr<ICoreWebView2WebResourceResponse> response;
  environment->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &response);
  args->put_Response(response.Get());
  return true;
}

void setup_browser() {
  EventRegistrationToken token;
  ComPtr<ICoreWebView2Settings> settings;
  webview->get_Settings(&settings);
  settings->put_AreHostObjectsAllowed(FALSE);
  settings->put_AreDefaultScriptDialogsEnabled(FALSE);
  settings->put_AreDevToolsEnabled(FALSE);
  settings->put_AreDefaultContextMenusEnabled(FALSE);
  webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR uri = nullptr;
            args->get_Uri(&uri);
            args->put_Cancel(!uri || std::wstring(uri) != kRuntimeUrl);
            CoTaskMemFree(uri);
            return S_OK;
          })
          .Get(),
      &token);
  webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(process_failed).Get(),
                             &token);
  webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            return S_OK;
          })
          .Get(),
      &token);
  webview->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
          })
          .Get(),
      &token);
  webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  webview->add_WebResourceRequested(
      Callback<ICoreWebView2WebResourceRequestedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> request;
            args->get_Request(&request);
            LPWSTR uri = nullptr;
            request->get_Uri(&uri);
            const std::wstring url = uri ? uri : L"";
            CoTaskMemFree(uri);
            // The .NET runtime of C# client resources, from the folder next to this program.
            if (serve_runtime_file(url, args)) {
              return S_OK;
            }
            const bool allowed = url == kRuntimeUrl;
            ComPtr<IStream> stream;
            CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            if (allowed) {
              ULONG written = 0;
              stream->Write(kResourceRuntimeHtml, ULONG(sizeof(kResourceRuntimeHtml) - 1),
                            &written);
              LARGE_INTEGER zero{};
              stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            }
            ComPtr<ICoreWebView2WebResourceResponse> response;
            environment->CreateWebResourceResponse(
                stream.Get(), allowed ? 200 : 403, allowed ? L"OK" : L"Blocked",
                // Cross-origin isolated, so the page can share a buffer with the resource worker
                // for synchronous requests (Client/resources/sync_channel.cjs).
                L"Content-Type: text/html; charset=utf-8\r\n"
                L"Cross-Origin-Opener-Policy: same-origin\r\n"
                L"Cross-Origin-Embedder-Policy: require-corp\r\n",
                &response);
            args->put_Response(response.Get());
            return S_OK;
          })
          .Get(),
      &token);
  webview->add_WebMessageReceived(
      Callback<ICoreWebView2WebMessageReceivedEventHandler>(
          [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR source = nullptr, json = nullptr;
            args->get_Source(&source);
            if (source && std::wstring(source) == kRuntimeUrl &&
                SUCCEEDED(args->get_WebMessageAsJson(&json))) {
              try {
                const auto text = utf8(json);
                if (text.size() <= 65536) {
                  const auto value = kcd2s::sdk::json_to_value(text);
                  if (value.ok)
                    send(value.value);
                }
              } catch (const std::exception&) {
              }
            }
            CoTaskMemFree(source);
            CoTaskMemFree(json);
            return S_OK;
          })
          .Get(),
      &token);
  webview->Navigate(kRuntimeUrl);
}
}  // namespace

int main() {
  _setmode(_fileno(stdout), _O_BINARY);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = procedure;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.lpszClassName = L"KCDSResourceRuntime";
  RegisterClassW(&window_class);
  window = CreateWindowExW(0, window_class.lpszClassName, L"KCDS resource runtime", WS_OVERLAPPED,
                           0, 0, 1, 1, nullptr, nullptr, window_class.hInstance, nullptr);
  if (!window)
    return 1;
  wchar_t root[MAX_PATH];
  GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH);
  // This runtime's own WebView2 profile, removed when it ends.
  const auto profile = kcds::resources::profiles::open(root);
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, profile.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT status, ICoreWebView2Environment* value) -> HRESULT {
            if (FAILED(status) || !value) {
              fail("WebView2 resource environment unavailable");
              return S_OK;
            }
            environment = value;
            value->CreateCoreWebView2Controller(
                window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [](HRESULT result, ICoreWebView2Controller* value) -> HRESULT {
                              if (FAILED(result) || !value) {
                                fail("WebView2 resource controller unavailable");
                                return S_OK;
                              }
                              controller = value;
                              controller->put_IsVisible(FALSE);
                              controller->get_CoreWebView2(&webview);
                              setup_browser();
                              return S_OK;
                            })
                            .Get());
            return S_OK;
          })
          .Get());
  std::thread input(read_input);
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  running = false;
  CancelSynchronousIo(input.native_handle());
  input.join();
  if (controller)
    controller->Close();
  webview.Reset();
  controller.Reset();
  environment.Reset();
  CoUninitialize();
  // While WebView2's browser process still lets go of it, the folder stays for the next runtime's
  // sweep.
  kcds::resources::profiles::close(profile);
  return 0;
}
