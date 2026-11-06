// In-game bug reports (the ESC menu's "Report a bug"). The player writes what happened and agrees
// to send their logs; the report goes to https://kcdsynced.com/api/bug-report, where the KCDS
// Discord bot opens a ticket only the team sees, with the text and the logs.
//
// The report is signed: HMAC-SHA256 over "<t>.<body>" under the bug report key, which
// CMakeLists.txt derives from the build secret (KCDS_BUG_REPORT_KEY; the secret itself is not in
// the binary). The query carries t (Unix seconds) and s (the hex signature); the bot refuses
// anything else, an old or a reused signature. A build without the secret signs with a key the bot
// does not take.
//
// The logs are the end of each file (at most kMaxLogBytes): this client's own log, the launcher's
// and the game's kcd.log. The report also names the player's ids, as the global bans do (Steam,
// Discord, hardware id; Epic, GOG and Xbox once the client has them), so the team can block a
// player who spams reports on any of them (the bot drops a blocked player's reports). Sending runs
// on a worker thread. For now the dialog shows the outcome (sent, or the error); the client log
// has it too.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace kcds::runtime::bug_report {

inline constexpr std::size_t kMaxTextBytes = 3000;
inline constexpr std::size_t kMaxLogBytes = 2 * 1024 * 1024;

struct Report {
  std::string text;
  std::string nickname;
  std::string version;
  std::string server;
  std::string map;
  std::string steam_id;
  std::string epic_id;
  std::string discord_id;
  std::string hardware_id;
  // The logs to attach: { name shown in the ticket, full path }.
  std::vector<std::pair<std::string, std::wstring>> logs;
};

using Logger = void (*)(const char* format, ...);

// The JSON body of a report (the logs' text included), and its signature's query for time `t`.
std::string body_of(const Report& report);
std::string signed_query(const std::string& body, long long t);

// What became of a report: the HTTP status (0 when no answer came) and, when it was not taken,
// why (the bot's answer, or the step that failed).
struct Outcome {
  int status = 0;
  std::string error;
};

// The menu page's message for an outcome:
// {"type":"bugReportResult","ok":..,"status":..,"error":..}.
std::string outcome_message(const Outcome& outcome);

// Starts sending in the background; false while another report is still being sent. `done` (may be
// empty) gets the outcome on the worker thread.
bool send(Report report, Logger log, std::function<void(const Outcome&)> done = {});

}  // namespace kcds::runtime::bug_report
