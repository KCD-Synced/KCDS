// sentry-native with the Crashpad backend behind the crash::Backend interface.
// See kcds/launcher/sentry_backend.hpp for the privacy and threading rules.
#include "kcds/launcher/sentry_backend.hpp"

#include <windows.h>

#include <shlobj.h>

#include <sentry.h>

#include <string>
#include <system_error>
#include <utility>

namespace kcds::launcher::crash {

namespace {

std::filesystem::path utf8_path(const std::string& text) {
  if (text.empty()) {
    return std::filesystem::path();
  }
  int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                   static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return std::filesystem::path();
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      wide.data(), length);
  return std::filesystem::path(wide);
}

// Everything the SDK could collect beyond the crash itself is switched off.
// Default PII stays off: the pinned release keeps it disabled and exposes the
// switch only on a console platform, so there is nothing to set on Windows.
void apply_privacy_settings(sentry_options_t* options) {
  sentry_options_set_attach_screenshot(options, 0);
  sentry_options_set_auto_session_tracking(options, 0);
  sentry_options_set_traces_sample_rate(options, 0.0);
  sentry_options_set_enable_logs(options, 0);
  sentry_options_set_enable_metrics(options, 0);
  sentry_options_set_max_breadcrumbs(options, 0);
  sentry_options_set_send_client_reports(options, 0);
  sentry_options_set_symbolize_stacktraces(options, 0);
}

// Registers each log snapshot as a report attachment. A stale snapshot from an
// earlier run is removed first, so a report only ever carries the snapshot its
// own crash wrote. Returns false when a snapshot folder cannot be created.
bool register_log_attachments(sentry_options_t* options, const std::vector<LogAttachment>& logs) {
  for (const LogAttachment& log : logs) {
    std::error_code error;
    std::filesystem::create_directories(log.snapshot.parent_path(), error);
    if (error) {
      return false;
    }
    std::filesystem::remove(log.snapshot, error);
    sentry_options_add_attachmentw(options, log.snapshot.c_str());
  }
  return true;
}

// The SDK's on_crash hook, run in the crashing process before the Crashpad
// handler writes the report: copy the tail of every log into its snapshot.
// The event is returned unchanged, so the crash is always reported.
sentry_value_t snapshot_logs_on_crash(const sentry_ucontext_t* context, sentry_value_t event,
                                      void* closure) {
  const auto* options = static_cast<const SentryOptions*>(closure);
  for (const LogAttachment& log : options->logs) {
    copy_log_tail(log.source.c_str(), log.snapshot.c_str(), log.max_bytes);
  }
  // What the process says about the moment: tags to search by, the stack as an extra.
  if (options->annotate) {
    std::vector<std::pair<std::string, std::string>> tags;
    std::string stack;
    options->annotate(context ? &context->exception_ptrs : nullptr, tags, stack);
    sentry_value_t event_tags = sentry_value_get_by_key(event, "tags");
    if (sentry_value_is_null(event_tags)) {
      event_tags = sentry_value_new_object();
      sentry_value_set_by_key(event, "tags", event_tags);
    }
    for (const auto& [name, value] : tags) {
      sentry_value_set_by_key(event_tags, name.c_str(), sentry_value_new_string(value.c_str()));
    }
    if (!stack.empty()) {
      sentry_value_t extra = sentry_value_get_by_key(event, "extra");
      if (sentry_value_is_null(extra)) {
        extra = sentry_value_new_object();
        sentry_value_set_by_key(event, "extra", extra);
      }
      sentry_value_set_by_key(extra, "kcds.stack", sentry_value_new_string(stack.c_str()));
    }
  }
  return event;
}

}  // namespace

SentryBackend::SentryBackend(SentryOptions options) : options_(std::move(options)) {
}

SentryBackend::~SentryBackend() {
  stop();
}

bool SentryBackend::start(const Dsn& /*dsn*/, const Settings& settings, const RuntimeInfo& info) {
  if (started_) {
    return false;
  }
  std::filesystem::path database = utf8_path(info.database_dir);
  if (database.empty() || info.release.empty() || options_.handler.empty()) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(options_.handler, error)) {
    return false;
  }
  std::filesystem::create_directories(database, error);
  if (error) {
    return false;
  }

  // settings.dsn was validated by crash::start() before this call.
  sentry_options_t* options = sentry_options_new();
  if (options == nullptr) {
    return false;
  }
  sentry_options_set_dsn(options, settings.dsn.c_str());
  sentry_options_set_release(options, info.release.c_str());
  sentry_options_set_environment(options, settings.environment.c_str());
  sentry_options_set_database_pathw(options, database.c_str());
  sentry_options_set_handler_pathw(options, options_.handler.c_str());
  sentry_options_set_crashpad_wait_for_upload(options, options_.wait_for_upload ? 1 : 0);
  apply_privacy_settings(options);
  if (!options_.logs.empty() && !register_log_attachments(options, options_.logs)) {
    sentry_options_free(options);
    return false;
  }
  if (!options_.logs.empty() || options_.annotate) {
    // options_ lives as long as this backend, which outlives the SDK.
    sentry_options_set_on_crash(options, snapshot_logs_on_crash, &options_);
  }

  // sentry_init takes ownership of options, also on failure.
  if (sentry_init(options) != 0) {
    return false;
  }
  started_ = true;
  return true;
}

void SentryBackend::stop() {
  if (!started_) {
    return;
  }
  sentry_close();
  started_ = false;
}

std::filesystem::path default_database_dir(const std::string& component) {
  if (!is_tag(component)) {
    return std::filesystem::path();
  }
  PWSTR local_app_data = nullptr;
  HRESULT result =
      SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
  if (FAILED(result) || local_app_data == nullptr) {
    CoTaskMemFree(local_app_data);
    return std::filesystem::path();
  }
  std::filesystem::path directory = std::filesystem::path(local_app_data) / L"KCDS" / L"sentry";
  CoTaskMemFree(local_app_data);

  directory /= utf8_path(component);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return std::filesystem::path();
  }
  return directory;
}

}  // namespace kcds::launcher::crash
