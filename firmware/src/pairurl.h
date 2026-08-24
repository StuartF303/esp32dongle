// usbdongle W3 — "is this request target the pair URL?", and nothing else.
//
// One predicate, lifted out of mod_http.cpp's handleWildcard() so it can be
// asserted on this machine:
//
//     request target -> the path span inside it -> is that span /dddd ?
//
// ---- WHY THIS IS THE MOST IMPORTANT PREDICATE IN THAT FILE --------------
//
// The block comment above handleWildcard() states the property at length and
// it is worth restating in the file that now owns half of it: EVERY PATH OF
// EXACTLY AuthFmt::PIN_LEN DIGITS GETS THE IDENTICAL RESPONSE. /0000 and /4821
// are byte-for-byte the same page. The route is not a PIN check, does not
// compare against pin_, does not touch the rate limiter, and creates no
// session — because a route that answered 200 for the right PIN and 404 for
// the wrong one is a PIN oracle that walks 10^4 in under a minute with plain
// GETs, bypassing the limiter that is the entire reason 4 digits is
// defensible (authfmt.h, ratelimit.h).
//
// So the thing that has to be true is a SHAPE test with no memory: it must say
// yes to all 10^4 digit strings and no to everything else. That is exactly the
// kind of claim a test can settle exhaustively, and until this header existed
// nothing in the repository asserted it — `pio test -e native` sets
// build_src_filter = -<*>, so mod_http.cpp is excluded from the host env by
// construction, and this machine has no 802.11 PHY, so the softAP listener the
// handler is registered on is unreachable here too. Same argument, same
// remedy, as pinpolicy.h / apgrace.h / qrfit.h / cmdauth.h.
//
// ---- THE OTHER HALF: WHAT COUNTS AS "THE PATH" -------------------------
//
// handleWildcard() used to do `strcspn(req->uri, "?")` over the whole request
// target and claim in a comment that this was "the same basis" the router
// matched on. IT WAS NOT, and the difference is two real inputs. Verified
// against the pinned SDK — ESP-IDF v5.5.5, components/esp_http_server/src/
// httpd_uri.c, and cross-checked by disassembling the shipped
// libesp_http_server.a because the framework ships only the built library:
//
//     if (res->field_set & (1 << UF_PATH)) {
//         uri = httpd_find_uri_handler(hd, req->uri + res->field_data[UF_PATH].off,
//                                      res->field_data[UF_PATH].len, req->method, &err);
//     }
//
// (In the disassembly: `l16ui a9, a8, 16` and `l16ui a12, a8, 18` load
// field_data[UF_PATH].off and .len out of hd->hd_req_aux.url_parse_res, and
// `add.n a11, a11, a9` adds the offset to &req->uri before the call. The
// POINTER is moved as well as the length clamped.)
//
// So the matcher is given the http_parser UF_PATH field — offset AND length —
// while the handler is given `req->uri`, which is the target exactly as it
// arrived. They differ for:
//
//   * ABSOLUTE-FORM. `GET http://192.168.4.1/4821 HTTP/1.1` is a request a
//     server MUST accept (RFC 7230 §5.3.2) and any client MAY send. UF_PATH
//     is `/4821` at offset 18, so `/*` matches on the offset pointer and the
//     handler IS reached — with a req->uri whose first byte is 'h'. The old
//     code compared from byte 0 and fell through to the 404.
//   * A FRAGMENT. `GET /4821#x` — clients must not send one, but nothing stops
//     a hostile or naive one. http_parser splits UF_PATH `/4821` from UF_FRAGMENT
//     `x`, so again the route matches and the handler saw all seven bytes.
//
// Both failed CLOSED (a 404), so this was correctness and an honest comment,
// not a hole. It is fixed here rather than papered over, because "the path"
// having one meaning in the router and another in the handler is the shape of
// a bug that is harmless today and is not harmless after the next edit.
//
// WHY NOT JUST READ httpd's PARSE RESULT? Because it is not reachable. It
// lives in `struct httpd_req_aux`, declared in the component's private
// esp_httpd_priv.h, which is not on the include path — the public
// httpd_req_t exposes only `uri`. Reparsing the two forms here is the
// available option, and it has the side benefit of being testable, which
// reaching into a private struct would not have been.
//
// ---- DELIBERATELY NOT A URL PARSER --------------------------------------
//
// pathOf() handles origin-form, absolute-form, the query string and the
// fragment. It does NOT decode percent-escapes, and that is a decision, not an
// omission: `/%34%38%32%31` is not the pair URL, and making it one would mean
// this predicate had a second spelling for every path — i.e. exactly the
// "does it look like X" ambiguity that pathsafe.h exists to refuse elsewhere.
// esp_http_server does not decode the path before matching either, so a
// decoding step here would ALSO reintroduce the mismatch this file was written
// to remove. A browser given `HTTP://192.168.4.1/4821` sends it literally.
//
// ---- DEPENDENCY-FREE, SAME RULE AS ITS NEIGHBOURS ----------------------
//
// <stddef.h> and authfmt.h (itself dependency-free) and nothing else. Every
// function is constexpr so mod_http.cpp can static_assert over its own probe
// table with them — see the noProbeIsPairShaped() assert there, which is what
// makes "no probe path is PIN-shaped, so the two lookups cannot collide" a
// compile error rather than a sentence.

