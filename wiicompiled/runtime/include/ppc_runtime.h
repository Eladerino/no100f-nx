// The PowerPC ISA package (runtime/include/isa/), re-exported under the single
// header name the generated code and the runtime include. The package has two
// host seams this project satisfies elsewhere: "ppc_isa_memory.h" (pulled by
// the quantized tier) and the ShowRuntimeFatalPopup implementation.

#pragma once

#include "isa/ppc_isa_config.h"
#include "isa/big_endian.h"
#include "isa/ppc_isa_fpenv.h"
#include "isa/ppc_isa_context.h"
#include "isa/ppc_isa_int.h"
#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_quantized.h"

#include <cstdint>

// Executor safepoint (SAFEPOINT-DESIGN, Route A): the translator emits a
// check calling these at call-free back-edges in generated code. Implemented
// in the runtime (hle/vi.cpp); declared here because every generated unit
// includes this header.
extern "C" bool GuestSafepointRequested();
extern "C" void GuestSafepointServe(CpuContext* ctx, uint32_t backEdgeTarget);
