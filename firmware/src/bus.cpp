#include "bus.h"

#include <atomic>

#include <freertos/FreeRTOS.h>

namespace Bus {

namespace {

// Append-only. An entry, once written, is never changed or removed — which is
// what makes an unlocked emit() correct: a reader can only ever see a prefix of
// the table, and every entry in that prefix is fully written.
SinkFn sinks[MAX_SINKS] = {nullptr};

// The publication point. Written with release ordering AFTER the sink pointer
// it exposes, read with acquire ordering BEFORE the pointer is dereferenced.
// A plain `uint8_t count` would let a reader on the other core observe the
// incremented count while sinks[i] is still null — one core's store buffer is
// not the other core's problem to guess at.
std::atomic<uint8_t> count{0};

// Guards addSink against a concurrent addSink. Not needed by emit(). Held for a
// handful of instructions and never across a call, so a spinlock is right here
// and a mutex would be heavier than the thing it protects.
portMUX_TYPE addLock = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

bool addSink(SinkFn sink) {
  if (sink == nullptr) {
    return false;
  }

  bool ok = false;
  portENTER_CRITICAL(&addLock);
  uint8_t n = count.load(std::memory_order_relaxed);  // sole writer inside the lock
  if (n < MAX_SINKS) {
    bool dup = false;
    for (uint8_t i = 0; i < n; i++) {
      if (sinks[i] == sink) {
        dup = true;  // already registered: never deliver an event twice
        break;
      }
    }
    if (!dup) {
      sinks[n] = sink;
      // RELEASE: everything above (the sink pointer) is visible to any task
      // that later sees this count.
      count.store((uint8_t)(n + 1), std::memory_order_release);
      ok = true;
    }
  }
  portEXIT_CRITICAL(&addLock);
  return ok;
}

void emit(const char *name, FillFn fill, void *ctx) {
  if (name == nullptr) {
    return;
  }
  // ACQUIRE: pairs with the release store in addSink().
  uint8_t n = count.load(std::memory_order_acquire);
  for (uint8_t i = 0; i < n; i++) {
    sinks[i](name, fill, ctx);
  }
}

uint8_t sinkCount() { return count.load(std::memory_order_acquire); }

}  // namespace Bus
