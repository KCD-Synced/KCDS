#pragma once
// The part of YAML the kcds-bots scenario files use, read into a small tree: block maps
// ("key: value", nested by indentation), block lists ("- item", an item may start a map), flow
// maps and lists ("{ a: 1, b: [2, 3] }", also over several lines and with a trailing comma, as
// Prettier writes them), plain and quoted scalars and "#" comments. Anchors,
// tags, multi-line strings and several documents are not part of it; a file that uses them is an
// error with its line number. Our own small parser: the scenarios need no more, and no YAML
// library is vendored.
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kcd2s::local::bots::yaml {

struct Node {
  enum class Kind { Null, Scalar, List, Map };
  Kind kind = Kind::Null;
  std::string scalar;
  std::vector<Node> list;
  std::vector<std::pair<std::string, Node>> map;  // in file order
  int line = 0;                                   // where it starts (1-based)

  bool is_map() const {
    return kind == Kind::Map;
  }
  bool is_list() const {
    return kind == Kind::List;
  }
  bool is_scalar() const {
    return kind == Kind::Scalar;
  }
  // The value of `key` in a map, or nullptr.
  const Node* find(const std::string& key) const {
    for (const auto& [name, value] : map) {
      if (name == key) {
        return &value;
      }
    }
    return nullptr;
  }
};

// A file that is not in the subset: the message names the line.
class ParseError : public std::runtime_error {
 public:
  ParseError(int line, const std::string& what)
      : std::runtime_error("line " + std::to_string(line) + ": " + what), line_(line) {
  }
  int line() const {
    return line_;
  }

 private:
  int line_;
};

namespace detail {

struct Line {
  int number = 0;    // 1-based
  int indent = 0;    // leading spaces
  std::string text;  // without the indentation and the comment, right-trimmed
};

inline std::string trim(const std::string& text) {
  std::size_t first = 0;
  while (first < text.size() && (text[first] == ' ' || text[first] == '\t')) {
    ++first;
  }
  std::size_t last = text.size();
  while (last > first &&
         (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) {
    --last;
  }
  return text.substr(first, last - first);
}

// The text before a "#" comment (a "#" inside quotes, or not after a space, is text).
inline std::string strip_comment(const std::string& text) {
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#' && (i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
      return text.substr(0, i);
    }
  }
  return text;
}

// How many flow collections ("[", "{") a line opens and does not close, outside quotes.
inline int open_brackets(const std::string& text) {
  int depth = 0;
  char quote = 0;
  for (const char c : text) {
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '[' || c == '{') {
      ++depth;
    } else if (c == ']' || c == '}') {
      --depth;
    }
  }
  return depth;
}

// The file's lines without comments and blank lines; a flow collection that goes on over several
// lines (as Prettier writes a long one) is joined into the line it starts on.
inline std::vector<Line> split_lines(const std::string& source) {
  std::vector<Line> lines;
  std::size_t start = 0;
  int number = 0;
  int open = 0;  // of the last line, still to be closed
  while (start <= source.size()) {
    std::size_t end = source.find('\n', start);
    if (end == std::string::npos) {
      end = source.size();
    }
    ++number;
    const std::string raw = source.substr(start, end - start);
    int indent = 0;
    while (std::size_t(indent) < raw.size() && raw[std::size_t(indent)] == ' ') {
      ++indent;
    }
    if (std::size_t(indent) < raw.size() && raw[std::size_t(indent)] == '\t') {
      throw ParseError(number, "indent with spaces, not tabs");
    }
    const std::string text = trim(strip_comment(raw));
    if (!text.empty()) {
      if (open > 0) {
        lines.back().text += " " + text;
        open += open_brackets(text);
      } else if (text == "---" || text == "...") {
        if (!lines.empty()) {
          throw ParseError(number, "one document per file");
        }
      } else {
        lines.push_back({number, indent, text});
        open = open_brackets(text);
      }
    }
    if (end == source.size()) {
      break;
    }
    start = end + 1;
  }
  if (open > 0) {
    throw ParseError(lines.back().number, "a [ or { is not closed");
  }
  return lines;
}

