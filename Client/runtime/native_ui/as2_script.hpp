// A writer of small ActionScript 2 movies (SWF 8) the client loads into the game's own movies, to
// change how a game screen draws (the inventory's max column, the width of the action hints).
#ifndef KCDS_RUNTIME_NATIVE_UI_AS2_SCRIPT_HPP
#define KCDS_RUNTIME_NATIVE_UI_AS2_SCRIPT_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace kcds::runtime::native_ui::as2 {

// The ActionScript 2 actions the movies use (SWF 8).
enum Op : std::uint8_t {
  kSubtract = 0x0B,
  kNot = 0x12,
  kPop = 0x17,
  kGetVariable = 0x1C,
  kDefineLocal = 0x3C,
  kReturn = 0x3E,
  kInitObject = 0x43,
  kAdd2 = 0x47,
  kLess2 = 0x48,
  kEquals2 = 0x49,
  kGetMember = 0x4E,
  kSetMember = 0x4F,
  kIncrement = 0x50,
  kCallMethod = 0x52,
  kPush = 0x96,
  kJump = 0x99,
  kDefineFunction = 0x9B,
  kIf = 0x9D,
};

// One value of a push.
struct Value {
  enum class Kind { kText, kNumber, kTrue, kFalse, kUndefined } kind;
  std::string text;
  std::int32_t number = 0;

  Value(const char* value) : kind(Kind::kText), text(value) {
  }
  Value(std::string value) : kind(Kind::kText), text(std::move(value)) {
  }
  Value(int value) : kind(Kind::kNumber), number(value) {
  }
  Value(bool value) : kind(value ? Kind::kTrue : Kind::kFalse) {
  }

  static Value undefined() {
    Value value(0);
    value.kind = Kind::kUndefined;
    return value;
  }
};

// An action list being written, with labels for its jumps (offsets count from the end of the
// jump, within the same list).
class Script {
 public:
  std::size_t label() {
    labels_.push_back(SIZE_MAX);
    return labels_.size() - 1;
  }

  void place(std::size_t label) {
    labels_[label] = bytes_.size();
  }

  void op(Op code) {
    bytes_.push_back(code);
  }

  void push(std::initializer_list<Value> values) {
    std::vector<std::uint8_t> payload;
    for (const Value& value : values) {
      switch (value.kind) {
        case Value::Kind::kText:
          payload.push_back(0);
          payload.insert(payload.end(), value.text.begin(), value.text.end());
          payload.push_back(0);
          break;
        case Value::Kind::kNumber:
          payload.push_back(7);
          for (int shift = 0; shift < 32; shift += 8) {
            payload.push_back(std::uint8_t(std::uint32_t(value.number) >> shift));
          }
          break;
        case Value::Kind::kTrue:
        case Value::Kind::kFalse:
          payload.push_back(5);
          payload.push_back(value.kind == Value::Kind::kTrue ? 1 : 0);
          break;
        case Value::Kind::kUndefined:
          payload.push_back(3);
          break;
      }
    }
    long_op(kPush, payload);
  }

  // Jumps to the label when the popped value is true, or always.
  void jump_if(std::size_t label) {
    jump(kIf, label);
  }

  void jump_to(std::size_t label) {
    jump(kJump, label);
  }

  // An anonymous function object on the stack: its parameters are variables by name.
  void function(std::initializer_list<const char*> parameters, const Script& body) {
    std::vector<std::uint8_t> payload{0};  // no name
    payload.push_back(std::uint8_t(parameters.size()));
    payload.push_back(0);
    for (const char* parameter : parameters) {
      payload.insert(payload.end(), parameter,
                     parameter + std::char_traits<char>::length(parameter));
      payload.push_back(0);
    }
    const std::vector<std::uint8_t> code = body.bytes();
    payload.push_back(std::uint8_t(code.size()));
    payload.push_back(std::uint8_t(code.size() >> 8));
    long_op(kDefineFunction, payload);
    bytes_.insert(bytes_.end(), code.begin(), code.end());
  }

  // --- expressions ---

  // name: a variable, or a path "a.b.c" of members below one.
  void value(const std::string& path) {
    std::size_t start = 0;
    std::size_t dot = path.find('.');
    push({path.substr(0, dot)});
    op(kGetVariable);
    while (dot != std::string::npos) {
      start = dot + 1;
      dot = path.find('.', start);
      push({path.substr(start, dot == std::string::npos ? std::string::npos : dot - start)});
      op(kGetMember);
    }
  }

