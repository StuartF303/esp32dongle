// Host-side unit tests for the pair-URL predicate (pairurl.h).
//
// `pio test -e native`. This suite exists because the property it covers is the
// most important one in the commit that introduced the `/<pin>` route, and
// nothing in the repository asserted it:
//
//     EVERY PATH OF EXACTLY AuthFmt::PIN_LEN DIGITS GETS THE IDENTICAL
//     RESPONSE, AND NOTHING ELSE REACHES THAT HANDLER.
//
// It could not be asserted where it lived. handleWildcard() is in
// mod_http.cpp, the native env sets build_src_filter = -<*> so that file is
// excluded from this machine by construction, and the handler is registered on
// the softAP listener — this machine has no 802.11 PHY (/sys/class/ieee80211
// does not exist), so there is no way to reach it over the air either. Lifting
// the predicate into a dependency-free header is the only way it gets coverage
// at all, which is the same argument pinpolicy.h, apgrace.h and qrfit.h were
// extracted on.
//
// WHAT A FAILURE HERE WOULD MEAN. If isPairPath() ever answers differently for
// two PIN-shaped paths, the route becomes a PIN ORACLE: an attacker on the AP
// walks /0000../9999 with plain GETs and the one that answers differently is
// the PIN, obtained without spending a single rate-limited attempt. So the
// digit space is walked EXHAUSTIVELY below rather than sampled — 10,000
// iterations is nothing on a host and a sampled test would not be the same
// claim.
//
// The near-miss shapes are the other half: /482, /48211, /4821/x, /482a and
// /4821?x=1 must all be false (or, for the query, true only because the query
// is not part of the path), because a route that answers the page for anything
// BUT a bare PIN-shaped path is a route whose behaviour the comment above
// handleWildcard() no longer describes.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include "pairurl.h"

void setUp() {}
void tearDown() {}

namespace {

// Convenience: the whole decision for a raw request target, exactly as
// handleWildcard() now performs it.
bool servesPage(const char *target) { return PairUrl::isPairPath(PairUrl::pathOf(target)); }

// The six captive-probe paths, copied from mod_http.cpp's PROBES[]. They are
// duplicated here ON PURPOSE and it is not the assertion that matters — the
// real guard against a probe path colliding with the PIN shape is the
// static_assert over the live table in mod_http.cpp, which cannot drift because
// it reads the table itself. This list is here so that the collision property
// is also visible in the place someone reads to understand the route.
const char *PROBE_PATHS[] = {
    "/hotspot-detect.html", "/library/test/success.html", "/generate_204", "/gen_204", "/ncsi.txt", "/connecttest.txt",
};

}  // namespace

// ---- the exhaustive property --------------------------------------------

void test_every_pin_shaped_path_is_accepted_all_ten_thousand() {
  char path[PairUrl::PATH_LEN + 1];
  for (int n = 0; n < 10000; n++) {
    snprintf(path, sizeof(path), "/%04d", n);
    TEST_ASSERT_EQUAL_size_t(PairUrl::PATH_LEN, strlen(path));
    if (!PairUrl::isPairPath(path, strlen(path))) {
      // Unity's message is the only way to say WHICH of ten thousand failed.
      TEST_FAIL_MESSAGE(path);
    }
  }
}

void test_the_same_holds_through_pathOf_from_a_raw_target() {
  char target[PairUrl::PATH_LEN + 1];
  for (int n = 0; n < 10000; n++) {
    snprintf(target, sizeof(target), "/%04d", n);
    if (!servesPage(target)) {
      TEST_FAIL_MESSAGE(target);
    }
  }
}

void test_no_path_of_any_other_length_is_ever_accepted() {
  // 0..8 digits, all of them but PIN_LEN rejected. This is the length half of
  // the rule stated separately from the digit half, so a change to either
  // fails with the right message.
  char path[16];
  for (size_t digits = 0; digits <= 8; digits++) {
    path[0] = '/';
    for (size_t i = 0; i < digits; i++) {
      path[1 + i] = '4';
    }
    path[1 + digits] = '\0';
    bool want = digits == AuthFmt::PIN_LEN;
    TEST_ASSERT_EQUAL_MESSAGE(want, PairUrl::isPairPath(path, strlen(path)), path);
  }
}

// ---- the near misses -----------------------------------------------------