// A scalar's text: quotes removed ("\"" and "\\" escapes in double quotes, '' in single).
inline std::string unquote(const std::string& text, int line) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
      if (text[i] == '\\' && i + 2 < text.size()) {
        const char next = text[++i];
        out.push_back(next == 'n' ? '\n' : next == 't' ? '\t' : next);
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }
  if (text.size() >= 2 && text.front() == '\'' && text.back() == '\'') {
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
      out.push_back(text[i]);
      if (text[i] == '\'' && i + 2 < text.size() && text[i + 1] == '\'') {
        ++i;
      }
    }
    return out;
  }
  if (!text.empty() && (text.front() == '"' || text.front() == '\'')) {
    throw ParseError(line, "unclosed quote: " + text);
  }
  if (!text.empty() && (text.front() == '&' || text.front() == '*' || text.front() == '!' ||
                        text.front() == '|' || text.front() == '>')) {
    throw ParseError(line, "anchors, aliases, tags and block strings are not supported: " + text);
  }
  return text;
}

// The flow collections ("[...]", "{...}") and scalars of one line.
class FlowReader {
 public:
  FlowReader(const std::string& text, int line) : text_(text), line_(line) {
  }

  Node read_value() {
    skip_space();
    Node node;
    node.line = line_;
    if (at_end()) {
      return node;  // null
    }
    if (peek() == '[') {
      ++at_;
      node.kind = Node::Kind::List;
      skip_space();
      if (!at_end() && peek() == ']') {
        ++at_;
        return node;
      }
      for (;;) {
        node.list.push_back(read_value());
        skip_space();
        if (at_end()) {
          throw ParseError(line_, "unclosed [");
        }
        if (peek() == ',') {
          ++at_;
          skip_space();
          if (!at_end() && peek() == ']') {
            ++at_;  // a trailing comma
            return node;
          }
          continue;
        }
        if (peek() == ']') {
          ++at_;
          return node;
        }
        throw ParseError(line_, std::string("expected , or ] but found ") + peek());
      }
    }
    if (peek() == '{') {
      ++at_;
      node.kind = Node::Kind::Map;
      skip_space();
      if (!at_end() && peek() == '}') {
        ++at_;
        return node;
      }
      for (;;) {
        skip_space();
        const std::string key = unquote(read_scalar_text(true), line_);
        skip_space();
        if (at_end() || peek() != ':') {
          throw ParseError(line_, "expected : after key " + key);
        }
        ++at_;
        if (node.find(key)) {
          throw ParseError(line_, "key given twice: " + key);
        }
        node.map.emplace_back(key, read_value());
        skip_space();
        if (at_end()) {
          throw ParseError(line_, "unclosed {");
        }
        if (peek() == ',') {
          ++at_;
          skip_space();
          if (!at_end() && peek() == '}') {
            ++at_;  // a trailing comma
            return node;
          }
          continue;
        }
        if (peek() == '}') {
          ++at_;
          return node;
        }
        throw ParseError(line_, std::string("expected , or } but found ") + peek());
      }
    }
    const std::string text = read_scalar_text(false);
    node.kind = Node::Kind::Scalar;
    node.scalar = unquote(text, line_);
    if (text == "~" || text == "null") {
      node.kind = Node::Kind::Null;
      node.scalar.clear();
    }
    return node;
  }

  bool at_end() const {
    return at_ >= text_.size();
  }
  void skip_space() {
    while (!at_end() && (peek() == ' ' || peek() == '\t')) {
      ++at_;
    }
  }

 private:
  char peek() const {
    return text_[at_];
  }
  // A scalar up to the next , ] } (and : when it is a flow map's key), quotes kept.
  std::string read_scalar_text(bool key) {
    skip_space();
    const std::size_t start = at_;
    if (!at_end() && (peek() == '"' || peek() == '\'')) {
      const char quote = peek();
      ++at_;
      while (!at_end()) {
        if (peek() == '\\' && quote == '"') {
          at_ += 2;
          continue;
        }
        if (peek() == quote) {
          if (quote == '\'' && at_ + 1 < text_.size() && text_[at_ + 1] == '\'') {
            at_ += 2;
            continue;
          }
          ++at_;
          return text_.substr(start, at_ - start);
        }
        ++at_;
      }
      throw ParseError(line_, "unclosed quote");
    }
    while (!at_end()) {
      const char c = peek();
      if (c == ',' || c == ']' || c == '}' || (key && c == ':')) {
        break;
      }
      ++at_;
    }
    return trim(text_.substr(start, at_ - start));
  }

  const std::string& text_;
  int line_;
  std::size_t at_ = 0;
};

