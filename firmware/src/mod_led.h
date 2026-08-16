// usbdongle W1 — the `led` module: the first real registry module.
//
// Deliberately the smallest thing that still proves the registry end to end
// against real hardware rather than a stub: it claims a real resource
// (Claims::RES_LED, exclusive, the APA102 on GPIO 40/39), it owns a scheduler
// task that must actually stop when the module is disabled, and it has an
// action that must actually be refused when the module is disabled.
//
// Actions:
//   {"mod":"led","act":"set","p":{"rgb":"ff0000"}}   manual colour ("off" also accepted)
//   {"mod":"led","act":"auto"}                        back to the heartbeat blink

#pragma once

#include "registry.h"

// Static descriptor; safe to hand straight to Registry::add().
const ModuleDescriptor *ledModuleDescriptor();

// Scheduler task (register at ~500 ms). Runs the heartbeat blink only while
// the module is enabled; a no-op otherwise.
void ledModuleTask();
