// usbdongle W3 — volume prefixes for the `storage` module (backlog F5).
//
// DEPENDENCY-FREE ON PURPOSE, exactly like claims.h / pathsafe.h / cmdauth.h:
// <stddef.h>, <stdint.h> and pathsafe.h (itself dependency-free) and nothing
// else, header-only and inline, so `pio test -e native` can compile and assert
// it on this machine. The native env sets build_src_filter = -<*>, so a .cpp
// here would never be built for the host and the rule would go untested.
//
// ---- WHAT THIS IS FOR ---------------------------------------------------
//
// `storage` now addresses TWO filesystems — the microSD card and the LittleFS
// partition at 0x820000 — through one chunked read/write/CRC/list/stat/mkdir/
// delete/verify surface. A caller therefore has to say WHICH, and the cheapest
// way that does not fork the API is a volume prefix on the path:
//
//     /sd/logs/run.txt        the microSD card
//     /fs/www/index.html.gz   the LittleFS partition
//     /sd                     the card's root
//     /fs                     the LittleFS root
//
// ---- HOW IT INTERACTS WITH PathSafe -------------------------------------
//
// It sits STRICTLY IN FRONT of it, and PathSafe is unchanged.
//
//   split()  peels the "/<volume>" prefix off and hands back `rest`, which is
//            the path WITHIN that volume and always begins with '/'.
//   PathSafe::check(rest)  is then the same single funnel it has always been —
//            one implementation, one set of rules, one place traversal is
//            refused. This header deliberately does NOT re-implement any of
//            it: it validates the VOLUME NAME and nothing else.
//
// The volume root is the one case that needs care, because PathSafe's contract
// is "a path starts with '/' and never ends with one". So for "/sd" this
// returns rest = "/" (the literal, not a pointer into the input) and sets
// isRoot — PathSafe accepts "/" as the root, and the module composes the VFS
// path as prefix + "" rather than prefix + "/".
//
// ---- EXACTLY ONE SPELLING PER PATH --------------------------------------
//
// The same rule PathSafe enforces below the prefix, enforced here above it:
//
//   "/sd"        the volume root                          ACCEPTED
//   "/sd/"       the same directory, spelled differently  REJECTED (trailing_slash)
//   "/sd/a"      a file                                   ACCEPTED
//   "/sd/a/"     the same file, spelled differently       REJECTED by PathSafe
//   "/SD/a"      the same volume, spelled differently     REJECTED (name_bad_char)
//
// Two callers that cannot agree on how to spell a path cannot agree on whether
// they are looking at the same file, which is how a resumed upload writes to
// one path and a verify hashes another.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pathsafe.h"