void test_the_near_miss_shapes() {
  TEST_ASSERT_FALSE(servesPage("/482"));      // one digit short
  TEST_ASSERT_FALSE(servesPage("/48211"));    // one digit long
  TEST_ASSERT_FALSE(servesPage("/4821/x"));   // right prefix, deeper path
  TEST_ASSERT_FALSE(servesPage("/4821/"));    // right prefix, trailing slash
  TEST_ASSERT_FALSE(servesPage("/482a"));     // a letter in the digit run
  TEST_ASSERT_FALSE(servesPage("/48 1"));     // a space in the digit run
  TEST_ASSERT_FALSE(servesPage("4821"));      // no leading slash
  TEST_ASSERT_FALSE(servesPage("//4821"));    // an empty first segment
  TEST_ASSERT_FALSE(servesPage("/x/4821"));   // right shape, wrong depth
  TEST_ASSERT_FALSE(servesPage(""));          //
  TEST_ASSERT_FALSE(servesPage("/"));         //
  TEST_ASSERT_FALSE(servesPage(nullptr));     //
  // Percent-encoding is NOT decoded — see the header comment. `/%34%38%32%31`
  // would be "/4821" after decoding and is deliberately not the pair URL,
  // because esp_http_server does not decode before matching either.
  TEST_ASSERT_FALSE(servesPage("/%34%38%32%31"));
}

void test_digits_only_means_ascii_digits_only() {
  // Every byte value in the last position, PIN_LEN-1 real digits before it.
  char path[PairUrl::PATH_LEN + 1] = {'/', '4', '8', '2', '?', '\0'};
  for (int c = 0; c < 256; c++) {
    path[PairUrl::PATH_LEN - 1] = (char)c;
    if (c == '\0') {
      continue;  // a NUL is a shorter string, covered by the length test
    }
    bool want = c >= '0' && c <= '9';
    TEST_ASSERT_EQUAL_MESSAGE(want, PairUrl::isPairPath(path, PairUrl::PATH_LEN), path);
  }
}

// ---- the two forms the router matches on, and the handler used not to -----

void test_a_query_string_is_not_part_of_the_path() {
  TEST_ASSERT_TRUE(servesPage("/4821?x=1"));
  TEST_ASSERT_TRUE(servesPage("/4821?"));
  // ...and the query cannot smuggle a shape past the test either.
  TEST_ASSERT_FALSE(servesPage("/482?1"));
  TEST_ASSERT_FALSE(servesPage("/48211?x=1"));
}

void test_a_fragment_is_not_part_of_the_path() {
  // THE FIRST OF THE TWO INPUTS THE OLD strcspn(uri, "?") GOT WRONG. A client
  // must not send a fragment, but nothing stops one; http_parser splits it off
  // into UF_FRAGMENT, so the router matches `/*` on a five-byte path while the
  // handler was handed all seven bytes and 404'd.
  TEST_ASSERT_TRUE(servesPage("/4821#x"));
  TEST_ASSERT_TRUE(servesPage("/4821#"));
  TEST_ASSERT_TRUE(servesPage("/4821?a=1#x"));
  TEST_ASSERT_FALSE(servesPage("/482#1"));
}

void test_absolute_form_targets_reach_the_same_answer() {
  // THE SECOND. RFC 7230 §5.3.2: a server MUST accept the absolute form, and
  // esp_http_server routes it correctly because it hands the matcher
  // `req->uri + UF_PATH.off`. The handler gets the whole target, so it has to
  // do the same skip.
  TEST_ASSERT_TRUE(servesPage("http://192.168.4.1/4821"));
  TEST_ASSERT_TRUE(servesPage("HTTP://192.168.4.1/4821"));
  TEST_ASSERT_TRUE(servesPage("http://192.168.4.1:80/4821"));
  TEST_ASSERT_TRUE(servesPage("https://tdongle-a9d8/4821?x=1"));
  TEST_ASSERT_TRUE(servesPage("http://user:pw@192.168.4.1/4821"));
  TEST_ASSERT_FALSE(servesPage("http://192.168.4.1/482"));
  TEST_ASSERT_FALSE(servesPage("http://192.168.4.1/"));
  TEST_ASSERT_FALSE(servesPage("http://192.168.4.1"));
  // The authority is not searched for a pair shape: only the path is.
  TEST_ASSERT_FALSE(servesPage("http://4821/x"));
  TEST_ASSERT_FALSE(servesPage("http://4821"));
}

