#include "runtime.hpp"
#include <thread>
int main(int argc, char** argv) {
  try {
    kcd2s::local::Winsock winsock;
    const unsigned short port = argc > 1 ? static_cast<unsigned short>(std::stoul(argv[1])) : 17779;
    kcd2s::local::Client a("127.0.0.1", port), b("127.0.0.1", port);
    const auto start = kcd2s::local::clock_seconds();
    bool ready = false, sent = false, plain = false;
    bool ah = false, bh = false, ac = false, bc = false;
    unsigned own_a = 0, own_b = 0;
    while (kcd2s::local::clock_seconds() - start < 10) {
      const auto now = kcd2s::local::clock_seconds();
      a.tick(now);
      b.tick(now);
      own_a = a.own_avatar();
      own_b = b.own_avatar();
      if (own_a && own_b && a.peer().world_config() && b.peer().world_config() && !ready) {
        a.peer().mark_world_ready(1);
        b.peer().mark_world_ready(1);
        ready = true;
      }
      if (ready) {
        using namespace kcd2s;
        for (auto c : {&a, &b})
          c->peer().send_input(c->own_avatar(), {{local::Position, core::Vec3{10, 20, 3}},
                                                 {local::Rotation, core::Quat{}},
                                                 {local::PoseValid, true}});
      }
      if (ready && !sent && now - start > 1) {
        sent = a.peer().send_chat(own_a, "/Horse");
        std::cout << "COMMAND sent=" << sent << std::endl;
      }
      if (sent && !plain && now - start > 2) {
        plain = a.peer().send_chat(own_a, "resource chat smoke");
      }
      for (auto pair : {std::pair{&a, &ah}, std::pair{&b, &bh}}) {
        for (const auto& [id, e] : pair.first->peer().mirror().entities()) {
          (void)id;
          if (e.type == kcd2s::core::type_id("NetworkHorse")) {
            const auto p = std::get<kcd2s::core::Vec3>(e.fields[0]);
            if (std::abs(p.x - 10) < .01f && std::abs(p.y - 23) < .01f && std::abs(p.z - 3) < .01f)
              *pair.second = true;
          }
        }
      }
      for (auto message : a.peer().take_chat()) {
        std::cout << "A CHAT " << message.author << ": " << message.text << std::endl;
        if (message.text == "resource chat smoke")
          ac = true;
      }
      for (auto message : b.peer().take_chat()) {
        std::cout << "B CHAT " << message.author << ": " << message.text << std::endl;
        if (message.text == "resource chat smoke")
          bc = true;
      }
      if (ah && bh && ac && bc) {
        std::cout
            << "PASS real TypeScript /horse -> server entity -> two client mirrors; chat relayed"
            << std::endl;
        a.disconnect();
        b.disconnect();
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "FAIL horse=" << ah << "," << bh << " chat=" << ac << "," << bc << std::endl;
    a.disconnect();
    b.disconnect();
    return 1;
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }
}