namespace VolPath {

// Longest volume name, excluding the leading '/'. Two exist ("sd", "fs"); 8 is
// room for a third or fourth without being large enough to be interesting to
// something trying to smuggle a path through the name.
constexpr size_t MAX_NAME_LEN = 8;

// "/" + name. The module sizes its VFS path buffer on this.
constexpr size_t MAX_PREFIX_LEN = 1 + MAX_NAME_LEN;

// Longest caller-visible path: the prefix plus the longest path PathSafe will
// accept inside it. PathSafe applies its own 255-byte limit to `rest`, so this
// is only the outer bound on the bytes this function is willing to walk.
constexpr size_t MAX_TOTAL_LEN = MAX_PREFIX_LEN + PathSafe::MAX_PATH_LEN;

enum Result : uint8_t {
  VOLPATH_OK = 0,
  VOLPATH_NULL,            // nullptr
  VOLPATH_EMPTY,           // ""
  VOLPATH_NOT_ABSOLUTE,    // no leading '/'
  VOLPATH_TOO_LONG,        // > MAX_TOTAL_LEN bytes
  VOLPATH_NO_VOLUME,       // "/" — names no volume at all
  VOLPATH_EMPTY_VOLUME,    // "//x" — an empty volume name
  VOLPATH_NAME_TOO_LONG,   // "/averylongvolumename/x"
  VOLPATH_NAME_BAD_CHAR,   // "/SD/x", "/../x", "/s d/x" — not [a-z0-9]
  VOLPATH_TRAILING_SLASH,  // "/sd/" — a second spelling of "/sd"
};

struct Split {
  char volume[MAX_NAME_LEN + 1];  // NUL-terminated copy of the name, no '/'
  const char *rest;               // path WITHIN the volume; always starts with '/'
  bool isRoot;                    // true == rest is the literal "/" and the input named the volume root
  size_t prefixLen;               // bytes of the input the prefix occupied ("/sd" -> 3)
};

// THE split. Pure, allocation-free, bounded by maxLen — it never walks past
// maxLen + 1 bytes, so a caller-supplied buffer that is not NUL-terminated
// within the limit is reported as VOLPATH_TOO_LONG rather than read off the
// end. (The same off-by-one PathSafe::check() documents applies here and is
// avoided the same way: test `len`, never `p[len]`.)
//
// On VOLPATH_OK, `out->rest` either points INTO `p` or is the static literal
// "/" — so it is valid for exactly as long as `p` is. The caller must run
// PathSafe::check(out->rest) next; this function does not, deliberately, so
// that there is exactly one path checker in the image and it is the one with
// the adversarial test suite behind it.
inline Result split(const char *p, Split *out, size_t maxLen = MAX_TOTAL_LEN) {
  if (out != nullptr) {
    out->volume[0] = '\0';
    out->rest = "";
    out->isRoot = false;
    out->prefixLen = 0;
  }
  if (p == nullptr) {
    return VOLPATH_NULL;
  }
  if (p[0] == '\0') {
    return VOLPATH_EMPTY;
  }
  if (p[0] != '/') {
    return VOLPATH_NOT_ABSOLUTE;
  }

  // Bounded length scan FIRST: everything below indexes into the string, and
  // this is what guarantees those indexes stay inside it.
  size_t len = 0;
  while (len <= maxLen && p[len] != '\0') {
    len++;
  }
  if (len > maxLen) {
    return VOLPATH_TOO_LONG;
  }
  if (len == 1) {
    return VOLPATH_NO_VOLUME;  // "/" on its own names no volume
  }

  // The name runs from index 1 to the next '/' or the end.
  size_t i = 1;
  while (i < len && p[i] != '/') {
    i++;
  }
  size_t nameLen = i - 1;
  if (nameLen == 0) {
    return VOLPATH_EMPTY_VOLUME;  // "//..."
  }
  if (nameLen > MAX_NAME_LEN) {
    return VOLPATH_NAME_TOO_LONG;
  }
  // Lowercase alphanumeric ONLY. That is far stricter than PathSafe is about a
  // segment, and deliberately: this is a fixed, tiny vocabulary chosen by the
  // firmware, not user data. It also disposes of "/../etc" (name "..") and
  // "/SD/x" (a second spelling) without either reaching a comparison.
  for (size_t j = 1; j < i; j++) {
    char c = p[j];
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (!ok) {
      return VOLPATH_NAME_BAD_CHAR;
    }
  }

  if (out != nullptr) {
    for (size_t j = 0; j < nameLen; j++) {
      out->volume[j] = p[1 + j];
    }
    out->volume[nameLen] = '\0';
    out->prefixLen = i;
  }

  if (i == len) {
    // "/sd" — the volume root. rest is the literal "/", which PathSafe accepts
    // and which the module maps to "the mountpoint itself".
    if (out != nullptr) {
      out->rest = "/";
      out->isRoot = true;
    }
    return VOLPATH_OK;
  }

  if (i + 1 == len) {
    // "/sd/" — p[i] is '/' and it is the last byte. That is the volume root
    // again, under a second name. One spelling only.
    return VOLPATH_TRAILING_SLASH;
  }

  if (out != nullptr) {
    out->rest = p + i;  // starts at the '/', so PathSafe sees an absolute path
    out->isRoot = false;
  }
  return VOLPATH_OK;
}

// Short, stable, wire-visible token. Goes into the error response so a caller
// can branch on the reason without parsing prose. Deliberately in the same
// namespace of tokens PathSafe::resultName() produces — a caller reading
// d.path_error gets one vocabulary whichever half rejected it.
inline const char *resultName(Result r) {
  switch (r) {
    case VOLPATH_OK:
      return "ok";
    case VOLPATH_NULL:
      return "null";
    case VOLPATH_EMPTY:
      return "empty";
    case VOLPATH_NOT_ABSOLUTE:
      return "not_absolute";
    case VOLPATH_TOO_LONG:
      return "too_long";
    case VOLPATH_NO_VOLUME:
      return "no_volume";
    case VOLPATH_EMPTY_VOLUME:
      return "empty_volume";
    case VOLPATH_NAME_TOO_LONG:
      return "volume_too_long";
    case VOLPATH_NAME_BAD_CHAR:
      return "volume_bad_char";
    case VOLPATH_TRAILING_SLASH:
      return "trailing_slash";
    default:
      return "?";
  }
}

// One line, actionable, for a human. Never a bare "invalid path".
inline const char *resultMessage(Result r) {
  switch (r) {
    case VOLPATH_OK:
      return "ok";
    case VOLPATH_NULL:
      return "missing p.path";
    case VOLPATH_EMPTY:
      return "p.path is empty; every path starts with a volume, e.g. \"/sd\" or \"/fs\"";
    case VOLPATH_NOT_ABSOLUTE:
      return "p.path must start with '/' followed by a volume name, e.g. \"/sd/logs/run.txt\"";
    case VOLPATH_TOO_LONG:
      return "p.path is longer than the 264-byte limit (a volume prefix plus a 255-byte path)";
    case VOLPATH_NO_VOLUME:
      return "p.path is \"/\", which names no volume; use \"/sd\" for the card or \"/fs\" for the LittleFS partition";
    case VOLPATH_EMPTY_VOLUME:
      return "p.path starts with '//', so the volume name is empty; write \"/sd/...\" or \"/fs/...\"";
    case VOLPATH_NAME_TOO_LONG:
      return "the volume name in p.path is longer than 8 characters";
    case VOLPATH_NAME_BAD_CHAR:
      return "the volume name in p.path must be lowercase letters and digits only (\"sd\", \"fs\")";
    case VOLPATH_TRAILING_SLASH:
      return "p.path ends in '/'; the volume root is spelled \"/sd\", not \"/sd/\" — one spelling per path";
    default:
      return "p.path is not acceptable";
  }
}

}  // namespace VolPath
