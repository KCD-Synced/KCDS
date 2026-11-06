#pragma once
// Command-line settings of kcd2s-load-probe. Portable so the parser is covered by Linux CTest.
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2s::local::load {

enum class Workload { Idle, Distributed, Dense };

// full    - the field set the game client sends every input (22 owner fields, including the
//           1220-byte animation state and the 22-byte rider state),
// compact - position, rotation, velocity and the convergence marker only.
enum class InputFields { Full, Compact };
// spawn-walk: each synthetic player walks its own small circle around its spawn, moving as a real
// player does. spawn-follow copies the reference's position as its own snapshots report it, so
// under a byte budget a whole crowd moves in steps a real player never makes.
enum class ReferenceLayout { Ring, Spawn, SpawnFollow, SpawnWalk };

inline const char* reference_layout_name(ReferenceLayout layout) {
  switch (layout) {
    case ReferenceLayout::Ring:
      return "ring";
    case ReferenceLayout::Spawn:
      return "spawn";
    case ReferenceLayout::SpawnFollow:
      return "spawn-follow";
    case ReferenceLayout::SpawnWalk:
      return "spawn-walk";
  }
  return "unknown";
}
enum class ReferenceUpdates { Live, FrozenIdle };

inline const char* reference_updates_name(ReferenceUpdates updates) {
  return updates == ReferenceUpdates::FrozenIdle ? "frozen-idle" : "live";
}

// How clients leave when the run ends: graceful sends the KSTP disconnect notice, timeout just
// stops (the server removes the member after its inactivity timeout).
enum class LeaveMode { Timeout, Graceful };

inline const char* workload_name(Workload workload) {
  switch (workload) {
    case Workload::Idle:
      return "idle";
    case Workload::Distributed:
      return "distributed";
    case Workload::Dense:
      return "dense";
  }
  return "unknown";
}

struct LoadOptions {
  std::string host = "127.0.0.1";
  unsigned port = 7777;
  unsigned clients = 8;
  double duration_seconds = 60.0;  // measurement window after the join phase
  double join_timeout_seconds = 15.0;
  double warmup_seconds = 2.0;  // settle time between the join phase and measurement
  double tick_hz = 60.0;        // client network ticks (receive, apply, flush) per second
  double input_hz = 60.0;       // owner input messages per client per second (movement only)
  double report_interval_seconds = 5.0;
  Workload workload = Workload::Distributed;
  InputFields input_fields = InputFields::Full;
  float spacing_meters = 200.0f;      // distributed: grid spacing between player centres
  float dense_radius_meters = 10.0f;  // dense: all players stay inside this circle
  float move_radius_meters = 5.0f;    // distributed: each player circles its own centre
  float speed_meters_per_second = 5.0f;
  // Multi-process generation: this process drives players [index_offset, index_offset + clients)
  // of a layout planned for `layout_count` players (0 = clients), so several processes together
  // place players exactly as one process would.
  unsigned index_offset = 0;
  unsigned layout_count = 0;
  // Emulated path per client socket (netsim/impairment.hpp): loss per direction and one-way
  // delay added to both directions, so the added round trip is 2 x delay.
  double loss_fraction = 0.0;
  double delay_ms = 0.0;
  LeaveMode leave = LeaveMode::Timeout;
  // Opt-in native-scene fixture: copy a real player's valid replicated pose into synthetic
  // players arranged around it. Zero preserves the existing transport-only workloads.
  std::uint32_t reference_player = 0;
  ReferenceLayout reference_layout = ReferenceLayout::Ring;
  ReferenceUpdates reference_updates = ReferenceUpdates::Live;
  std::string reference_capture;

  unsigned total_players() const {
    return layout_count ? layout_count : clients;
  }
};

inline std::string load_usage() {
  return "kcd2s-load-probe [--host 127.0.0.1] [--port 7777] [--clients 8] [--duration 60]\n"
         "  [--workload idle|distributed|dense] [--tick-hz 60] [--input-hz 60]\n"
         "  [--join-timeout 15] [--warmup 2] [--report-interval 5] [--spacing 200]\n"
         "  [--dense-radius 10] [--move-radius 5] [--speed 5]\n"
         "  [--input-fields full|compact] [--index-offset 0] [--layout-count N]\n"
         "  [--loss 0..0.5] [--delay-ms 0..1000] [--leave timeout|graceful]\n"
         "  [--reference-player ENTITY_ID] [--reference-layout ring|spawn|spawn-follow|spawn-walk] "
         "(full "
         "fields)\n"
         "  [--reference-updates live|frozen-idle] [--reference-capture PATH]\n";
}

namespace detail {

inline double parse_number(std::string_view name, const std::string& text, double min, double max) {
  std::size_t used = 0;
  double value = 0.0;
  try {
    value = std::stod(text, &used);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(name) + " expects a number");
  }
  if (used != text.size() || !(value >= min) || !(value <= max)) {
    throw std::invalid_argument(std::string(name) + " is out of range");
  }
  return value;
}

inline unsigned parse_whole(std::string_view name, const std::string& text, unsigned min,
                            unsigned max) {
  const double value = parse_number(name, text, min, max);
  if (value != static_cast<double>(static_cast<unsigned>(value))) {
    throw std::invalid_argument(std::string(name) + " expects a whole number");
  }
  return static_cast<unsigned>(value);
}

}  // namespace detail