void test_pathOf_reports_the_span_it_matched() {
  PairUrl::Path p = PairUrl::pathOf("http://192.168.4.1/4821?x=1#f");
  TEST_ASSERT_EQUAL_size_t(5, p.len);
  TEST_ASSERT_EQUAL_STRING_LEN("/4821", p.at, 5);
  // The span points INTO the caller's buffer; nothing is copied.
  p = PairUrl::pathOf("/api/status?x=1");
  TEST_ASSERT_EQUAL_size_t(11, p.len);
  TEST_ASSERT_EQUAL_STRING_LEN("/api/status", p.at, 11);
}

void test_things_that_only_look_like_a_scheme() {
  // A path that contains "://" later on is not absolute-form, because the
  // scheme must start at byte 0 and byte 0 here is '/'.
  PairUrl::Path p = PairUrl::pathOf("/x/http://192.168.4.1/4821");
  TEST_ASSERT_EQUAL_size_t(strlen("/x/http://192.168.4.1/4821"), p.len);
  TEST_ASSERT_FALSE(PairUrl::isPairPath(p));
  // A bare colon is not "://".
  p = PairUrl::pathOf("mailto:4821");
  TEST_ASSERT_EQUAL_STRING_LEN("mailto:4821", p.at, p.len);
  // CONNECT's authority-form has no scheme and no leading slash: no path, no
  // match, and no read past the end of the string.
  TEST_ASSERT_FALSE(servesPage("192.168.4.1:443"));
}

// ---- the collision that must not exist -----------------------------------

void test_no_captive_probe_path_is_pin_shaped() {
  for (size_t i = 0; i < sizeof(PROBE_PATHS) / sizeof(PROBE_PATHS[0]); i++) {
    TEST_ASSERT_FALSE_MESSAGE(PairUrl::isPairPath(PROBE_PATHS[i], strlen(PROBE_PATHS[i])), PROBE_PATHS[i]);
  }
}

void test_equals_is_length_then_compare() {
  PairUrl::Path p = PairUrl::pathOf("/generate_204");
  TEST_ASSERT_TRUE(PairUrl::equals(p, "/generate_204"));
  TEST_ASSERT_FALSE(PairUrl::equals(p, "/generate_20"));
  TEST_ASSERT_FALSE(PairUrl::equals(p, "/generate_2040"));
  TEST_ASSERT_FALSE(PairUrl::equals(p, "/gen_204"));
  // A prefix of the probe path must not match it, which is the property that
  // makes one wildcard handler safe to do six exact lookups inside.
  p = PairUrl::pathOf("/generate_204x");
  TEST_ASSERT_FALSE(PairUrl::equals(p, "/generate_204"));
  // And the query is off the end of the span, so it still matches.
  p = PairUrl::pathOf("http://connectivitycheck.gstatic.com/generate_204?t=1#f");
  TEST_ASSERT_TRUE(PairUrl::equals(p, "/generate_204"));
}

// ---- constant-expression usability ---------------------------------------

void test_the_predicate_is_usable_at_compile_time() {
  // mod_http.cpp static_asserts over its own probe table with these, so a
  // non-constexpr regression here would break that build rather than this
  // test. Asserted anyway so the failure arrives with a message.
  static_assert(PairUrl::isPairPath("/4821", 5), "isPairPath is no longer constexpr-usable");
  static_assert(!PairUrl::isPairPath("/482", 4), "");
  static_assert(!PairUrl::isPairPath("/ncsi.txt", 9), "");
  static_assert(PairUrl::litLen("/gen_204") == 8, "");
  TEST_PASS();
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_every_pin_shaped_path_is_accepted_all_ten_thousand);
  RUN_TEST(test_the_same_holds_through_pathOf_from_a_raw_target);
  RUN_TEST(test_no_path_of_any_other_length_is_ever_accepted);
  RUN_TEST(test_the_near_miss_shapes);
  RUN_TEST(test_digits_only_means_ascii_digits_only);
  RUN_TEST(test_a_query_string_is_not_part_of_the_path);
  RUN_TEST(test_a_fragment_is_not_part_of_the_path);
  RUN_TEST(test_absolute_form_targets_reach_the_same_answer);
  RUN_TEST(test_pathOf_reports_the_span_it_matched);
  RUN_TEST(test_things_that_only_look_like_a_scheme);
  RUN_TEST(test_no_captive_probe_path_is_pin_shaped);
  RUN_TEST(test_equals_is_length_then_compare);
  RUN_TEST(test_the_predicate_is_usable_at_compile_time);
  return UNITY_END();
}
