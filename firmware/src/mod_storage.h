// usbdongle W3 — `storage`: the microSD card over the command bus.
//
// See ARCHITECTURE.md section 4: browse / upload / download the card from the
// phone. Claims RES_SD SHARED, so `msc` (which claims it EXCLUSIVE to hand the
// raw block device to the host PC) locks this out by arbitration rather than by
// either module knowing about the other.

#pragma once

#include "registry.h"

// Static descriptor; register with registry.add() from setup().
const ModuleDescriptor *storageModuleDescriptor();
