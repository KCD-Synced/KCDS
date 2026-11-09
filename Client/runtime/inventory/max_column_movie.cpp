#include "max_column_movie.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../native_ui/as2_script.hpp"

namespace kcds::runtime::inventory {
namespace {

using namespace native_ui::as2;

// The icon (icon 10 of the max carry set): a hash as large as the game's own # in the sort bar,
// between two brackets, in the colour of the bar's icons, around 0,0 in movie units.
struct Bar {
  int left;
  int top;
  int right;
  int bottom;
};
constexpr Bar kIconPlate{-34, -27, 34, 27};
constexpr Bar kIconBars[] = {
    {-10, -18, -3, 18},  {3, -18, 10, 18},     {-16, -10, 16, -3}, {-16, 3, 16, 10},
    {-30, -23, -24, 23}, {-24, -23, -19, -18}, {-24, 18, -19, 23}, {24, -23, 30, 23},
    {19, -23, 24, -18},  {19, 18, 24, 23},
};
constexpr int kIconColour = 0xE2D184;

void draw_bar(Script& s, const char* clip, const Bar& bar) {
  const std::pair<int, int> corners[] = {
      {bar.right, bar.top}, {bar.right, bar.bottom}, {bar.left, bar.bottom}, {bar.left, bar.top}};
  s.push({bar.top, bar.left});
  s.run(clip, "moveTo", 2);
  for (const auto& [x, y] : corners) {
    s.push({y, x});
    s.run(clip, "lineTo", 2);
  }
}

// host.cell(mc, data): one row of the list, drawn by the game a moment before.
Script cell_function() {
  Script s;
  const std::size_t done = s.label();
  const std::size_t homed = s.label();
  const std::size_t shown = s.label();
  const std::size_t made = s.label();
  const std::size_t limited = s.label();
  s.jump_if_undefined("mc.amount", done);
  s.jump_if_undefined("mc.stat", done);
  // Where the game puts the equip and amount cells, kept the first time.
  s.jump_if_defined("mc.kcdsHome", homed);
  s.copy("mc.kcdsHome", "mc.amount._x");
  s.copy("mc.kcdsShirt", "mc.outfitPresence._x");
  s.place(homed);
  // One column: from the amount to the next cell.
  s.push({"step"});
  s.value("mc.stat._x");
  s.value("mc.kcdsHome");
  s.op(kSubtract);
  s.op(kDefineLocal);
  s.value("host.on");
  s.jump_if(shown);
  s.copy("mc.amount._x", "mc.kcdsHome");
  s.copy("mc.outfitPresence._x", "mc.kcdsShirt");
  s.set("mc.kcdsMaxCell._visible", false);
  s.leave();
  s.place(shown);
  // The equip and amount cells one column to the left; the limit where the amount was.
  for (const auto& [cell, home] : {std::pair{"mc.amount._x", "mc.kcdsHome"},
                                   std::pair{"mc.outfitPresence._x", "mc.kcdsShirt"}}) {
    s.target(cell);
    s.value(home);
    s.value("step");
    s.op(kSubtract);
    s.op(kSetMember);
  }
  s.jump_if_defined("mc.kcdsMaxCell", made);
  s.call("mc", "getNextHighestDepth", 0);
  s.push({"kcdsMaxCell"});
  s.run("mc.amount", "duplicateMovieClip", 2);
  s.place(made);
  s.copy("mc.kcdsMaxCell._x", "mc.kcdsHome");
  s.copy("mc.kcdsMaxCell._y", "mc.amount._y");
  // The row's item: its limit, when its class has one. The player's own items come by their
  // names; another inventory's rows (a trader's or a container's, IsPlayerInventoryId not true)
  // by their class's icon.
  const std::size_t found = s.label();
  s.push({"max"});
  s.value("host.limits");
  s.value("data.Id");
  s.op(kGetMember);
  s.op(kDefineLocal);
  s.jump_if_defined("max", found);
  s.value("data.IsPlayerInventoryId");
  s.push({true});
  s.op(kEquals2);
  s.jump_if(found);
  s.push({"max"});
  s.value("host.limits");
  s.push({"i"});
  s.value("data.IconId");
  s.op(kAdd2);
  s.op(kGetMember);
  s.op(kDefineLocal);
  s.place(found);
  s.jump_if_defined("max", limited);
  s.set("mc.kcdsMaxCell._visible", false);
  s.leave();
  s.place(limited);
  s.set("mc.kcdsMaxCell._visible", true);
  s.copy("mc.kcdsMaxCell.tField.text", "max");
  s.value("mc");
  s.push({1});
  s.value("host");
  s.push({"tint"});
  s.op(kCallMethod);
  s.op(kPop);
  s.place(done);
  return s;
}

// host.tint(mc): the limit in the amount's frame (focused or not) and colour.
Script tint_function() {
  Script s;
  const std::size_t done = s.label();
  s.jump_if_undefined("mc.kcdsMaxCell", done);
  s.value("mc.amount._currentframe");
  s.run("mc.kcdsMaxCell", "gotoAndStop", 1);
  s.copy("mc.kcdsMaxCell.tField.textColor", "mc.amount.tField.textColor");
  s.place(done);
  return s;
}

// The icon's own rollover: its sort bar's own button rollover run on the icon, so its tooltip
// shows and goes as the game's icons' do.
Script rollover_function() {
  Script s;
  s.value("this");
  s.push({1});
  s.value("this._parent.sort1.onRollOver");
  s.push({"call"});
  s.op(kCallMethod);
  s.op(kPop);
  return s;
}

// host.drawIcon(mc).
Script icon_function() {
  Script s;
  s.run("mc", "clear", 0);
  // An invisible plate under the whole icon: the pointer between its bars stays on it.
  s.push({0, 0});
  s.run("mc", "beginFill", 2);
  draw_bar(s, "mc", kIconPlate);
  s.run("mc", "endFill", 0);
  s.push({100, kIconColour});
  s.run("mc", "beginFill", 2);
  for (const Bar& bar : kIconBars) {
    draw_bar(s, "mc", bar);
  }
  s.run("mc", "endFill", 0);
  s.copy("mc.tooltip", "host.tooltip");
  s.target("mc.onRollOver");
  s.function({}, rollover_function());
  s.op(kSetMember);
  return s;
}

// The sort bars: the inventory's, and the container screen's found and player sides
// (ItemTransfer.gfx main.inside.first and .second, laid out as the inventory's).
constexpr const char* kSortBars[] = {
    "_root.g_Content.sortContainer",
    "_root.main.inside.first.sortContainer",
    "_root.main.inside.second.sortContainer",
};

// host.header(): each sort bar the movie has laid out by host.headerOf.
Script header_function() {
  Script s;
  for (const char* bar : kSortBars) {
    s.value(bar);
    s.run("host", "headerOf", 1);
  }
  return s;
}

// host.headerOf(bar): the bar's equip and amount icons one column to the left, the max icon
// where the amount icon was; a bar the movie does not have is skipped.
Script bar_function() {
  Script s;
  const std::size_t done = s.label();
  const std::size_t homed = s.label();
  const std::size_t shown = s.label();
  const std::size_t made = s.label();
  s.jump_if_undefined("bar.sort3", done);
  s.jump_if_defined("bar.kcdsHome", homed);
  s.copy("bar.kcdsHome", "bar.sort2._x");
  s.copy("bar.kcdsShirt", "bar.sort1._x");
  s.place(homed);
  s.push({"step"});
  s.value("bar.sort3._x");
  s.value("bar.kcdsHome");
  s.op(kSubtract);
  s.op(kDefineLocal);
  s.value("host.on");
  s.jump_if(shown);
  s.copy("bar.sort2._x", "bar.kcdsHome");
  s.copy("bar.sort1._x", "bar.kcdsShirt");
  s.set("bar.kcdsMaxIcon._visible", false);
  s.leave();
  s.place(shown);
  for (const auto& [icon, home] :
       {std::pair{"bar.sort2._x", "bar.kcdsHome"}, std::pair{"bar.sort1._x", "bar.kcdsShirt"}}) {
    s.target(icon);
    s.value(home);
    s.value("step");
    s.op(kSubtract);
    s.op(kSetMember);
  }
  s.jump_if_defined("bar.kcdsMaxIcon", made);
  s.call("bar", "getNextHighestDepth", 0);
  s.push({"kcdsMaxIcon"});
  s.run("bar", "createEmptyMovieClip", 2);
  s.value("bar.kcdsMaxIcon");
  s.push({1});
  s.value("host");
  s.push({"drawIcon"});
  s.op(kCallMethod);
  s.op(kPop);
  s.place(made);
  s.copy("bar.kcdsMaxIcon._x", "bar.kcdsHome");
  s.copy("bar.kcdsMaxIcon._y", "bar.sort2._y");
  s.set("bar.kcdsMaxIcon._visible", true);
  s.place(done);
  return s;
}

// host.setLimits(text): "<on>:<item>=<max>|...", on 1 while the server limits any class, then
// the player's items of a limited class by the names the list gives them.
Script limits_function() {
  Script s;
  const std::size_t loop = s.label();
  const std::size_t end = s.label();
  s.push({"limits", 0});
  s.op(kInitObject);
  s.op(kDefineLocal);
  // var parts = text.substr(2).split("|");
  s.push({"parts", "|", 1, 2, 1});
  s.value("text");
  s.push({"substr"});
  s.op(kCallMethod);
  s.push({"split"});
  s.op(kCallMethod);
  s.op(kDefineLocal);
  s.push({"i", 0});
  s.op(kDefineLocal);
  s.place(loop);
  // while (i < parts.length)
  s.value("i");
  s.value("parts.length");
  s.op(kLess2);
  s.op(kNot);
  s.jump_if(end);
  // var pair = parts[i].split("="); limits[pair[0]] = pair[1];
  s.push({"pair", "=", 1});
  s.value("parts");
  s.value("i");
  s.op(kGetMember);
  s.push({"split"});
  s.op(kCallMethod);
  s.op(kDefineLocal);
  s.value("limits");
  s.value("pair");
  s.push({0});
  s.op(kGetMember);
  s.value("pair");
  s.push({1});
  s.op(kGetMember);
  s.op(kSetMember);
  s.push({"i"});
  s.value("i");
  s.op(kIncrement);
  s.op(kDefineLocal);
  s.jump_to(loop);
  s.place(end);
  s.copy("host.limits", "limits");
  // host.on = text.charAt(0) == "1";
  s.target("host.on");
  s.push({0, 1});
  s.value("text");
  s.push({"charAt"});
  s.op(kCallMethod);
  s.push({"1"});
  s.op(kEquals2);
  s.op(kSetMember);
  s.run("host", "header", 0);
  // The list's view redraws its rows: the inventory's g_View, the container screen's two (the
  // found and the player's side, ItemTransfer.gfx); a view the movie does not have is skipped.
  s.push({true});
  s.run("_root.g_View", "periodicalUpdate", 1);
  s.push({true});
  s.run("_root.g_ViewFirst", "periodicalUpdate", 1);
  s.push({true});
  s.run("_root.g_ViewSecond", "periodicalUpdate", 1);
  return s;
}

// view.m_setDataFunc = function(mc, data) { this.kcdsSetData(mc, data); _root.kcdsMax.cell(...);
// _root.kcdsMax.header(); }: the sort bar is laid out again by the container screen's own redraw
// (its found and player sides), so its icon follows each row.
Script set_data_wrapper() {
  Script s;
  s.value("data");
  s.value("mc");
  s.run("this", "kcdsSetData", 2);
  s.value("data");
  s.value("mc");
  s.run("_root.kcdsMax", "cell", 2);
  s.run("_root.kcdsMax", "header", 0);
  return s;
}

// view.colorizeName = function(mc) { this.kcdsColorize(mc); _root.kcdsMax.tint(mc); }
Script colorize_wrapper() {
  Script s;
  s.value("mc");
  s.run("this", "kcdsColorize", 1);
  s.value("mc");
  s.run("_root.kcdsMax", "tint", 1);
  return s;
}

// The movie's frame: the host's functions on the clip it was loaded into, then the list's row
// drawing wrapped once.
Script frame_script() {
  Script s;
  const std::size_t wrapped = s.label();
  s.local("host", "this");
  s.target("host.limits");
  s.push({0});
  s.op(kInitObject);
  s.op(kSetMember);
  s.set("host.on", false);
  s.set("host.tooltip", "kcds_tip_max_carry");
  s.target("host.cell");
  s.function({"mc", "data"}, cell_function());
  s.op(kSetMember);
  s.target("host.tint");
  s.function({"mc"}, tint_function());
  s.op(kSetMember);
  s.target("host.drawIcon");
  s.function({"mc"}, icon_function());
  s.op(kSetMember);
  s.target("host.headerOf");
  s.function({"bar"}, bar_function());
  s.op(kSetMember);
  s.target("host.header");
  s.function({}, header_function());
  s.op(kSetMember);
  s.target("host.setLimits");
  s.function({"text"}, limits_function());
  s.op(kSetMember);
  s.local("view", "_global.ItemView.prototype");
  s.jump_if_defined("view.kcdsSetData", wrapped);
  s.copy("view.kcdsSetData", "view.m_setDataFunc");
  s.target("view.m_setDataFunc");
  s.function({"mc", "data"}, set_data_wrapper());
  s.op(kSetMember);
  s.copy("view.kcdsColorize", "view.colorizeName");
  s.target("view.colorizeName");
  s.function({"mc"}, colorize_wrapper());
  s.op(kSetMember);
  s.place(wrapped);
  s.set("host.ready", true);
  return s;
}

}  // namespace

std::vector<std::uint8_t> max_column_movie() {
  return native_ui::as2::movie(frame_script());
}

}  // namespace kcds::runtime::inventory
