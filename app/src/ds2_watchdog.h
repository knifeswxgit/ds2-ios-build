// ds2 - load/hang telemetry for console runs (cvar ds2_watchdog, on by default).
//
// Every ds2_watchdog_snapshot_s seconds one INFO block goes to the main log (logs/ds2_NNN.log):
//   [ds2 wd] snapshot: guest threads, open guest files, host memory (process heap used / total, guest
//            backing committed), and every guest thread's lr / r1 / r3 / ctr / last indirect target;
//   [ds2 wd] hb:       every ds2_watchdog_heartbeat_s (2) s, one line: vblank counter, shaders last bound by the
//                      GPU command processor, thread/file counts, host memory - it stops only if the process stops;
//   [ds2 wd] file +/-: guest files opened or closed since the previous second (each change once);
//   [ds2 wd] STALL:    no guest register and no open file changed for 5 s (repeated every 15 s while it lasts).
// The log is flushed after every block, so the last lines survive a forced close of a hung game.
// Nothing runs in the game's own loops: it is one thread that wakes up once per second.

#pragma once

namespace rex {
class Runtime;
}

namespace ds2::watchdog {
// Switch-only switches that must be set early (ds2_switch_saltynx), called from OnPostInitLogging.
void ApplyPlatformSwitches();
void Start(rex::Runtime* runtime);
void Stop();
}  // namespace ds2::watchdog
