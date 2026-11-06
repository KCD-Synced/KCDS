// Real UDP diagnostic: not a game client. Checks both ownership and bidirectional movement.
#include "runtime.hpp"
#include "animation.hpp"
#include <thread>
#include <cmath>
using namespace kcd2s;
using namespace kcd2s::local;
int main(int argc, char** argv) {
  try {
    Winsock winsock;
    auto port = argc > 1 ? std::stoul(argv[1]) : 7777;
    if (!port || port > 65535)
      throw std::runtime_error("Invalid port");
    const char* ip = argc > 2 ? argv[2] : "127.0.0.1";
    Client a(ip, static_cast<unsigned short>(port)), b(ip, static_cast<unsigned short>(port));
    double start = clock_seconds();
    bool run = false, jump = false, land = false;
    while (clock_seconds() - start < 8) {
      double now = clock_seconds();
      a.tick(now);
      b.tick(now);
      for (auto* c : {&a, &b}) {
        if (c->peer().world_config() && !c->peer().world_ready()) {
          const auto& world = *c->peer().world_config();
          std::cout << "SERVER WORLD " << world.map << " debug=" << world.debug
                    << " quests=" << world.quests << '\n';
          // Headless diagnostic only: emulate the engine's completion callback.
          c->peer().mark_world_ready(world.revision);
        }
      }
      auto ea = a.own_avatar(), eb = b.own_avatar();
      if (ea != core::kInvalidEntity && eb != core::kInvalidEntity && ea != eb) {
        int phase = !run ? 0 : (!jump ? 1 : 2);
        core::Vec3 pa{10.0f, 20.0f, phase == 1 ? 3.0f : 0.0f};
        core::Vec3 pb{-10.0f, -20.0f, phase == 1 ? 4.0f : 0.0f};
        auto animation = [&](int owner) {
          AnimationState state{};
          auto& layer = state[0];
          layer.sequence = std::uint64_t(phase + 1);
          layer.clip = phase == 1 ? -1 : owner;
          layer.params.flags = 0x102;
          return encode_animation(state);
        };
        auto gait = std::int64_t(phase == 1 ? Locomotion::Jump : Locomotion::Run);
        a.peer().send_input(ea, {{Animations, animation(101)},
                                 {Position, pa},
                                 {Rotation, core::Quat{0, 0, 0, 1}},
                                 {Velocity, core::Vec3{2, 0, phase == 1 ? 3.0f : 0.0f}},
                                 {Grounded, phase != 1},
                                 {Gait, gait},
                                 {JumpSequence, std::int64_t(phase ? 1 : 0)},
                                 {PoseValid, true},
                                 {MotionValid, true},
                                 {MotionType, std::int64_t{1}},
                                 {MotionVelocity, core::Vec3{pa.x, 0, phase == 1 ? 3.0f : 0.0f}},
                                 {MotionRotation, core::Quat{0, 0, 0, 1}},
                                 {MotionFlags, std::int64_t(phase == 1 ? 3 : 1)},
                                 {MotionMask, std::int64_t{7}},
                                 {MotionParams0, core::Vec3{pa.x, float(phase), 1}}});
        b.peer().send_input(eb, {{Animations, animation(202)},
                                 {Position, pb},
                                 {Rotation, core::Quat{0, 0, 0, 1}},
                                 {Velocity, core::Vec3{-2, 0, phase == 1 ? 3.0f : 0.0f}},
                                 {Grounded, phase != 1},
                                 {Gait, gait},
                                 {JumpSequence, std::int64_t(phase ? 1 : 0)},
                                 {PoseValid, true},
                                 {MotionValid, true},
                                 {MotionType, std::int64_t{1}},
                                 {MotionVelocity, core::Vec3{pb.x, 0, phase == 1 ? 3.0f : 0.0f}},
                                 {MotionRotation, core::Quat{0, 0, 0, 1}},
                                 {MotionFlags, std::int64_t(phase == 1 ? 3 : 1)},
                                 {MotionMask, std::int64_t{7}},
                                 {MotionParams0, core::Vec3{pb.x, float(phase), 1}}});
        auto check = [&](Client& c, core::EntityId e, core::Vec3 expected) {
          auto it = c.peer().mirror().entities().find(e);
          if (it == c.peer().mirror().entities().end())
            return false;
          const auto& f = it->second.fields;
          auto clips = decode_animation(std::get<std::string>(f[Animations]));
          if (!clips || (*clips)[0].sequence != std::uint64_t(phase + 1) ||
              (*clips)[0].clip != (phase == 1 ? -1 : (expected.x > 0 ? 101 : 202)))
            return false;
          auto p = std::get<core::Vec3>(f[Position]);
          return std::abs(p.x - expected.x) < 0.001f && std::abs(p.z - expected.z) < 0.001f &&
                 std::get<bool>(f[Grounded]) == (phase != 1) &&
                 std::get<std::int64_t>(f[Gait]) == gait &&
                 std::get<std::int64_t>(f[JumpSequence]) == (phase ? 1 : 0) &&
                 std::get<bool>(f[MotionValid]) && std::get<std::int64_t>(f[MotionType]) == 1 &&
                 std::get<core::Vec3>(f[MotionVelocity]).x == expected.x &&
                 std::get<std::int64_t>(f[MotionFlags]) == (phase == 1 ? 3 : 1) &&
                 std::get<std::int64_t>(f[MotionMask]) == 7 &&
                 std::get<core::Vec3>(f[MotionParams0]).x == expected.x &&
                 std::get<core::Vec3>(f[MotionParams0]).y == float(phase);
        };
        if (check(a, eb, pb) && check(b, ea, pa)) {
          if (phase == 0) {
            run = true;
            std::cout << "PASS two owners + bidirectional run\n";
          }
          if (phase == 1) {
            jump = true;
            std::cout << "PASS jump + airborne + jump sequence\n";
          }
          if (phase == 2) {
            land = true;
            std::cout << "PASS landing + native motion + clip start/stop/restart state\n";
            break;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    // Leave gracefully so the server removes both probe players now, not after its timeout.
    a.disconnect();
    b.disconnect();
    if (!run || !jump || !land)
      throw std::runtime_error("Timed out waiting for two-client movement convergence");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
