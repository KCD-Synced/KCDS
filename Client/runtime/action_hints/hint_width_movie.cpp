#include "hint_width_movie.hpp"

#include "../native_ui/as2_script.hpp"

namespace kcds::runtime::action_hints {
namespace {

using namespace native_ui::as2;

// The hint's new drawInt: the game's own, then its text field as wide as its text. The game puts
// the field's left edge one text width before the key icon, but the field keeps the width the
// movie gave it, so a longer text was cut off before the icon.
Script draw_wrapper() {
  Script s;
  const std::size_t done = s.label();
  s.run("this", "kcdsDrawInt", 0);
  s.local("field", "this.m_mc.tfText");
  s.jump_if_undefined("field", done);
  s.target("field._width");
  s.value("field.textWidth");
  s.push({5});
  s.op(kAdd2);
  s.op(kSetMember);
  s.place(done);
  return s;
}

Script frame_script() {
  Script s;
  const std::size_t wrapped = s.label();
  s.local("host", "this");
  s.jump_if_undefined("_global.ActionHintNotification", wrapped);
  s.local("hint", "_global.ActionHintNotification.prototype");
  s.jump_if_defined("hint.kcdsDrawInt", wrapped);
  s.copy("hint.kcdsDrawInt", "hint.drawInt");
  s.target("hint.drawInt");
  s.function({}, draw_wrapper());
  s.op(kSetMember);
  s.place(wrapped);
  s.set("host.ready", true);
  return s;
}

}  // namespace

std::vector<std::uint8_t> hint_width_movie() {
  return movie(frame_script());
}

}  // namespace kcds::runtime::action_hints
