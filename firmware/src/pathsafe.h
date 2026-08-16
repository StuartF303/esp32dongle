// usbdongle W3 — path sanitisation for the `storage` module.
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h: <stddef.h>/<stdint.h> and
// nothing else, header-only and inline, so `pio test -e native` can compile and
// assert it on this machine. The native env sets build_src_filter = -<*>, so a
// .cpp here would never be built for the host and the rule would go untested.
// Keep it that way: no Arduino.h, no ESP-IDF, no ArduinoJson.
//
// WHAT THIS DEFENDS AGAINST. `storage` accepts a path from a phone over Wi-Fi
// or BLE, and (per ARCHITECTURE.md §4) that client may be at AUTH_NONE until it
// presents a token. The SD card root is the sandbox boundary; a single
// unfiltered ".." turns a file browser into arbitrary read/write of whatever
// else is mounted on the VFS. So the check is a WHITELIST of shape, applied
// BEFORE the path is ever concatenated onto the mountpoint, and it rejects
// rather than normalises — a canonicaliser that silently rewrites "a/../b" into
// "b" is one bug away from rewriting it into something else.
//
// It is deliberately stricter than FAT itself:
//   * no '\' at all — FatFs accepts backslash as a separator, so allowing it
//     would give a second, unchecked traversal syntax;
//   * no ':' — FatFs treats "0:" / "N:" as a DRIVE PREFIX, which escapes the
//     mountpoint without using ".." at all;
//   * no trailing '.' or ' ' in a segment — silently stripped by some FAT
//     implementations, so "/secret." and "/secret" can name the same file and
//     an allow/deny list comparing strings would disagree with the filesystem.
//
// Bytes >= 0x80 ARE allowed: ESP-IDF's FatFs is built with UTF-8 API encoding,
// so non-ASCII filenames are legitimate. Encoding validity is FatFs's problem.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace PathSafe {

// Longest caller-visible path, in bytes, excluding the NUL and INCLUDING the
// leading '/'. 255 keeps "/sd" + path inside ESP-IDF's FILE_PATH_MAX
// (ESP_VFS_PATH_MAX + CONFIG_FATFS_MAX_LFN + 3) with room to spare, and is
// advertised to callers as caps.max_path so nobody has to guess.
constexpr size_t MAX_PATH_LEN = 255;

// Longest single path component. FatFs's LFN limit is 255; 200 leaves room for
// the mountpoint and a parent prefix without ever being the binding limit in
// practice.
constexpr size_t MAX_SEGMENT_LEN = 200;

enum Result : uint8_t {
  PATH_OK = 0,
  PATH_NULL,          // nullptr
  PATH_EMPTY,         // ""
  PATH_NOT_ABSOLUTE,  // no leading '/'
  PATH_TOO_LONG,      // > MAX_PATH_LEN bytes
  PATH_BACKSLASH,     // '\' anywhere — a second separator syntax
  PATH_CONTROL_CHAR,  // byte < 0x20 or == 0x7f
  PATH_RESERVED_CHAR, // one of * ? " < > | : — ':' is a FatFs drive prefix
  PATH_EMPTY_SEGMENT, // "//" or a trailing '/'
  PATH_DOT_SEGMENT,   // a "." component
  PATH_PARENT_SEGMENT,// a ".." component — the traversal case
  PATH_TRAILING_DOT,  // component ends in '.' (e.g. "..." or "name.")
  PATH_TRAILING_SPACE,// component ends in ' '
  PATH_SEGMENT_TOO_LONG,
};

// Characters FAT/exFAT reserve, plus ':' for the FatFs drive prefix. '\' and
// '/' are handled separately so they get their own, more specific results.
inline bool isReservedChar(char c) {
  return c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':';
}

