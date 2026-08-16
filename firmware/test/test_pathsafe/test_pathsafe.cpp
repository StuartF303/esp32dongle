// Host-side unit tests for PathSafe::check() (pathsafe.h).
//
// `pio test -e native` — no hardware. The SD card root is the sandbox boundary
// for the `storage` module, and a path may arrive from a phone over Wi-Fi or
// BLE. This is the one function standing between that and arbitrary read/write
// of anything else mounted on the VFS, so it gets tested independently of the
// module that calls it.
//
// Written deliberately as an adversarial table rather than a happy path: the
// interesting cases are the ones that look like a filename and are not.

#include <unity.h>

#include <string>

#include "pathsafe.h"

using namespace PathSafe;

void setUp() {}
void tearDown() {}

static void reject(const char *p, Result expected, const char *why) {
  Result got = check(p);
  if (got != expected) {
    std::string m = std::string(why) + " — path " + (p ? std::string("\"") + p + "\"" : "(null)") +
                    " gave " + resultName(got) + ", expected " + resultName(expected);
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

static void accept(const char *p, const char *why) {
  Result got = check(p);
  if (got != PATH_OK) {
    std::string m = std::string(why) + " — path \"" + p + "\" was rejected as " + resultName(got);
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

// ---- the traversal cases, which are the whole point --------------------

void test_parent_traversal_is_rejected() {
  reject("/..", PATH_PARENT_SEGMENT, "bare parent");
  reject("/../etc", PATH_PARENT_SEGMENT, "parent at the front");
  reject("/a/../../b", PATH_PARENT_SEGMENT, "parent climbing past the root");
  reject("/a/..", PATH_PARENT_SEGMENT, "parent at the end");
  reject("/a/../b", PATH_PARENT_SEGMENT, "parent in the middle — must NOT be normalised away");
  reject("/macros/../../../../etc/passwd", PATH_PARENT_SEGMENT, "deep climb");
}

// A canonicaliser would collapse these to something harmless; we reject
// instead, so a path has exactly one spelling.
void test_dot_segments_are_rejected() {
  reject("/.", PATH_DOT_SEGMENT, "bare dot");
  reject("/./a", PATH_DOT_SEGMENT, "dot at the front");
  reject("/a/./b", PATH_DOT_SEGMENT, "dot in the middle");
}

// FatFs accepts '\' as a separator, so allowing it would give a second,
// unchecked traversal syntax. Must be caught as BACKSLASH regardless of what
// the segment walk would otherwise have said.
void test_backslash_is_rejected_before_anything_else() {
  reject("/a\\b", PATH_BACKSLASH, "backslash as separator");
  reject("/a\\..\\b", PATH_BACKSLASH, "backslash traversal reported as backslash, deterministically");
  reject("\\", PATH_NOT_ABSOLUTE, "a lone backslash is not an absolute path");
}

// FatFs reads "0:" / "N:" as a DRIVE PREFIX — escapes the mountpoint with no
// ".." involved at all.
void test_drive_prefix_is_rejected() {
  reject("/0:/secret", PATH_RESERVED_CHAR, "FatFs drive prefix");
  reject("/a:b", PATH_RESERVED_CHAR, "colon anywhere");
}

// FAT strips trailing dots and spaces, so "/secret." and "/secret" can name the
// same file — any allow/deny list comparing strings would disagree with the
// filesystem.
void test_trailing_dot_and_space_are_rejected() {
  reject("/name.", PATH_TRAILING_DOT, "trailing dot");
  reject("/...", PATH_TRAILING_DOT, "three dots is not a parent segment but is still ambiguous");
  reject("/name ", PATH_TRAILING_SPACE, "trailing space");
  reject("/a/name./b", PATH_TRAILING_DOT, "trailing dot on an interior component");
}

void test_shape_rules() {
  reject(nullptr, PATH_NULL, "null pointer");
  reject("", PATH_EMPTY, "empty string");
  reject("relative/path", PATH_NOT_ABSOLUTE, "no leading slash");
  reject("//a", PATH_EMPTY_SEGMENT, "double separator");
  reject("/a//b", PATH_EMPTY_SEGMENT, "double separator in the middle");
  reject("/a/", PATH_EMPTY_SEGMENT, "trailing separator — one spelling per path");
  reject("/a\x01" "b", PATH_CONTROL_CHAR, "control character");
  // NOTE the string-literal split. "\x7fb" is a MULTI-DIGIT hex escape in C++
  // (value 0x7fb), not DEL followed by 'b' — writing it the obvious way meant
  // this case silently tested nothing and passed.
  reject("/a\x7f" "b", PATH_CONTROL_CHAR, "DEL");
  reject("/a*b", PATH_RESERVED_CHAR, "FAT reserved character");
  reject("/a?b", PATH_RESERVED_CHAR, "FAT reserved character");
}

void test_length_bounds() {
  // Multi-segment on purpose: a single long component trips the SEGMENT limit
  // first and masks the total-length gate. That masking is exactly what hid an
  // off-by-one here (a maxLen+1 path was accepted) until this case was split out.
  auto multiSeg = [](size_t total) {
    size_t tail = total - 1 - 150 - 1;  // "/" + 150 + "/" + tail
    return "/" + std::string(150, 'a') + "/" + std::string(tail, 'b');
  };

  std::string atLimit = multiSeg(MAX_PATH_LEN);
  accept(atLimit.c_str(), "exactly at the limit must be accepted");

  std::string oneOver = multiSeg(MAX_PATH_LEN + 1);
  reject(oneOver.c_str(), PATH_TOO_LONG, "exactly one byte over the limit");

  std::string wayOver = multiSeg(MAX_PATH_LEN + 40);
  reject(wayOver.c_str(), PATH_TOO_LONG, "well over the limit");

  std::string longSeg = "/" + std::string(MAX_SEGMENT_LEN + 1, 'a');
  reject(longSeg.c_str(), PATH_SEGMENT_TOO_LONG, "component over the segment limit");
}

// A non-terminated buffer must be reported, not read off the end. Run this
// under ASan (see the env) to make the guarantee mean something.
void test_unterminated_buffer_is_bounded() {
  char buf[8];
  for (size_t i = 0; i < sizeof(buf); i++) {
    buf[i] = 'a';
  }
  buf[0] = '/';
  TEST_ASSERT_EQUAL(PATH_TOO_LONG, check(buf, sizeof(buf) - 2));
}

void test_legitimate_paths_are_accepted() {
  accept("/", "the card root");
  accept("/macros", "a top-level file");
  accept("/macros/login.txt", "a nested file");
  accept("/a/b/c/d/e", "several levels");
  accept("/file.with.dots.txt", "interior dots are fine");
  accept("/name with spaces.txt", "interior spaces are fine");
  accept("/UPPER_lower-123", "ordinary punctuation");
  accept("/\xc3\xa9t\xc3\xa9.txt", "UTF-8 — ESP-IDF FatFs uses UTF-8 API encoding");
}

// Every reason must have a distinct token and a message that is not the
// fallback — an error a caller cannot branch on is barely better than none.
void test_every_result_has_a_name_and_message() {
  for (uint8_t r = PATH_OK; r <= PATH_SEGMENT_TOO_LONG; r++) {
    const char *n = resultName((Result)r);
    const char *m = resultMessage((Result)r);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_TRUE_MESSAGE(n[0] != '?', "a PathSafe::Result is missing from resultName()");
    // Compare against the ACTUAL fallback rather than guessing at its prefix:
    // most legitimate messages also begin "p.path", so a prefix test here was
    // just wrong and failed on correct code.
    const char *fallback = resultMessage((Result)0xFF);
    if (r != PATH_OK) {
      TEST_ASSERT_TRUE_MESSAGE(std::string(m) != std::string(fallback),
                               "a PathSafe::Result fell through to the generic resultMessage()");
    }
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parent_traversal_is_rejected);
  RUN_TEST(test_dot_segments_are_rejected);
  RUN_TEST(test_backslash_is_rejected_before_anything_else);
  RUN_TEST(test_drive_prefix_is_rejected);
  RUN_TEST(test_trailing_dot_and_space_are_rejected);
  RUN_TEST(test_shape_rules);
  RUN_TEST(test_length_bounds);
  RUN_TEST(test_unterminated_buffer_is_bounded);
  RUN_TEST(test_legitimate_paths_are_accepted);
  RUN_TEST(test_every_result_has_a_name_and_message);
  return UNITY_END();
}
