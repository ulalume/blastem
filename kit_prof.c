// See kit_prof.h. Host-side (zero-ROM-cost) profiling + video-memory hashing for the
// genesis-kit test harness. Because BlastEm is cycle-deterministic, everything here observes
// emulator state without ever writing it back, so it cannot change emulated results.

#include <stdio.h>
#include <string.h>

#include "kit_prof.h"
#include "vdp.h"
#include "util.h"
#include "terminal.h"
#ifdef NEW_CORE
#include "m68k.h"
#else
#include "m68k_core.h"
#endif

#define KIT_PROF_MAX_BRACKETS 8
#define KIT_PROF_NAME_LEN     32

typedef struct {
	char     name[KIT_PROF_NAME_LEN];
	uint32_t start_addr;
	uint32_t end_addr;
	uint32_t start_cycle;   // MCLK snapshot at last entry (absolute, rebase-adjusted)
	uint32_t accum_mclk;    // MCLK accumulated this frame
	uint32_t hits;          // completed brackets this frame
	uint8_t  in_use;
	uint8_t  active;        // currently inside [start,end)
} kit_bracket;

static kit_bracket brackets[KIT_PROF_MAX_BRACKETS];
static uint8_t     num_brackets;
static uint8_t     prof_log_enabled;
static uint8_t     vramhash_enabled;

static struct m68k_context *prof_m68k;

// Per-frame total anchor: absolute MCLK cycle at the previous frame emission.
static uint32_t last_frame_cycle;
static uint8_t  have_last_frame_cycle;

// Dedup guard: vdp_update_per_frame_debug() is reached from two call sites; a given frame
// number must only be emitted once.
static uint32_t kit_last_frame = 0xFFFFFFFFu;

static uint32_t clock_divider(void)
{
	if (prof_m68k && prof_m68k->opts && prof_m68k->opts->gen.clock_divider) {
		return prof_m68k->opts->gen.clock_divider;
	}
	return 7; // Genesis 68K default; matches the KDEBUG TIMER convention in vdp.c
}

// Single dispatching handler installed at both the start and end address of every bracket.
// Enter at <start>: snapshot cycles, mark active (re-entry restarts the bracket).
// Exit  at <end>:   if active, accumulate (cycles - start), bump hits, clear active.
static void kit_prof_handler(void *vcontext, uint32_t pc)
{
	struct m68k_context *context = vcontext;
	if (!prof_m68k) {
		prof_m68k = context;
	}
	for (uint8_t i = 0; i < num_brackets; i++) {
		kit_bracket *b = &brackets[i];
		if (!b->in_use) {
			continue;
		}
		if (pc == b->start_addr) {
			b->start_cycle = context->cycles;
			b->active = 1;
		} else if (pc == b->end_addr) {
			if (b->active) {
				b->accum_mclk += context->cycles - b->start_cycle;
				b->hits++;
				b->active = 0;
			}
		}
	}
}

void kit_prof_set_context(struct m68k_context *m68k)
{
	if (m68k && !prof_m68k) {
		prof_m68k = m68k;
	}
}

uint8_t kit_prof_add_bracket(const char *name, uint32_t start_addr, uint32_t end_addr)
{
	if (!prof_m68k) {
		warning("kit_prof: no m68k context (Genesis system) yet; cannot register bracket '%s'\n", name);
		return 1;
	}
	if (num_brackets >= KIT_PROF_MAX_BRACKETS) {
		warning("kit_prof: bracket table full (max %d), ignoring '%s'\n", KIT_PROF_MAX_BRACKETS, name);
		return 1;
	}
	for (uint8_t i = 0; i < num_brackets; i++) {
		if (brackets[i].in_use && !strcmp(brackets[i].name, name)) {
			warning("kit_prof: duplicate bracket name '%s'\n", name);
			return 1;
		}
	}
	kit_bracket *b = &brackets[num_brackets];
	memset(b, 0, sizeof(*b));
	strncpy(b->name, name, KIT_PROF_NAME_LEN - 1);
	b->name[KIT_PROF_NAME_LEN - 1] = 0;
	b->start_addr = start_addr;
	b->end_addr = end_addr;
	b->in_use = 1;
	num_brackets++;
	insert_breakpoint(prof_m68k, start_addr, kit_prof_handler);
	insert_breakpoint(prof_m68k, end_addr, kit_prof_handler);
	return 0;
}

