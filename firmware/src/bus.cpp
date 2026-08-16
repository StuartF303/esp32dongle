#include "bus.h"

namespace Bus {

namespace {

SinkFn sinks[MAX_SINKS] = {nullptr};
uint8_t count = 0;

}  // namespace

bool addSink(SinkFn sink) {
  if (sink == nullptr || count >= MAX_SINKS) {
    return false;
  }
  for (uint8_t i = 0; i < count; i++) {
    if (sinks[i] == sink) {
      return false;  // already registered: never deliver an event twice
    }
  }
  sinks[count++] = sink;
  return true;
}

void emit(const char *name, FillFn fill, void *ctx) {
  if (name == nullptr) {
    return;
  }
  for (uint8_t i = 0; i < count; i++) {
    sinks[i](name, fill, ctx);
  }
}

uint8_t sinkCount() { return count; }

}  // namespace Bus
