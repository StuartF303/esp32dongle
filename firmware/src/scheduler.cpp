#include "scheduler.h"

Scheduler scheduler;

bool Scheduler::addTask(const char *name, uint32_t intervalMs, SchedulerTaskFn fn) {
  if (count_ >= MAX_TASKS || fn == nullptr) {
    return false;
  }

  Task &t = tasks_[count_];
  t.fn = fn;
  t.nextDueMs = millis();
  t.stats.name = name;
  t.stats.intervalMs = intervalMs;
  t.stats.lastRunMs = 0;
  t.stats.runCount = 0;
  t.stats.lastDurationUs = 0;
  t.stats.worstDurationUs = 0;

  count_++;
  return true;
}

void Scheduler::run() {
  uint32_t now = millis();

  for (uint8_t i = 0; i < count_; i++) {
    Task &t = tasks_[i];

    // Signed subtraction on unsigned millis() handles the ~49.7-day wrap
    // safely: (now - nextDueMs) is still the correct signed delta either
    // side of a wrap, as long as the gap is well under 2^31 ms.
    if ((int32_t)(now - t.nextDueMs) < 0) {
      continue;  // not due yet
    }

    uint32_t startUs = micros();
    t.fn();
    uint32_t elapsedUs = micros() - startUs;

    t.stats.lastRunMs = now;
    t.stats.runCount++;
    t.stats.lastDurationUs = elapsedUs;
    if (elapsedUs > t.stats.worstDurationUs) {
      t.stats.worstDurationUs = elapsedUs;
    }

    // Rebase from *now* (post-run), not from the missed deadline. If this
    // task just overran, this is what stops it firing again immediately to
    // "catch up" and hogging the loop at the expense of every other task.
    now = millis();
    t.nextDueMs = now + t.stats.intervalMs;
  }
}
