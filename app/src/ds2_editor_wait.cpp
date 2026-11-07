// ds2 - the character editor wait (v42).
//
// v41 call counts (diag_toml41A, docs/handoff/16-switch-gpu-optimization.md): after the old ladies' cutscene the
// game calls "fade to black" (sub_825406C0) and never "fade to clear" (sub_82540678); the fade object holds alpha
// 1.0 and sub_82F1A3D8 draws it over the UI every frame. No editor-specific code runs. The only functions that
// run on the black screen and nowhere else are the fade drawing and sub_8239A2E8, called once per frame:
//
//   sub_8239A2E8: ui = G->[3296] (G = *0x834AA89C); return ui ? !sub_82677A78(ui) : 1
//   sub_82677A78: x = ui->[116]; return x && x->[16] != 0
//
// i.e. "is the UI free?" (x->[16] holds an inner object while something is pending). It is called from the
// menu-flow state machine sub_8239ABA0 (states 8/9, return 0x8239AD5C / 0x8239AD68) and from the event-script
// interpreter sub_825EC618 (0x825ED6B4 / 0x825ED758 area). The game waits for it to become true.
//
// ds2_editor_wait_log: one line every 2 s while it answers "busy": caller, ui, x, x->[16], x->[24], the inner
//   object's first words, and the state of the menu-flow machine (G->[68]->[40], state at +84).
// ds2_editor_force_free_s: experiment - after N seconds of continuous "busy", answer "free" (0 = off).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>

REXCVAR_DEFINE_BOOL(ds2_editor_wait_log, false, "DS2",
                    "Log the 'is the UI free' wait that holds the character editor (sub_8239A2E8) every 2 s");
REXCVAR_DEFINE_INT32(ds2_editor_force_free_s, 0, "DS2",
                     "Experiment: after N s of continuous 'UI busy' (sub_8239A2E8 = 0), answer 'free' (0 = off)");

REXCVAR_DEFINE_INT32(ds2_editor_open_mode, 0, "DS2",
                     "Experiment: on the editor black screen open UI menu type 4 ourselves: 1 = like script command "
                     "130459 (sub_8239A2A8), 2 = like 130461 (sub_8239A2C8), 0 = off");
REXCVAR_DEFINE_INT32(ds2_editor_open_after_s, 8, "DS2",
                     "Seconds of the black-screen wait (script polling function 130442) before ds2_editor_open_mode acts");
extern "C" void __imp__sub_825EFDF0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82679B88(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8239A2A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8239A2C8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8239A2E8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8239ABA0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
inline uint32_t Ld(uint8_t* base, uint32_t a) {
  if (a < 0x10000u) return 0;
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a));
}
constexpr uint32_t kGlobal = 0x834AA89Cu;
using Clock = std::chrono::steady_clock;
Clock::time_point g_busy_since{};
bool g_busy = false;
Clock::time_point g_last_log{};
uint64_t g_calls = 0, g_forced = 0;
uint32_t g_flow_state = 0xFFFFFFFFu;
}  // namespace