// The position of a "key: value" separator in a block line ("key:" at the end counts), or npos.
inline std::size_t key_separator(const std::string& text) {
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '[' || c == '{') {
      return std::string::npos;  // a flow value, no key before it
    } else if (c == ':' && (i + 1 == text.size() || text[i + 1] == ' ')) {
      return i;
    }
  }
  return std::string::npos;
}

inline Node inline_value(const std::string& text, int line) {
  FlowReader reader(text, line);
  Node value = reader.read_value();
  reader.skip_space();
  if (!reader.at_end()) {
    throw ParseError(line, "unexpected text after the value: " + text);
  }
  return value;
}

class BlockReader {
 public:
  explicit BlockReader(std::vector<Line> lines) : lines_(std::move(lines)) {
  }

  Node read_document() {
    if (lines_.empty()) {
      return Node{};
    }
    Node root = read_block(lines_[0].indent);
    if (next_ < lines_.size()) {
      throw ParseError(lines_[next_].number, "unexpected indentation");
    }
    return root;
  }

 private:
  // The block whose lines start at `indent`: a list when its first line starts with "- ".
  Node read_block(int indent) {
    const Line& first = lines_[next_];
    if (first.text == "-" || first.text.rfind("- ", 0) == 0) {
      return read_list(indent);
    }
    if (key_separator(first.text) != std::string::npos) {
      return read_map(indent, std::nullopt);
    }
    ++next_;
    return inline_value(first.text, first.number);
  }

  Node read_list(int indent) {
    Node list;
    list.kind = Node::Kind::List;
    list.line = lines_[next_].number;
    while (next_ < lines_.size() && lines_[next_].indent == indent) {
      const Line line = lines_[next_];
      if (line.text != "-" && line.text.rfind("- ", 0) != 0) {
        break;
      }
      const std::string rest = line.text == "-" ? std::string() : trim(line.text.substr(2));
      if (rest.empty()) {
        ++next_;
        list.list.push_back(nested(indent, line.number));
        continue;
      }
      if (key_separator(rest) != std::string::npos) {
        // "- key: value" starts a map whose other keys sit under "key".
        const int item_indent = indent + int(line.text.size() - rest.size());
        Line first{line.number, item_indent, rest};
        list.list.push_back(read_map(item_indent, first));
        continue;
      }
      ++next_;
      list.list.push_back(inline_value(rest, line.number));
    }
    return list;
  }

  // A map at `indent`; `first`: its first entry when it came after a list's "- ".
  Node read_map(int indent, std::optional<Line> first) {
    Node map;
    map.kind = Node::Kind::Map;
    map.line = first ? first->number : lines_[next_].number;
    bool use_first = first.has_value();
    for (;;) {
      Line line;
      if (use_first) {
        line = *first;
        use_first = false;
      } else {
        if (next_ >= lines_.size() || lines_[next_].indent != indent) {
          break;
        }
        line = lines_[next_];
      }
      ++next_;
      const std::size_t colon = key_separator(line.text);
      if (colon == std::string::npos) {
        throw ParseError(line.number, "expected key: value, found " + line.text);
      }
      const std::string key = unquote(trim(line.text.substr(0, colon)), line.number);
      if (key.empty()) {
        throw ParseError(line.number, "empty key");
      }
      if (map.find(key)) {
        throw ParseError(line.number, "key given twice: " + key);
      }
      const std::string rest = trim(line.text.substr(colon + 1));
      if (!rest.empty()) {
        map.map.emplace_back(key, inline_value(rest, line.number));
      } else {
        map.map.emplace_back(key, nested(indent, line.number));
      }
    }
    if (next_ < lines_.size() && lines_[next_].indent > indent) {
      throw ParseError(lines_[next_].number, "unexpected indentation");
    }
    return map;
  }

  // The block under a key or "-" with nothing after it: deeper lines, or null. A list may sit at
  // its key's own indentation ("key:\n- a").
  Node nested(int indent, int line) {
    if (next_ < lines_.size()) {
      const Line& next = lines_[next_];
      const bool list_at_same = next.indent == indent && next.text.rfind("- ", 0) == 0;
      if (next.indent > indent || list_at_same) {
        return read_block(next.indent);
      }
    }
    Node null;
    null.line = line;
    return null;
  }

  std::vector<Line> lines_;
  std::size_t next_ = 0;
};

}  // namespace detail

// The tree of a scenario file's text; ParseError when it is not in the subset.
inline Node parse(const std::string& source) {
  return detail::BlockReader(detail::split_lines(source)).read_document();
}

}  // namespace kcd2s::local::bots::yaml
