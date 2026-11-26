#ifndef KIT_PROF_H_
#define KIT_PROF_H_

// host-side observation helpers. These move two kinds of measurement that
// used to cost ROM cycles (getSubTick brackets, per-frame VRAM/CRAM hashing) into the
// emulator, where they cannot perturb the deterministic emulated timing:
//
//   * PC-bracket cycle profiler: named [start,end) PC brackets registered as breakpoints;
//     per frame we emit the accumulated 68K cycles and hit count for each bracket.
//   * PC-lap timer: the same table and the same per-frame line, but measuring the interval
//     between consecutive passes of ONE address (a main-loop marker), so no second address
//     has to be recovered from a disassembly.
//   * per-frame VRAM/CRAM/VSRAM hash: an FNV-1a fingerprint of video memory each frame.
//
// Driven over the control socket (see ctrl_fifo.c). No license header, matching the other
// fork-added source files (the fork as a whole is dedicated to the public domain, COPYING.fork).

#include <stdint.h>

struct m68k_context;
struct vdp_context;

// Provide the m68k context used for cycle accounting and breakpoint registration. Null-safe;
// the pointer is remembered the first time a non-null value is supplied.
void kit_prof_set_context(struct m68k_context *m68k);

#define KIT_PROF_MAX_ENDS 8

// Register a named PC bracket: from passing start_addr to passing any of end_addrs (a function
// with two rts, or one that leaves by a tail jmp, has several exits). 68K addresses. Returns 0 on
// success, nonzero on error (no m68k context yet, duplicate name, table full, 0 or too many ends).
uint8_t kit_prof_add_bracket(const char *name, uint32_t start_addr, const uint32_t *end_addrs, uint8_t num_ends);

// Register a named function bracket: from passing entry_addr until that call returns, i.e. until
// the PC reaches the return address that was on the stack at entry with a7 back above it. Covers
// every rts and tail-call jmp exit without listing them. A nested entry (recursion, an interrupt
// calling it) counts inside the outer call. Same table, line format and errors as a bracket.
uint8_t kit_prof_add_func(const char *name, uint32_t entry_addr);

// Register a named PC lap: the 68K cycles between consecutive executions of addr. Reported in the
// same "KIT PROF" line and the same "<name>=<cycles>:<hits>" shape as a bracket, so hits==0 means
// the address was not reached during that video frame (a dropped frame for a once-per-loop marker).
// Shares the bracket table; same error cases and return convention as kit_prof_add_bracket.
uint8_t kit_prof_add_lap(const char *name, uint32_t addr);

// Remove every bracket (and its breakpoints) and reset per-frame accumulation state.
void kit_prof_clear(void);

// Enable/disable per-frame "KIT PROF" emission (default off; registering brackets does not print).
void kit_prof_set_log(uint8_t on);

// Enable/disable per-frame "KIT VHASH" emission (default off).
void kit_prof_set_vramhash(uint8_t on);

// Per-frame hook, called from vdp_update_per_frame_debug(). Emits the enabled lines and rolls
// over the per-frame accumulators. Guards against being called twice for the same frame.
void kit_prof_frame(struct vdp_context *vdp);

// Cycle-rebase hook, called from the genesis.c sync sites immediately where the shared MCLK
// cycle counter has "deduction" subtracted from it. Re-aligns any saved absolute cycle snapshots
// (per-frame anchor + live bracket starts + HUD idle/window snapshots) so measurements spanning a
// rebase stay correct.
void kit_prof_rebase(uint32_t deduction);

// ---------------------------------------------------------------------------
// FEATURE A: HUD (Game FPS + CPU%) -- see PATCHES.md.
// Driven over the control socket ("hud ..."). All measurement is host-side: PC sampling and
// breakpoints never change the emulated cycle counter, so enabling the HUD cannot perturb output.
// ---------------------------------------------------------------------------

// Master switch. kit_hud_on() defaults idle accounting to auto-discovery when no idle mode was
// explicitly chosen. kit_hud_off() clears the HUD title suffix and removes the idle/marker breakpoints.
void kit_hud_on(void);
void kit_hud_off(void);

// Optional exact game-frame marker: a PC executed exactly once per main-loop iteration. Pass 0 to
// clear the marker (fall back to the gap heuristic).
void kit_hud_set_gamefps(uint32_t addr);

// Idle-loop address selection. Manual pins one address; auto continuously PC-samples and re-arms.
void kit_hud_set_idle_manual(uint32_t addr);
void kit_hud_set_idle_auto(void);

// Obsolete (the spin-frame heuristic needs no threshold); accepted and ignored with a warning.
void kit_hud_set_fpsgap(uint32_t cycles68k);