// THE check. Pure, allocation-free, bounded by maxLen — it never walks past
// maxLen+1 bytes, so a caller-supplied buffer that is not NUL-terminated within
// the limit is reported as PATH_TOO_LONG rather than read off the end.
//
// Returns PATH_OK only for a path that is absolute, non-empty, within length,
// built entirely of non-empty components that are not "." or "..", and free of
// control characters, backslashes, reserved characters and trailing dots or
// spaces. "/" (the card root) is PATH_OK.
inline Result check(const char *p, size_t maxLen = MAX_PATH_LEN) {
  if (p == nullptr) {
    return PATH_NULL;
  }
  if (p[0] == '\0') {
    return PATH_EMPTY;
  }
  if (p[0] != '/') {
    return PATH_NOT_ABSOLUTE;
  }

  // Bounded length scan FIRST: everything below indexes into the string, and
  // this is what guarantees those indexes stay inside it.
  size_t len = 0;
  while (len <= maxLen && p[len] != '\0') {
    len++;
  }
  // Test `len`, NOT `p[len]`. The obvious-looking `if (p[len] != '\0')` has an
  // off-by-one: a path of exactly maxLen+1 bytes leaves the loop with
  // len == maxLen+1 pointing AT its NUL, so the string reads as
  // terminated-in-range and the check passes. Found by test_length_bounds
  // 2026-08-16 — a 256-byte path was accepted against a 255-byte limit, and
  // only the segment limit had been masking it. That matters: callers size
  // buffers on MAX_PATH_LEN and then prefix the mountpoint.
  if (len > maxLen) {
    return PATH_TOO_LONG;
  }

  // Byte-level pass, over the whole string, before any segment shape is
  // considered. Doing it in one pass up front makes the reported reason
  // deterministic: "/a\\..\\b" is BACKSLASH, never PARENT_SEGMENT, regardless of
  // how the segment walk below happens to split it.
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '\\') {
      return PATH_BACKSLASH;
    }
    if (c < 0x20 || c == 0x7f) {
      return PATH_CONTROL_CHAR;
    }
    if (isReservedChar((char)c)) {
      return PATH_RESERVED_CHAR;
    }
  }

  if (len == 1) {
    return PATH_OK;  // "/" — the card root
  }

  // Segment walk. Segments are the runs between '/' after the leading one, so
  // a trailing '/' yields a final EMPTY segment and is rejected — the module
  // wants exactly one spelling of every path, and "/a" vs "/a/" being both
  // accepted is how two callers end up disagreeing about identity.
  size_t segStart = 1;
  for (size_t i = 1; i <= len; i++) {
    if (p[i] != '/' && p[i] != '\0') {
      continue;
    }
    size_t segLen = i - segStart;
    if (segLen == 0) {
      return PATH_EMPTY_SEGMENT;
    }
    if (segLen > MAX_SEGMENT_LEN) {
      return PATH_SEGMENT_TOO_LONG;
    }
    if (segLen == 1 && p[segStart] == '.') {
      return PATH_DOT_SEGMENT;
    }
    if (segLen == 2 && p[segStart] == '.' && p[segStart + 1] == '.') {
      return PATH_PARENT_SEGMENT;
    }
    char last = p[i - 1];
    if (last == '.') {
      return PATH_TRAILING_DOT;  // catches "..." and "name." alike
    }
    if (last == ' ') {
      return PATH_TRAILING_SPACE;
    }
    segStart = i + 1;
  }
  return PATH_OK;
}

// Short, stable, wire-visible token. Goes into the error response so a caller
// can branch on the reason without parsing prose.
inline const char *resultName(Result r) {
  switch (r) {
    case PATH_OK:
      return "ok";
    case PATH_NULL:
      return "null";
    case PATH_EMPTY:
      return "empty";
    case PATH_NOT_ABSOLUTE:
      return "not_absolute";
    case PATH_TOO_LONG:
      return "too_long";
    case PATH_BACKSLASH:
      return "backslash";
    case PATH_CONTROL_CHAR:
      return "control_char";
    case PATH_RESERVED_CHAR:
      return "reserved_char";
    case PATH_EMPTY_SEGMENT:
      return "empty_segment";
    case PATH_DOT_SEGMENT:
      return "dot_segment";
    case PATH_PARENT_SEGMENT:
      return "parent_segment";
    case PATH_TRAILING_DOT:
      return "trailing_dot";
    case PATH_TRAILING_SPACE:
      return "trailing_space";
    case PATH_SEGMENT_TOO_LONG:
      return "segment_too_long";
    default:
      return "?";
  }
}

// One line, actionable, for a human. Never a bare "invalid path".
inline const char *resultMessage(Result r) {
  switch (r) {
    case PATH_OK:
      return "ok";
    case PATH_NULL:
      return "missing p.path";
    case PATH_EMPTY:
      return "p.path is empty; use \"/\" for the card root";
    case PATH_NOT_ABSOLUTE:
      return "p.path must start with '/' (it is relative to the card root, not to any working directory)";
    case PATH_TOO_LONG:
      return "p.path is longer than the 255-byte limit (see the storage caps action)";
    case PATH_BACKSLASH:
      return "p.path contains a backslash; use '/' as the only separator";
    case PATH_CONTROL_CHAR:
      return "p.path contains a control character (byte < 0x20 or 0x7f)";
    case PATH_RESERVED_CHAR:
      return "p.path contains a reserved character (one of * ? \" < > | :)";
    case PATH_EMPTY_SEGMENT:
      return "p.path has an empty component (a '//' or a trailing '/'); write it exactly once, with no trailing slash";
    case PATH_DOT_SEGMENT:
      return "p.path contains a '.' component; paths are not normalised, write the path out in full";
    case PATH_PARENT_SEGMENT:
      return "p.path contains a '..' component; paths cannot escape the card root";
    case PATH_TRAILING_DOT:
      return "a component of p.path ends in '.'; FAT strips those, so the name would be ambiguous";
    case PATH_TRAILING_SPACE:
      return "a component of p.path ends in a space; FAT strips those, so the name would be ambiguous";
    case PATH_SEGMENT_TOO_LONG:
      return "a component of p.path is longer than 200 bytes";
    default:
      return "p.path is not acceptable";
  }
}

}  // namespace PathSafe