REX_HOOK_RAW(sub_8239A2E8) {
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_8239A2E8(ctx, base);
  ++g_calls;
  const auto now = Clock::now();
  const bool busy = (ctx.r3.u32 & 0xFF) == 0;
  if (!busy) {
    g_busy = false;
  } else if (!g_busy) {
    g_busy = true;
    g_busy_since = now;
  }
  const double busy_s = busy ? std::chrono::duration<double>(now - g_busy_since).count() : 0.0;
  const int force = REXCVAR_GET(ds2_editor_force_free_s);
  const bool forcing = force > 0 && busy_s >= force;
  if (REXCVAR_GET(ds2_editor_wait_log) && now - g_last_log >= std::chrono::seconds(2)) {
    g_last_log = now;
    const uint32_t g = Ld(base, kGlobal);
    const uint32_t ui = g ? Ld(base, g + 3296) : 0;
    const uint32_t x = ui ? Ld(base, ui + 116) : 0;
    const uint32_t inner = x ? Ld(base, x + 16) : 0;
    const uint32_t flow = g ? Ld(base, Ld(base, g + 68) + 40) : 0;
    REXLOG_INFO(
        "[editor] {} {:.1f} s (llamada desde {:08X}, {} llamadas): G {:08X} ui {:08X} x {:08X} "
        "x.vt {:08X} x[16] {:08X} x[20] {:08X} x[24] {:08X} x[28] {:08X} | interior vt {:08X} [4] {:08X} [8] {:08X} "
        "[12] {:08X} [16] {:08X} [20] {:08X} [24] {:08X} | flujo {:08X} estado {} [88..91] {:08X} | "
        "ui[108] {:08X} ui[120] {:08X}{}",
        busy ? "UI ocupada" : "UI libre", busy_s, caller, g_calls, g, ui, x, Ld(base, x), inner, Ld(base, x + 20), Ld(base, x + 24), Ld(base, x + 28),
        Ld(base, inner), Ld(base, inner + 4), Ld(base, inner + 8), Ld(base, inner + 12), Ld(base, inner + 16),
        Ld(base, inner + 20), Ld(base, inner + 24), flow, flow ? Ld(base, flow + 84) : 0,
        flow ? Ld(base, flow + 88) : 0, ui ? Ld(base, ui + 108) : 0, ui ? Ld(base, ui + 120) : 0,
        forcing ? " -> FORZADO libre" : "");
  }
  {
    // Black-screen wait streak: sub_8239A2E8 is only polled by the script on that screen (v41 counts).
    static Clock::time_point streak_start{}, streak_last{};
    static bool opened = false;
    if (streak_last == Clock::time_point{} || now - streak_last > std::chrono::seconds(1)) streak_start = now;
    streak_last = now;
    const int mode = REXCVAR_GET(ds2_editor_open_mode);
    if (mode && !opened && now - streak_start >= std::chrono::seconds(REXCVAR_GET(ds2_editor_open_after_s))) {
      opened = true;
      const uint32_t g = Ld(base, kGlobal);
      PPCContext c = ctx;
      c.r3.u64 = g ? Ld(base, Ld(base, g + 68) + 40) : 0;
      REXLOG_WARN("[editor] EXPERIMENTO: se abre el menu tipo 4 como la orden {} ({})", mode == 2 ? 130461 : 130459,
                  mode == 2 ? "sub_8239A2C8" : "sub_8239A2A8");
      if (mode == 2)
        __imp__sub_8239A2C8(c, base);
      else
        __imp__sub_8239A2A8(c, base);
      const uint32_t ui = g ? Ld(base, g + 3296) : 0;
      const uint32_t x = ui ? Ld(base, ui + 116) : 0;
      REXLOG_WARN("[editor] EXPERIMENTO hecho: ui[116] {:08X} x[16] {:08X} x[20] {:08X} x[24] {:08X}", x,
                  Ld(base, x + 16), Ld(base, x + 20), Ld(base, x + 24));
    }
  }
  if (forcing) {
    if (g_forced++ == 0) REXLOG_WARN("[editor] UI ocupada {:.1f} s: se responde 'libre' (ds2_editor_force_free_s)", busy_s);
    ctx.r3.u64 = 1;
  }
}

// Menu-flow state machine: log every state change (the object pointer and the new state).
REX_HOOK_RAW(sub_8239ABA0) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8239ABA0(ctx, base);
  if (!REXCVAR_GET(ds2_editor_wait_log)) return;
  const uint32_t st = Ld(base, self + 84);
  if (st != g_flow_state) {
    REXLOG_INFO("[editor] flujo de menus {:08X}: estado {} -> {} ([88..91] {:08X}, temporizador [80] {:08X})", self,
                g_flow_state, st, Ld(base, self + 88), Ld(base, self + 80));
    g_flow_state = st;
  }
}

// The other two conditions the event script evaluates every frame on the black screen (v41 counts): their
// return values, every 2 s.
extern "C" void __imp__sub_8268C4F8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8243EF40(PPCContext& __restrict ctx, uint8_t* base);
namespace {
Clock::time_point g_last_c4f8{}, g_last_ef40{};
}
REX_HOOK_RAW(sub_8268C4F8) {
  const uint32_t self = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_8268C4F8(ctx, base);
  if (!REXCVAR_GET(ds2_editor_wait_log)) return;
  const auto now = Clock::now();
  if (now - g_last_c4f8 < std::chrono::seconds(2)) return;
  g_last_c4f8 = now;
  REXLOG_INFO("[editor] sub_8268C4F8({:08X}) = {} (desde {:08X}; [660] {:08X})", self, ctx.r3.u32, caller,
              Ld(base, self + 660));
}
REX_HOOK_RAW(sub_8243EF40) {
  const uint32_t self = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_8243EF40(ctx, base);
  if (!REXCVAR_GET(ds2_editor_wait_log)) return;
  const auto now = Clock::now();
  if (now - g_last_ef40 < std::chrono::seconds(2)) return;
  g_last_ef40 = now;
  REXLOG_INFO("[editor] sub_8243EF40({:08X}) = {} (desde {:08X})", self, ctx.r3.u32, caller);
}