  // path.member = <the value the caller pushed after this call's object and name>: pushes the
  // object and the member name; the caller pushes the value and calls op(kSetMember).
  void target(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    value(path.substr(0, dot));
    push({path.substr(dot + 1)});
  }

  void set(const std::string& path, Value constant) {
    target(path);
    push({std::move(constant)});
    op(kSetMember);
  }

  void copy(const std::string& to, const std::string& from) {
    target(to);
    value(from);
    op(kSetMember);
  }

  // var name = <value on the stack>: the caller pushes the name first.
  void local(const char* name, const std::string& from) {
    push({name});
    value(from);
    op(kDefineLocal);
  }

  // object.method(args) with the arguments already pushed, last first; the result stays.
  void call(const std::string& object, const char* method, int arguments) {
    push({arguments});
    value(object);
    push({method});
    op(kCallMethod);
  }

  // The same, the result dropped.
  void run(const std::string& object, const char* method, int arguments) {
    call(object, method, arguments);
    op(kPop);
  }

  // if (path == undefined) jump.
  void jump_if_undefined(const std::string& path, std::size_t label) {
    value(path);
    push({Value::undefined()});
    op(kEquals2);
    jump_if(label);
  }

  void jump_if_defined(const std::string& path, std::size_t label) {
    value(path);
    push({Value::undefined()});
    op(kEquals2);
    op(kNot);
    jump_if(label);
  }

  void leave() {
    push({Value::undefined()});
    op(kReturn);
  }

  std::vector<std::uint8_t> bytes() const {
    std::vector<std::uint8_t> out = bytes_;
    for (const auto& [at, label] : fixups_) {
      const auto offset = std::int16_t(labels_[label] - (at + 2));
      out[at] = std::uint8_t(offset);
      out[at + 1] = std::uint8_t(std::uint16_t(offset) >> 8);
    }
    return out;
  }

 private:
  void long_op(Op code, const std::vector<std::uint8_t>& payload) {
    bytes_.push_back(code);
    bytes_.push_back(std::uint8_t(payload.size()));
    bytes_.push_back(std::uint8_t(payload.size() >> 8));
    bytes_.insert(bytes_.end(), payload.begin(), payload.end());
  }

  void jump(Op code, std::size_t label) {
    bytes_.push_back(code);
    bytes_.push_back(2);
    bytes_.push_back(0);
    fixups_.emplace_back(bytes_.size(), label);
    bytes_.push_back(0);
    bytes_.push_back(0);
  }

  std::vector<std::uint8_t> bytes_;
  std::vector<std::size_t> labels_;
  std::vector<std::pair<std::size_t, std::size_t>> fixups_;
};

namespace detail {

inline void put_u16(std::vector<std::uint8_t>& out, std::size_t value) {
  out.push_back(std::uint8_t(value));
  out.push_back(std::uint8_t(value >> 8));
}

inline void put_u32(std::vector<std::uint8_t>& out, std::size_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

// A tag with a long header (any length).
inline void put_tag(std::vector<std::uint8_t>& out, unsigned code,
                    const std::vector<std::uint8_t>& body) {
  put_u16(out, (code << 6) | 0x3F);
  put_u32(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
}

}  // namespace detail

// A movie of one frame that runs the script once, when it loads.
inline std::vector<std::uint8_t> movie(const Script& frame) {
  std::vector<std::uint8_t> actions = frame.bytes();
  actions.push_back(0);                     // the end of the actions
  std::vector<std::uint8_t> body{0x00};     // an empty frame rectangle (Nbits 0)
  detail::put_u16(body, 24 << 8);           // 24 frames a second
  detail::put_u16(body, 1);                 // one frame
  detail::put_tag(body, 69, {0, 0, 0, 0});  // FileAttributes: ActionScript 2
  detail::put_tag(body, 12, actions);       // DoAction
  detail::put_tag(body, 1, {});             // ShowFrame
  detail::put_tag(body, 0, {});             // End
  std::vector<std::uint8_t> out{'F', 'W', 'S', 8};
  detail::put_u32(out, body.size() + 8);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace kcds::runtime::native_ui::as2

#endif
