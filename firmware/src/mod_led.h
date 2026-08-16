// usbdongle W1 — the `led` module: the first real registry module.
//
// Deliberately the smallest thing that still proves the registry end to end
// against real hardware rather than a stub: it claims a real resource
// (Claims::RES_LED, exclusive, the APA102 on GPIO 40/39), it owns a periodic
// tick that must actually stop when the module is disabled, and it has an
// action that must actually be refused when the module is disabled.
//
// Actions:
//   {"mod":"led","act":"set","p":{"rgb":"ff0000"}}   manual colour ("off" also accepted)
//   {"mod":"led","act":"auto"}                        back to the heartbeat blink
//
// The heartbeat tick is declared in the descriptor (tick / tickIntervalMs) and
// registered with the Scheduler by the registry, which also gates it on the
// enabled flag. There is nothing for main.cpp to wire up beyond add().

#pragma once

#include "registry.h"

// Static descriptor; safe to hand straight to Registry::add().
const ModuleDescriptor *ledModuleDescriptor();
