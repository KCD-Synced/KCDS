#include "map_labels_movie.hpp"

#include "../native_ui/as2_script.hpp"

namespace kcds::runtime::kcd1 {
namespace {

using namespace native_ui::as2;

// The label's new SetData: the game's own with the same arguments, then the face hidden while the
// client asks for it; it returns what the game's returns (the index of the next label's data).
Script set_data_wrapper() {
  Script s;
  const std::size_t done = s.label();
  s.push({"result"});
  s.value("arguments");
  s.value("this");
  s.call("this.kcdsSetData", "apply", 2);
  s.op(kDefineLocal);
  s.value("_root.kcdsMapLabels.hideReputation");
  s.op(kNot);
  s.jump_if(done);
  s.local("face", "this.m_Mc.reputationIcon");
  s.jump_if_undefined("face", done);
  s.set("face._visible", false);
  s.place(done);
  s.value("result");
  s.op(kReturn);
  return s;
}

Script frame_script() {
  Script s;
  const std::size_t wrapped = s.label();
  s.local("host", "this");
  s.jump_if_undefined("_global.MapLabel", wrapped);
  s.local("label", "_global.MapLabel.prototype");
  s.jump_if_defined("label.kcdsSetData", wrapped);
  s.copy("label.kcdsSetData", "label.SetData");
  s.target("label.SetData");
  s.function({}, set_data_wrapper());
  s.op(kSetMember);
  s.place(wrapped);
  s.copy("host.wrapped", "_global.MapLabel.prototype.kcdsSetData");
  s.set("host.ready", true);
  return s;
}

}  // namespace

std::vector<std::uint8_t> map_labels_movie() {
  return movie(frame_script());
}

}  // namespace kcds::runtime::kcd1