// Parses `--name value` pairs. Throws std::invalid_argument on unknown or malformed options.
inline LoadOptions parse_load_options(const std::vector<std::string>& arguments) {
  LoadOptions options;
  for (std::size_t i = 0; i < arguments.size(); i += 2) {
    const std::string& name = arguments[i];
    if (i + 1 >= arguments.size()) {
      throw std::invalid_argument(name + " needs a value");
    }
    const std::string& value = arguments[i + 1];
    if (name == "--host") {
      options.host = value;
    } else if (name == "--port") {
      options.port = detail::parse_whole(name, value, 1, 65535);
    } else if (name == "--clients") {
      options.clients = detail::parse_whole(name, value, 1, 5000);
    } else if (name == "--duration") {
      options.duration_seconds = detail::parse_number(name, value, 1.0, 86400.0);
    } else if (name == "--join-timeout") {
      options.join_timeout_seconds = detail::parse_number(name, value, 1.0, 3600.0);
    } else if (name == "--warmup") {
      options.warmup_seconds = detail::parse_number(name, value, 0.0, 3600.0);
    } else if (name == "--tick-hz") {
      options.tick_hz = detail::parse_number(name, value, 1.0, 1000.0);
    } else if (name == "--input-hz") {
      options.input_hz = detail::parse_number(name, value, 0.1, 1000.0);
    } else if (name == "--report-interval") {
      options.report_interval_seconds = detail::parse_number(name, value, 0.5, 3600.0);
    } else if (name == "--spacing") {
      options.spacing_meters = float(detail::parse_number(name, value, 1.0, 100000.0));
    } else if (name == "--dense-radius") {
      options.dense_radius_meters = float(detail::parse_number(name, value, 0.5, 10000.0));
    } else if (name == "--move-radius") {
      options.move_radius_meters = float(detail::parse_number(name, value, 0.5, 10000.0));
    } else if (name == "--index-offset") {
      options.index_offset = detail::parse_whole(name, value, 0, 100000);
    } else if (name == "--layout-count") {
      options.layout_count = detail::parse_whole(name, value, 1, 100000);
    } else if (name == "--reference-player") {
      options.reference_player =
          detail::parse_whole(name, value, 1, std::numeric_limits<std::uint32_t>::max());
    } else if (name == "--reference-layout") {
      if (value == "ring") {
        options.reference_layout = ReferenceLayout::Ring;
      } else if (value == "spawn") {
        options.reference_layout = ReferenceLayout::Spawn;
      } else if (value == "spawn-follow") {
        options.reference_layout = ReferenceLayout::SpawnFollow;
      } else if (value == "spawn-walk") {
        options.reference_layout = ReferenceLayout::SpawnWalk;
      } else {
        throw std::invalid_argument(
            "--reference-layout expects ring, spawn, spawn-follow or spawn-walk");
      }
    } else if (name == "--reference-updates") {
      if (value == "live") {
        options.reference_updates = ReferenceUpdates::Live;
      } else if (value == "frozen-idle") {
        options.reference_updates = ReferenceUpdates::FrozenIdle;
      } else {
        throw std::invalid_argument("--reference-updates expects live or frozen-idle");
      }
    } else if (name == "--reference-capture") {
      if (value.empty()) {
        throw std::invalid_argument("--reference-capture requires a nonempty path");
      }
      options.reference_capture = value;
    } else if (name == "--loss") {
      options.loss_fraction = detail::parse_number(name, value, 0.0, 0.5);
    } else if (name == "--delay-ms") {
      options.delay_ms = detail::parse_number(name, value, 0.0, 1000.0);
    } else if (name == "--leave") {
      if (value == "timeout") {
        options.leave = LeaveMode::Timeout;
      } else if (value == "graceful") {
        options.leave = LeaveMode::Graceful;
      } else {
        throw std::invalid_argument("--leave expects timeout or graceful");
      }
    } else if (name == "--speed") {
      options.speed_meters_per_second = float(detail::parse_number(name, value, 0.1, 100.0));
    } else if (name == "--workload") {
      if (value == "idle") {
        options.workload = Workload::Idle;
      } else if (value == "distributed") {
        options.workload = Workload::Distributed;
      } else if (value == "dense") {
        options.workload = Workload::Dense;
      } else {
        throw std::invalid_argument("--workload expects idle, distributed or dense");
      }
    } else if (name == "--input-fields") {
      if (value == "full") {
        options.input_fields = InputFields::Full;
      } else if (value == "compact") {
        options.input_fields = InputFields::Compact;
      } else {
        throw std::invalid_argument("--input-fields expects full or compact");
      }
    } else {
      throw std::invalid_argument("Unknown option " + name);
    }
  }
  if (options.input_hz > options.tick_hz) {
    throw std::invalid_argument("--input-hz cannot exceed --tick-hz");
  }
  if (options.reference_player && options.input_fields != InputFields::Full) {
    throw std::invalid_argument("--reference-player requires --input-fields full");
  }
  if (!options.reference_player && options.reference_layout != ReferenceLayout::Ring) {
    throw std::invalid_argument(
        "--reference-layout spawn or spawn-follow requires --reference-player");
  }
  if (options.reference_layout == ReferenceLayout::SpawnFollow &&
      options.reference_updates != ReferenceUpdates::Live) {
    throw std::invalid_argument(
        "--reference-layout spawn-follow requires --reference-updates live");
  }
  if (options.reference_updates == ReferenceUpdates::FrozenIdle &&
      (!options.reference_player || options.workload != Workload::Idle)) {
    throw std::invalid_argument(
        "--reference-updates frozen-idle requires --reference-player "
        "and --workload idle");
  }
  if (!options.reference_capture.empty() && !options.reference_player) {
    throw std::invalid_argument("--reference-capture requires --reference-player");
  }
  if (std::uint64_t(options.index_offset) + options.clients > options.total_players()) {
    throw std::invalid_argument("--index-offset plus --clients exceeds --layout-count");
  }
  return options;
}

}  // namespace kcd2s::local::load
