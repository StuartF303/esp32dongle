// usbdongle W1 — cooperative task scheduler.
//
// Replaces delay()-based pacing in loop(). Tasks are checked once per
// Scheduler::run() call (i.e. once per loop() iteration) and a task whose
// interval has elapsed runs exactly once per pass. If a task overran and is
// "owed" several periods, the scheduler does not try to catch up by firing
// it back-to-back — nextDue is rebased from *now*, not from the missed
// deadline. That is what stops one slow task from starving the rest of the
// table or wrecking their timing.
//
// Fixed-size task table, no dynamic allocation — this chip has no PSRAM and
// 320 KB usable RAM is a real budget.

#pragma once

#include <Arduino.h>

typedef void (*SchedulerTaskFn)();

struct SchedulerTaskStats {
  const char *name;
  uint32_t intervalMs;       // 0 means "run every pass"
  uint32_t lastRunMs;        // millis() at the start of the most recent run, 0 if never run
  uint32_t runCount;
  uint32_t lastDurationUs;   // wall time of the most recent run
  uint32_t worstDurationUs;  // worst wall time seen since boot — this is how a
                              // hogging module gets caught, see the `tasks` console command
};

class Scheduler {
 public:
  static const uint8_t MAX_TASKS = 12;

  // Registers a periodic task. intervalMs == 0 means "run every pass" (as
  // fast as the loop turns over) — used by the console's serial poll.
  // Returns false if the table is full or fn is null.
  bool addTask(const char *name, uint32_t intervalMs, SchedulerTaskFn fn);

  // Call once per loop() iteration. Never blocks, never calls delay().
  void run();

  uint8_t taskCount() const { return count_; }
  const SchedulerTaskStats &statsAt(uint8_t i) const { return tasks_[i].stats; }

 private:
  struct Task {
    SchedulerTaskFn fn;
    uint32_t nextDueMs;
    SchedulerTaskStats stats;
  };

  Task tasks_[MAX_TASKS];
  uint8_t count_ = 0;
};

// One scheduler instance for the whole app — this is a small, single-purpose
// firmware image, not a library, so a global keeps every task registration
// call site simple.
extern Scheduler scheduler;
