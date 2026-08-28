#ifndef _HOST_STUB_PICO_MULTICORE_H_
#define _HOST_STUB_PICO_MULTICORE_H_

// Host stand-in for pico/multicore.h. Deliberately empty of lockout helpers: a host test drives
// the two cores as threads, and any firmware source that reaches for multicore_lockout_* while
// under test should fail to link rather than silently no-op.

#include "pico/stdlib.h"

#endif  // _HOST_STUB_PICO_MULTICORE_H_
