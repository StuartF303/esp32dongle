// Host-side unit tests for VolPath::split() (volpath.h).
//
// `pio test -e native` — no hardware. This is the half of the `storage` path
// funnel that runs BEFORE PathSafe, so anything it lets through unsplit, or
// splits wrongly, lands on a syscall with a mountpoint glued to the front of
// it. Written as an adversarial table in the same spirit as test_pathsafe: the
// interesting inputs are the ones that look like a volume and are not.
//
// The pairing with PathSafe is tested too — a "/sd" prefix must not become a
// way to smuggle "..", a backslash or a drive letter past the checker that
// exists to refuse them.

#include <unity.h>

#include <string>

#include "pathsafe.h"
#include "volpath.h"

using namespace VolPath;

void setUp() {}
void tearDown() {}

static void reject(const char *p, Result expected, const char *why) {
  Split s;
  Result got = split(p, &s);
  if (got != expected) {
    std::string m = std::string(why) + " — path " + (p ? std::string("\"") + p + "\"" : "(null)") + " gave " +
                    resultName(got) + ", expected " + resultName(expected);
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

static void accept(const char *p, const char *volume, const char *rest, bool isRoot, const char *why) {
  Split s;
  Result got = split(p, &s);
  if (got != VOLPATH_OK) {
    std::string m = std::string(why) + " — path \"" + p + "\" was rejected as " + resultName(got);
    TEST_FAIL_MESSAGE(m.c_str());
  }
  if (std::string(s.volume) != volume) {
    std::string m = std::string(why) + " — volume was \"" + s.volume + "\", expected \"" + volume + "\"";
    TEST_FAIL_MESSAGE(m.c_str());
  }
  if (std::string(s.rest) != rest) {
    std::string m = std::string(why) + " — rest was \"" + s.rest + "\", expected \"" + rest + "\"";
    TEST_FAIL_MESSAGE(m.c_str());
  }
  if (s.isRoot != isRoot) {
    TEST_FAIL_MESSAGE(why);
  }
}

// ---- the ordinary shapes -------------------------------------------------

void test_the_two_real_volumes_split() {
  accept("/sd", "sd", "/", true, "the card root");
  accept("/fs", "fs", "/", true, "the littlefs root");
  accept("/sd/logs/run.txt", "sd", "/logs/run.txt", false, "a file on the card");
  accept("/fs/www/index.html.gz", "fs", "/www/index.html.gz", false, "a file on littlefs");
  accept("/sd/a", "sd", "/a", false, "one segment");
}

// The prefix length is what the module uses to strip the mountpoint back off a
// VFS path when reporting where a recursive delete stopped. If it is wrong the
// caller is told about a path they never sent.
void test_prefix_length_is_the_bytes_consumed() {
  Split s;
  TEST_ASSERT_EQUAL(VOLPATH_OK, split("/sd/a/b", &s));
  TEST_ASSERT_EQUAL_size_t(3, s.prefixLen);
  TEST_ASSERT_EQUAL(VOLPATH_OK, split("/fs", &s));
  TEST_ASSERT_EQUAL_size_t(3, s.prefixLen);
  TEST_ASSERT_EQUAL(VOLPATH_OK, split("/abcdefgh/x", &s));
  TEST_ASSERT_EQUAL_size_t(9, s.prefixLen);
}

// An unknown volume is NOT this function's business — it splits, the module
// looks the name up. Splitting must therefore succeed for a well-formed name
// that names nothing, so the module can answer "no such volume" rather than
// "malformed path".
void test_unknown_but_wellformed_volumes_split_cleanly() {
  accept("/nope", "nope", "/", true, "well-formed, names nothing");
  accept("/x9/a", "x9", "/a", false, "digits are legal in a volume name");
}

// ---- one spelling per path ----------------------------------------------

void test_trailing_slash_is_a_second_spelling_and_is_refused() {
  reject("/sd/", VOLPATH_TRAILING_SLASH, "the volume root, spelled twice");
  reject("/fs/", VOLPATH_TRAILING_SLASH, "the littlefs root, spelled twice");
  // Deeper trailing slashes belong to PathSafe, not here: rest is "/a/" and
  // PathSafe rejects it as an empty segment. Assert BOTH halves, because the
  // split succeeding here is only safe if the second half really does refuse.
  Split s;
  TEST_ASSERT_EQUAL(VOLPATH_OK, split("/sd/a/", &s));
  TEST_ASSERT_EQUAL(PathSafe::PATH_EMPTY_SEGMENT, PathSafe::check(s.rest));
}

void test_case_is_not_folded() {
  reject("/SD", VOLPATH_NAME_BAD_CHAR, "uppercase would be a second spelling of the same volume");
  reject("/Sd/a", VOLPATH_NAME_BAD_CHAR, "mixed case");
  reject("/FS/x", VOLPATH_NAME_BAD_CHAR, "uppercase littlefs");
}

// ---- the traversal cases, which are the whole point ----------------------

// A volume name is a fixed vocabulary chosen by the firmware. Anything that is
// not [a-z0-9] is refused before it can be compared to anything.
void test_traversal_in_the_volume_position() {
  reject("/..", VOLPATH_NAME_BAD_CHAR, "bare parent as a volume name");
  reject("/../etc/passwd", VOLPATH_NAME_BAD_CHAR, "climb out through the volume slot");
  reject("/.", VOLPATH_NAME_BAD_CHAR, "dot as a volume name");
  reject("/sd../x", VOLPATH_NAME_BAD_CHAR, "a name that starts like a real one");
  reject("/0:", VOLPATH_NAME_BAD_CHAR, "a FatFs drive prefix as a volume name");
  reject("/sd:/x", VOLPATH_NAME_BAD_CHAR, "drive prefix smuggled into the name");
  reject("/s d/x", VOLPATH_NAME_BAD_CHAR, "space in the name");
  reject("/sd\\x", VOLPATH_NAME_BAD_CHAR, "backslash — FatFs would take it as a separator");
  reject("/s\x01d/x", VOLPATH_NAME_BAD_CHAR, "control character in the name");
  reject("/s\x7f" "d/x", VOLPATH_NAME_BAD_CHAR, "DEL in the name");
}

// Traversal BELOW the prefix is PathSafe's job, and the prefix must not become
// a way of getting a path past it. These split fine and must then be refused.
void test_traversal_below_the_prefix_still_reaches_pathsafe() {
  struct Case {
    const char *path;
    PathSafe::Result expect;
  } cases[] = {
      {"/sd/../etc/passwd", PathSafe::PATH_PARENT_SEGMENT},
      {"/fs/../../sd/secret", PathSafe::PATH_PARENT_SEGMENT},
      {"/sd/a/../b", PathSafe::PATH_PARENT_SEGMENT},
      {"/sd/./a", PathSafe::PATH_DOT_SEGMENT},
      {"/sd/a\\b", PathSafe::PATH_BACKSLASH},
      {"/sd/0:/x", PathSafe::PATH_RESERVED_CHAR},
      {"/fs/a//b", PathSafe::PATH_EMPTY_SEGMENT},
      {"/sd/name.", PathSafe::PATH_TRAILING_DOT},
      {"/sd/name ", PathSafe::PATH_TRAILING_SPACE},
  };
  for (const Case &c : cases) {
    Split s;
    Result r = split(c.path, &s);
    if (r != VOLPATH_OK) {
      std::string m = std::string("split refused \"") + c.path + "\" as " + resultName(r) +
                      "; it must reach PathSafe instead";
      TEST_FAIL_MESSAGE(m.c_str());
    }
    PathSafe::Result pr = PathSafe::check(s.rest);
    if (pr != c.expect) {
      std::string m = std::string("\"") + c.path + "\" -> rest \"" + s.rest + "\" gave " + PathSafe::resultName(pr) +
                      ", expected " + PathSafe::resultName(c.expect);
      TEST_FAIL_MESSAGE(m.c_str());
    }
  }
}

// ---- shape rules ---------------------------------------------------------

void test_shape_rules() {
  reject(nullptr, VOLPATH_NULL, "null");
  reject("", VOLPATH_EMPTY, "empty");
  reject("sd/a", VOLPATH_NOT_ABSOLUTE, "relative");
  reject("sd", VOLPATH_NOT_ABSOLUTE, "a bare volume name is not a path");
  reject("/", VOLPATH_NO_VOLUME, "the bare root names no volume");
  reject("//", VOLPATH_EMPTY_VOLUME, "double slash");
  reject("//sd/a", VOLPATH_EMPTY_VOLUME, "empty name in front of a real one");
}

// ---- bounds --------------------------------------------------------------

void test_volume_name_length_bounds() {
  accept("/abcdefgh", "abcdefgh", "/", true, "exactly MAX_NAME_LEN");
  accept("/abcdefgh/x", "abcdefgh", "/x", false, "exactly MAX_NAME_LEN with a path");
  reject("/abcdefghi", VOLPATH_NAME_TOO_LONG, "one over MAX_NAME_LEN");
  reject("/abcdefghi/x", VOLPATH_NAME_TOO_LONG, "one over, with a path");
}

void test_total_length_bounds() {
  // Exactly MAX_TOTAL_LEN must pass the OUTER check; PathSafe then applies its
  // own 255-byte limit to `rest`, which is the one that actually binds here.
  std::string atLimit = "/sd/";
  atLimit += std::string(MAX_TOTAL_LEN - 4, 'a');
  TEST_ASSERT_EQUAL_size_t(MAX_TOTAL_LEN, atLimit.size());
  Split s;
  TEST_ASSERT_EQUAL(VOLPATH_OK, split(atLimit.c_str(), &s));
  // rest is 261 bytes, which is over PathSafe's own 255-byte limit — so the
  // outer bound being generous does NOT widen the inner one.
  TEST_ASSERT_EQUAL(PathSafe::PATH_TOO_LONG, PathSafe::check(s.rest));

  // ...and the longest path that is legal END TO END really is accepted by
  // both halves, so the pair has not quietly become unusable.
  // Two segments, because a single 254-byte one would hit the 200-byte segment
  // limit first and prove nothing about the total.
  std::string longest = "/sd/" + std::string(PathSafe::MAX_SEGMENT_LEN, 'a') + "/" +
                        std::string(PathSafe::MAX_PATH_LEN - PathSafe::MAX_SEGMENT_LEN - 2, 'b');
  TEST_ASSERT_EQUAL(VOLPATH_OK, split(longest.c_str(), &s));
  TEST_ASSERT_EQUAL_size_t(PathSafe::MAX_PATH_LEN, std::string(s.rest).size());
  TEST_ASSERT_EQUAL(PathSafe::PATH_OK, PathSafe::check(s.rest));

  std::string over = atLimit + "a";
  TEST_ASSERT_EQUAL(VOLPATH_TOO_LONG, split(over.c_str(), &s));
}

// The same off-by-one that bit PathSafe in 2026-08-16: a buffer with no NUL
// inside the limit must be reported as too long, never walked off the end.
void test_unterminated_buffer_is_bounded() {
  char buf[16];
  for (size_t i = 0; i < sizeof(buf); i++) {
    buf[i] = 'a';
  }
  buf[0] = '/';
  Split s;
  // maxLen 8 against a 16-byte run of non-NUL bytes: must stop, not read on.
  TEST_ASSERT_EQUAL(VOLPATH_TOO_LONG, split(buf, &s, 8));
}

// Out-params must be safe to read on a rejection too — the module fills its
// error response before it looks at anything else.
void test_out_is_cleared_on_rejection() {
  Split s;
  s.volume[0] = 'X';
  s.rest = "poison";
  s.isRoot = true;
  s.prefixLen = 99;
  TEST_ASSERT_EQUAL(VOLPATH_NO_VOLUME, split("/", &s));
  TEST_ASSERT_EQUAL_STRING("", s.volume);
  TEST_ASSERT_EQUAL_STRING("", s.rest);
  TEST_ASSERT_FALSE(s.isRoot);
  TEST_ASSERT_EQUAL_size_t(0, s.prefixLen);
}

void test_null_out_is_tolerated() {
  // The module always passes one, but a checker that segfaults on nullptr is a
  // checker someone will be tempted to skip.
  TEST_ASSERT_EQUAL(VOLPATH_OK, split("/sd/a", nullptr));
  TEST_ASSERT_EQUAL(VOLPATH_NO_VOLUME, split("/", nullptr));
}

// Every reason must have a distinct token and a message that is not the
// fallback — an error a caller cannot branch on is barely better than none.
void test_every_result_has_a_name_and_message() {
  for (uint8_t r = VOLPATH_OK; r <= VOLPATH_TRAILING_SLASH; r++) {
    const char *n = resultName((Result)r);
    const char *m = resultMessage((Result)r);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE_MESSAGE(n[0] != '?', "a VolPath::Result is missing from resultName()");
    const char *fallback = resultMessage((Result)0xFF);
    if (r != VOLPATH_OK) {
      TEST_ASSERT_TRUE_MESSAGE(std::string(m) != std::string(fallback),
                               "a VolPath::Result fell through to the generic resultMessage()");
    }
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_two_real_volumes_split);
  RUN_TEST(test_prefix_length_is_the_bytes_consumed);
  RUN_TEST(test_unknown_but_wellformed_volumes_split_cleanly);
  RUN_TEST(test_trailing_slash_is_a_second_spelling_and_is_refused);
  RUN_TEST(test_case_is_not_folded);
  RUN_TEST(test_traversal_in_the_volume_position);
  RUN_TEST(test_traversal_below_the_prefix_still_reaches_pathsafe);
  RUN_TEST(test_shape_rules);
  RUN_TEST(test_volume_name_length_bounds);
  RUN_TEST(test_total_length_bounds);
  RUN_TEST(test_unterminated_buffer_is_bounded);
  RUN_TEST(test_out_is_cleared_on_rejection);
  RUN_TEST(test_null_out_is_tolerated);
  RUN_TEST(test_every_result_has_a_name_and_message);
  return UNITY_END();
}