void kit_prof_clear(void)
{
	if (prof_m68k) {
		for (uint8_t i = 0; i < num_brackets; i++) {
			if (brackets[i].in_use) {
				remove_breakpoint(prof_m68k, brackets[i].start_addr);
				remove_breakpoint(prof_m68k, brackets[i].end_addr);
			}
		}
	}
	memset(brackets, 0, sizeof(brackets));
	num_brackets = 0;
	have_last_frame_cycle = 0;
	// "clear" is a full profiler reset: also drop the log-enable flag so `prof clear` on its own
	// stops KIT PROF emission (VHASH is controlled separately by `vramhash off`).
	prof_log_enabled = 0;
}

void kit_prof_set_log(uint8_t on)
{
	prof_log_enabled = on ? 1 : 0;
}

void kit_prof_set_vramhash(uint8_t on)
{
	vramhash_enabled = on ? 1 : 0;
}

static void kit_emit_line(const char *line)
{
	// Same branch as the KDEBUG output in vdp.c so the genesis-kit log pipeline sees it,
	// and so gdb-remote (pipe) stays clean when stdout messages are disabled.
	if (is_stdout_enabled()) {
		init_terminal();
		printf("%s\n", line);
		fflush(stdout);
	} else {
		fprintf(stderr, "%s\n", line);
	}
}

static uint32_t fnv1a(uint32_t hash, const uint8_t *data, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		hash ^= data[i];
		hash *= 16777619u;
	}
	return hash;
}

static void emit_vramhash(struct vdp_context *vdp)
{
	// FNV-1a 32-bit over: vdpmem[0..VRAM_SIZE) bytes, then each cram[0..CRAM_SIZE) word
	// (low byte then high byte), then each vsram[0..MAX_VSRAM_SIZE) word (low then high).
	uint32_t hash = 2166136261u;
	hash = fnv1a(hash, vdp->vdpmem, VRAM_SIZE);
	for (uint32_t i = 0; i < CRAM_SIZE; i++) {
		uint16_t w = vdp->cram[i];
		uint8_t bytes[2] = { (uint8_t)(w & 0xFF), (uint8_t)(w >> 8) };
		hash = fnv1a(hash, bytes, 2);
	}
	for (uint32_t i = 0; i < MAX_VSRAM_SIZE; i++) {
		uint16_t w = vdp->vsram[i];
		uint8_t bytes[2] = { (uint8_t)(w & 0xFF), (uint8_t)(w >> 8) };
		hash = fnv1a(hash, bytes, 2);
	}
	char line[64];
	snprintf(line, sizeof(line), "KIT VHASH frame=%u hash=%08x", vdp->frame, hash);
	kit_emit_line(line);
}

void kit_prof_frame(struct vdp_context *vdp)
{
	if (vdp->frame == kit_last_frame) {
		return; // already handled this frame from the other call site
	}
	kit_last_frame = vdp->frame;

	if (vramhash_enabled) {
		emit_vramhash(vdp);
	}

	// Profiler bookkeeping runs whenever brackets exist (so the anchor stays fresh and the
	// per-frame accumulators never grow across frames), or when logging a total-only line.
	if (prof_log_enabled || num_brackets) {
		uint32_t div = clock_divider();
		uint32_t total68k = 0;
		if (prof_m68k) {
			uint32_t cur = prof_m68k->cycles;
			if (have_last_frame_cycle) {
				total68k = (cur - last_frame_cycle) / div;
			}
			last_frame_cycle = cur;
			have_last_frame_cycle = 1;
		}

		if (prof_log_enabled) {
			char line[1024];
			int off = snprintf(line, sizeof(line), "KIT PROF frame=%u total=%u", vdp->frame, total68k);
			for (uint8_t i = 0; i < num_brackets && off > 0 && off < (int)sizeof(line); i++) {
				kit_bracket *b = &brackets[i];
				if (!b->in_use) {
					continue;
				}
				off += snprintf(line + off, sizeof(line) - off, " %s=%u:%u",
					b->name, b->accum_mclk / div, b->hits);
			}
			kit_emit_line(line);
		}

		// Roll over the per-frame accumulators (active brackets keep their live start_cycle
		// so a bracket that straddles the frame boundary is attributed to its exit frame).
		for (uint8_t i = 0; i < num_brackets; i++) {
			brackets[i].accum_mclk = 0;
			brackets[i].hits = 0;
		}
	}
}

void kit_prof_rebase(uint32_t deduction)
{
	// The shared MCLK counter just had "deduction" subtracted. Re-base our saved absolute
	// snapshots by the same amount so per-frame totals and in-flight brackets stay correct.
	if (have_last_frame_cycle) {
		last_frame_cycle -= deduction;
	}
	for (uint8_t i = 0; i < num_brackets; i++) {
		if (brackets[i].in_use && brackets[i].active) {
			brackets[i].start_cycle -= deduction;
		}
	}
}