// Event-script function call (sub_825EC618): r3 = result (value at +0, type at +4), r5 = call object whose
// vtable[1] returns the function id. ds2_editor_wait_log: every 2 s one line with every id evaluated in that
// window, how often and its last result - which script function keeps the editor from opening.
extern "C" void __imp__sub_825EC618(PPCContext& __restrict ctx, uint8_t* base);
#include <map>
#include <string>
namespace {
struct ScriptCall {
  uint32_t n = 0, value = 0, type = 0, caller = 0;
};
std::map<uint32_t, ScriptCall> g_script;
Clock::time_point g_last_script{};
}  // namespace
REX_HOOK_RAW(sub_825EC618) {
  if (!REXCVAR_GET(ds2_editor_wait_log)) {
    __imp__sub_825EC618(ctx, base);
    return;
  }
  const uint32_t out = ctx.r3.u32, call = ctx.r5.u32, caller = uint32_t(ctx.lr);
  uint32_t id = 0;
  if (const uint32_t vt = Ld(base, call)) {
    if (PPCFunc* getter = rex::runtime::ResolveIndirectFunction(Ld(base, vt + 4))) {
      PPCContext probe = ctx;
      probe.r3.u64 = call;
      getter(probe, base);
      id = probe.r3.u32;
    }
  }
  __imp__sub_825EC618(ctx, base);
  ScriptCall& s = g_script[id];
  ++s.n;
  s.value = Ld(base, out);
  s.type = Ld(base, out + 4);
  s.caller = caller;
  const auto now = Clock::now();
  if (now - g_last_script < std::chrono::seconds(2)) return;
  g_last_script = now;
  std::string line;
  for (const auto& [k, v] : g_script) line += fmt::format(" {}:{}x={:X}/t{}", k, v.n, v.value, v.type);
  REXLOG_INFO("[editor] guion (id:llamadas=valor/tipo):{}", line);
  g_script.clear();
}

// v44. Event-script commands (sub_825EFDF0: r4 = call object, vtable[2] returns the command id) - rare, each one
// logged - and the UI's "open menu of type N" (sub_82679B88). Command 130459 / 130461 open menu type 4 through
// sub_8239A2A8 / sub_8239A2C8 (sub_82679DC0 / sub_82679D58, they differ only in byte [20] of the menu object: 0 / 1);
// type 4 is the object in ui[116] that script function 130442 checks every frame on the black screen.
REX_HOOK_RAW(sub_825EFDF0) {
  const uint32_t call = ctx.r4.u32, caller = uint32_t(ctx.lr), r3 = ctx.r3.u32, r5 = ctx.r5.u32;
  uint32_t id = 0;
  if (REXCVAR_GET(ds2_editor_wait_log)) {
    if (const uint32_t vt = Ld(base, call)) {
      if (PPCFunc* getter = rex::runtime::ResolveIndirectFunction(Ld(base, vt + 8))) {
        PPCContext probe = ctx;
        probe.r3.u64 = call;
        getter(probe, base);
        id = probe.r3.u32;
      }
    }
  }
  __imp__sub_825EFDF0(ctx, base);
  if (REXCVAR_GET(ds2_editor_wait_log))
    REXLOG_INFO("[editor] orden del guion {} (desde {:08X}, r3 {:08X} r5 {:08X}) -> {:08X}", id, caller, r3, r5,
                ctx.r3.u32);
}
REX_HOOK_RAW(sub_82679B88) {
  const uint32_t type = ctx.r4.u32, caller = uint32_t(ctx.lr);
  __imp__sub_82679B88(ctx, base);
  if (REXCVAR_GET(ds2_editor_wait_log))
    REXLOG_INFO("[editor] UI abre menu tipo {} (desde {:08X})", type, caller);
}