#pragma once

#include <stddef.h>

#include "authfmt.h"

namespace PairUrl {

// A span into the caller's buffer. NOT NUL-terminated and never copied: the
// whole point is to look at the request target where it already is, without a
// second buffer that could be sized wrong.
struct Path {
  const char *at;
  size_t len;
};

// strlen, but usable in a constant expression. Named apart from the C one so
// it is obvious at the call site that this is compile-time work.
constexpr size_t litLen(const char *s) {
  size_t n = 0;
  if (s != nullptr) {
    while (s[n] != '\0') {
      n++;
    }
  }
  return n;
}

namespace detail {

// RFC 3986 §3.1 scheme character set, minus the first character's rule.
constexpr bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr bool isSchemeChar(char c) { return isAlpha(c) || isDigit(c) || c == '+' || c == '-' || c == '.'; }

// Bytes to skip to get past an absolute-form target's `scheme://authority`,
// or 0 when there is no such prefix.
//
// Conservative on purpose: the prefix is only recognised when it is a
// well-formed scheme (ALPHA followed by scheme characters) immediately
// followed by "://". An origin-form target starts with '/', which is not a
// scheme character, so this returns 0 on the first byte for every ordinary
// request and costs nothing.
constexpr size_t skipAuthority(const char *t) {
  if (t == nullptr || !isAlpha(t[0])) {
    return 0;
  }
  size_t i = 1;
  while (isSchemeChar(t[i])) {
    i++;
  }
  if (t[i] != ':' || t[i + 1] != '/' || t[i + 2] != '/') {
    return 0;
  }
  i += 3;  // past "://"
  // The authority runs to the first '/', '?' or '#', per RFC 3986 §3.2. A
  // target with no path at all ("http://192.168.4.1") lands on the NUL and
  // yields an empty path, which matches no route — which is what the router
  // does with it too, since http_parser leaves UF_PATH unset.
  while (t[i] != '\0' && t[i] != '/' && t[i] != '?' && t[i] != '#') {
    i++;
  }
  return i;
}

}  // namespace detail

// The path span inside a request target, on the same basis the router matched
// on: absolute-form authority stripped, query string and fragment excluded.
constexpr Path pathOf(const char *target) {
  if (target == nullptr) {
    return Path{nullptr, 0};
  }
  const char *at = target + detail::skipAuthority(target);
  size_t n = 0;
  while (at[n] != '\0' && at[n] != '?' && at[n] != '#') {
    n++;
  }
  return Path{at, n};
}

// Whether a path span is byte-for-byte a literal. The literal's length is part
// of the comparison, so `/generate_204x` cannot match `/generate_204` — the
// same length-then-compare rule httpd_uri_match_simple() applies, restated
// here because this lookup does not go through the router.
constexpr bool equals(const Path &p, const char *literal) {
  if (p.at == nullptr || literal == nullptr) {
    return false;
  }
  if (litLen(literal) != p.len) {
    return false;
  }
  for (size_t i = 0; i < p.len; i++) {
    if (p.at[i] != literal[i]) {
      return false;
    }
  }
  return true;
}

// THE PREDICATE. True for exactly the 10^4 paths `/0000` .. `/9999`, and for
// nothing else.
//
// `len` is PIN_LEN + 1 because it counts the leading slash, so `/48211` and
// `/4821/x` are both false: a path that merely STARTS with the right digits is
// not a pair URL. Nothing here looks at the current PIN — see the header
// comment for why that is the entire point.
constexpr bool isPairPath(const char *path, size_t len) {
  if (path == nullptr || len != AuthFmt::PIN_LEN + 1 || path[0] != '/') {
    return false;
  }
  for (size_t i = 1; i < len; i++) {
    if (path[i] < '0' || path[i] > '9') {
      return false;
    }
  }
  return true;
}

constexpr bool isPairPath(const Path &p) { return isPairPath(p.at, p.len); }

// The shape of the pair URL is a published format, like AuthFmt::PIN_LEN
// itself; the VALUE in it is the secret. These two exist so a producer sizing
// a buffer for "HTTP://192.168.4.1/dddd" does not count characters by hand.
constexpr size_t PATH_LEN = AuthFmt::PIN_LEN + 1;
static_assert(PATH_LEN == 5, "the pair path is no longer 5 bytes; ARCHITECTURE.md's version-1 QR table moves with it");

}  // namespace PairUrl
