// ds2 - automated tests on the PC: a virtual pad driven by a button script.
//
// With --ds2_input_script (for example "60:start,70:a,80-82:down") the app adds to the SDK input system a synthetic
// device that presses those buttons at those seconds after the input system is created. Empty (the default) adds
// nothing. Adapted from nfsmw-nx app/src/nfsmw_prueba_entrada.cpp (GPL-3.0), without its game-specific autopilot.

#pragma once

#include <rex/runtime.h>

namespace ds2::input_script {

// Wraps config.input_factory to add the script pad if the cvar is not empty. Call it in OnPreSetup.
void Wrap(rex::RuntimeConfig& config);
// Applies ds2_guest_time_scalar (call after the runtime has set up its clock).
void ApplyTimeScalar();

}  // namespace ds2::input_script
