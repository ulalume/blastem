#ifndef KIT_PROF_H_
#define KIT_PROF_H_

// genesis-kit host-side observation helpers. These move two kinds of measurement that
// used to cost ROM cycles (getSubTick brackets, per-frame VRAM/CRAM hashing) into the
// emulator, where they cannot perturb the deterministic emulated timing:
//
//   * PC-bracket cycle profiler: named [start,end) PC brackets registered as breakpoints;
//     per frame we emit the accumulated 68K cycles and hit count for each bracket.
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

// Register a named PC bracket. start_addr/end_addr are 68K addresses. Returns 0 on success,
// nonzero on error (no m68k context yet, duplicate name, or table full).
uint8_t kit_prof_add_bracket(const char *name, uint32_t start_addr, uint32_t end_addr);

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
// (per-frame anchor + live bracket starts) so measurements spanning a rebase stay correct.
void kit_prof_rebase(uint32_t deduction);

#endif //KIT_PROF_H_