// Once-per-scanline hook (vdp.c advance_output_line): rotating-line PC sampling for the auto
// idle detector — time-weighted, unlike frame-push-moment sampling which lands in the vint
// handler. Cheap early-outs when the HUD/auto mode is off.
void kit_prof_scanline(struct vdp_context *vdp);

// Current HUD title suffix ("Game 8.5 fps / CPU 92%"), or "" when the HUD is off. Read from the
// SDL render thread at the caption-refresh site.
const char *kit_prof_hud_text(void);

// ---------------------------------------------------------------------------
// FEATURE B: watchlog (cycle-stamped RAM write logging) -- NEW_CORE interpreter only.
// ---------------------------------------------------------------------------

// Number of armed watchpoints. Read directly (cheap guard) by the interpreter write path in
// m68k_util.c: `if (kit_watch_count) kit_watch_check(...)`. Zero when nothing is watched.
extern uint32_t kit_watch_count;

// Add/remove watched byte addresses (up to 8). kit_watch_check() is the write-path hook: it emits a
// KIT WATCH line when [addr, addr+size) covers any watched byte.
void kit_watch_add(uint32_t addr);
void kit_watch_clear(void);
void kit_watch_check(struct m68k_context *context, uint32_t addr, uint32_t val, uint8_t size);

// ---------------------------------------------------------------------------
// FEATURE C: vramdump (one-shot binary VRAM/CRAM/VSRAM/regs snapshot).
// ---------------------------------------------------------------------------

// Arm a one-shot dump: at the next frame boundary (see kit_prof_frame), write a KITVDMP1 binary
// snapshot to <path> (see the format comment above kit_prof_do_vramdump() in kit_prof.c) and emit
// "KIT VDUMP frame=<N> file=<path>". Async: this call only records the request and returns
// immediately; the file appears within a frame. A null/empty path cancels any pending dump. A
// second call before the pending dump is serviced replaces the path (only the latest one fires).
// Zero cost when never called: kit_prof_frame's pending check is a single null-pointer test.
void kit_prof_request_vramdump(const char *path);

// ---------------------------------------------------------------------------
// FEATURE D: prof count (per-instruction execution counts) -- NEW_CORE interpreter only.
// ---------------------------------------------------------------------------
//
//   prof count <start_hex> <end_hex> [<path>]
//       Count how many times each instruction address in [start, end) is executed, from now on,
//       in one run. Re-arming resets the counts. With <path>, the counts are also written there
//       when blastem exits (run_live.sh --kill, window close).
//   prof countdump <path>
//       Write the counts now. Paused at `scriptend hold`, that is a fixed frame.
//   prof count off   /   prof clear
//       Stop counting and free the table (`prof clear` resets the whole profiler).
//
// Dump format: one header line, then one line per address executed at least once, ascending:
//   # KIT COUNT start=<hex> end=<hex> frame=<N>
//   <addr_hex>\t<count>
// An address not listed was not executed (or is not an instruction start). Prints
// "KIT COUNT frame=<N> file=<path> addrs=<lines> total=<sum of counts>".
//
// Cost: while armed, the interpreter runs its breakpoint loop (as with any `prof bracket`) plus
// one compare and one increment per instruction. Memory: one 64-bit counter per 16-bit word of the
// range (a 4 MB ROM: 16 MB). Counting never writes emulator state, so the emulated run is the same
// with it on or off. The x86 JIT core (x86_64 / i686 builds) has no per-instruction loop: there the
// command is refused with a warning.

// Read by the interpreter loop (m68k_util.c / cpu_dsl.py): the counted range, its length in bytes
// (0 = off), and one 64-bit counter per 16-bit word.
extern uint32_t kit_count_start;
extern uint32_t kit_count_len;
extern uint64_t *kit_count_tab;

static inline void kit_count_pc(uint32_t pc)
{
	uint32_t off = pc - kit_count_start;
	if (off < kit_count_len) {
		kit_count_tab[off >> 1]++;
	}
}

// Arm counting over [start, end) (resets the counts). path: write at exit too (NULL: don't).
// Returns 0 on success, nonzero on error (empty or too large range, JIT build, out of memory).
uint8_t kit_count_arm(uint32_t start, uint32_t end, const char *path);
void kit_count_off(void);
// Write the counts to path now. Returns 0 on success.
uint8_t kit_count_dump(const char *path);

// Print one "KIT ..." line on the same stream as the KDEBUG output in vdp.c.
void kit_emit_line(const char *line);

#endif //KIT_PROF_H_
