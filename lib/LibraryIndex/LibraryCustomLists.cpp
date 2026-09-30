#include "LibraryCustomLists.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <cstring>
#include <string>

#include "LibraryText.h"

namespace library {
namespace {

constexpr uint8_t MAX_DEPTH = 16;
constexpr size_t MAX_STRING_BYTES = 255;

// Just enough JSON for the custom lists file, read through a small buffer.
class JsonStream {
 public:
  explicit JsonStream(HalFile& file) : file(file) {}

  int peek() {
    if (pos == len) {
      if (eof) return -1;
      const int got = file.read(buf, sizeof(buf));
      pos = 0;
      len = got > 0 ? got : 0;
      if (got < 0) readFailed = true;
      if (len == 0) {
        eof = true;
        return -1;
      }
    }
    return static_cast<unsigned char>(buf[pos]);
  }
  int get() {
    const int c = peek();
    if (c >= 0) pos++;
    return c;
  }
  // The next non-space character, not consumed.
  int next() {
    int c = peek();
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pos++;
      c = peek();
    }
    return c;
  }
  bool expect(const char c) { return next() == c && get() == c; }

  // A string, into `out` when given, cut at a UTF-8 boundary to fit a label.
  bool string(std::string* out) {
    if (out) out->clear();
    if (!expect('"')) return false;
    for (;;) {
      int c = get();
      if (c < 0) return false;
      if (c == '"') break;
      if (c != '\\') {
        append(out, static_cast<char>(c));
        continue;
      }
      c = get();
      uint32_t cp = 0;
      switch (c) {
        case '"':
        case '\\':
        case '/':
          append(out, static_cast<char>(c));
          continue;
        case 'b':
          append(out, '\b');
          continue;
        case 'f':
          append(out, '\f');
          continue;
        case 'n':
          append(out, '\n');
          continue;
        case 'r':
          append(out, '\r');
          continue;
        case 't':
          append(out, '\t');
          continue;
        case 'u':
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            uint32_t low = 0;
            if (get() != '\\' || get() != 'u' || !hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          if (out && out->size() < MAX_STRING_BYTES + 4) utf8AppendCodepoint(cp, *out);
          continue;
        default:
          return false;
      }
    }
    if (out && out->size() > MAX_STRING_BYTES) {
      out->resize(static_cast<size_t>(utf8SafeTruncateBuffer(out->data(), static_cast<int>(MAX_STRING_BYTES))));
    }
    return true;
  }

  bool skipValue(const uint8_t depth) {
    if (depth > MAX_DEPTH) return false;
    const int c = next();
    if (c == '"') return string(nullptr);
    if (c == '{' || c == '[') {
      const char close = c == '{' ? '}' : ']';
      get();
      if (next() == close) return get() == close;
      for (;;) {
        if (c == '{' && (!string(nullptr) || !expect(':'))) return false;
        if (!skipValue(depth + 1)) return false;
        const int after = next();
        get();
        if (after == close) return true;
        if (after != ',') return false;
      }
    }
    // A number, true, false or null.
    int taken = 0;
    for (int d = peek();
         (d >= '0' && d <= '9') || (d >= 'a' && d <= 'z') || d == '-' || d == '+' || d == '.' || d == 'E'; d = peek()) {
      get();
      taken++;
    }
    return taken > 0;
  }

  bool readFailed = false;

 private:
  static void append(std::string* out, const char c) {
    if (out && out->size() < MAX_STRING_BYTES + 4) out->push_back(c);
  }
  bool hex4(uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; i++) {
      const int c = get();
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    return true;
  }

  HalFile& file;
  char buf[128];
  int len = 0;
  int pos = 0;
  bool eof = false;
};

// Copy `text` into the label pool; false (and the pool unchanged) when full.
bool addLabel(CustomLists& out, const std::string& text, uint16_t& off, uint8_t& len) {
  if (out.labelBytes + text.size() > MAX_CUSTOM_LABEL_BYTES) return false;
  off = out.labelBytes;
  len = static_cast<uint8_t>(text.size());
  memcpy(out.labels.get() + off, text.data(), text.size());
  out.labelBytes = static_cast<uint16_t>(out.labelBytes + text.size());
  return true;
}

// The value of the member labelled `label`: a tag, or an object of sublists. A
// value of another shape is skipped.
bool parseEntry(JsonStream& json, CustomLists& out, const std::string& label, std::string& key, std::string& value) {
  const int c = json.next();
  if (c != '"' && c != '{') return json.skipValue(1);

  const bool room = out.listCount < MAX_CUSTOM_LISTS;
  CustomLists::List list{0, 0, out.subCount, 0, c == '"'};
  const bool listed = room && !label.empty() && addLabel(out, label, list.labelOff, list.labelLen);
  if (!room || (!label.empty() && !listed)) out.truncated = true;

  // A sublist, unlabelled for a direct tag.
  const auto addSub = [&](const std::string& subLabel) {
    if (!listed || fold(value).empty()) return;
    CustomLists::Sub sub{foldedTagHash(value), 0, 0};
    if (out.subCount < MAX_CUSTOM_SUBLISTS &&
        (subLabel.empty() || addLabel(out, subLabel, sub.labelOff, sub.labelLen))) {
      out.subs[out.subCount++] = sub;
      list.subCount++;
    } else {
      out.truncated = true;
    }
  };

  if (list.direct) {
    if (!json.string(&value)) return false;
    key.clear();
    addSub(key);
  } else {
    json.get();
    if (json.next() == '}') {
      json.get();
    } else {
      for (;;) {
        if (!json.string(&key) || !json.expect(':')) return false;
        if (json.next() == '"') {
          if (!json.string(&value)) return false;
          if (!key.empty()) addSub(key);
        } else if (!json.skipValue(2)) {
          return false;
        }
        const int after = json.next();
        json.get();
        if (after == '}') break;
        if (after != ',') return false;
      }
    }
  }
  if (listed && list.subCount > 0) out.lists[out.listCount++] = list;
  return true;
}

}  // namespace

uint32_t foldedTagHash(const std::string_view tag) {
  const std::string folded = fold(tag);
  uint32_t hash = 2166136261u;  // FNV-1a 32
  for (const char c : folded) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return hash;
}

CustomListsResult loadCustomLists(const char* path, CustomLists& out) {
  out = CustomLists{};
  if (!Storage.exists(path)) return CustomListsResult::Missing;
  HalFile file;
  if (!Storage.openFileForRead("LIBCUST", path, file)) return CustomListsResult::ReadError;
  out.labels = makeUniqueNoThrow<char[]>(MAX_CUSTOM_LABEL_BYTES);
  out.lists = makeUniqueNoThrow<CustomLists::List[]>(MAX_CUSTOM_LISTS);
  out.subs = makeUniqueNoThrow<CustomLists::Sub[]>(MAX_CUSTOM_SUBLISTS);
  if (!out.labels || !out.lists || !out.subs) {
    LOG_ERR("LIBCUST", "OOM: custom lists tables");
    out = CustomLists{};
    return CustomListsResult::OutOfMemory;
  }

  JsonStream json(file);
  std::string label;
  std::string key;
  std::string value;
  bool ok = json.expect('{');
  if (ok && json.next() == '}') {
    json.get();
  } else {
    while (ok) {
      ok = json.string(&label) && json.expect(':') && parseEntry(json, out, label, key, value);
      if (!ok) break;
      const int after = json.next();
      json.get();
      if (after == '}') break;
      ok = after == ',';
    }
  }
  if (json.readFailed) {
    LOG_ERR("LIBCUST", "cannot read %s", path);
    out = CustomLists{};
    return CustomListsResult::ReadError;
  }
  if (!ok) {
    LOG_ERR("LIBCUST", "%s is not valid custom lists JSON", path);
    out = CustomLists{};
    return CustomListsResult::Malformed;
  }
  if (out.truncated) LOG_INF("LIBCUST", "custom lists past the limits were left out");
  return CustomListsResult::Ok;
}

uint32_t customListsHash(const char* path) {
  if (!Storage.exists(path)) return 0;
  HalFile file;
  if (!Storage.openFileForRead("LIBCUST", path, file)) return 0;
  uint32_t hash = 2166136261u;  // FNV-1a 32
  uint8_t buf[128];
  for (int got = file.read(buf, sizeof(buf)); got > 0; got = file.read(buf, sizeof(buf))) {
    for (int i = 0; i < got; i++) {
      hash ^= buf[i];
      hash *= 16777619u;
    }
  }
  return hash != 0 ? hash : 1;
}

}  // namespace library
