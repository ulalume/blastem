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

// Most-recently processed frame number, tagged onto KIT WATCH / KIT HUD lines. Updated once per
// deduped frame; RAM writes inside frame N are attributed to the boundary number seen at N's start.
static uint32_t kit_cur_frame;

// ---- FEATURE A: HUD state --------------------------------------------------
#define HUD_WINDOW_FRAMES 60   // rollup cadence (== 1s on NTSC)
#define HUD_CALIB_FRAMES  120  // PC-sampling calibration window (~2s on NTSC)
#define HUD_HIST_SIZE     16   // distinct sampled addresses tracked per calibration window
#define HUD_CLUSTER_RADIUS 32  // bytes: samples this close count as one wait loop (loops are tiny)
#define HUD_IDLE_GAP_68K  256  // <= this many 68K cycles between idle hits == one loop iteration

static uint8_t  hud_enabled;
static uint8_t  hud_idle_auto = 1;  // 1 = auto-discover idle loop, 0 = manual/pinned. Default: auto.

static uint32_t hud_idle_addr;      // armed idle breakpoint address (valid when hud_idle_armed)
static uint8_t  hud_idle_armed;     // an idle breakpoint is currently installed
static uint8_t  hud_auto_calibrated;// at least one auto calibration window has completed

static uint32_t hud_marker_addr;    // armed game-frame marker address (valid when hud_marker_armed)
static uint8_t  hud_marker_armed;

// GAP-method idle accounting. last_idle_hit is an absolute MCLK snapshot -> rebased.
static uint32_t last_idle_hit;
static uint8_t  have_last_idle_hit;
static uint32_t idle_accum;          // MCLK spent looping in the idle address this window
static uint32_t heuristic_frame_count; // spin-frames this window (no-marker FPS fallback):
                                       // a frame counts once if the idle loop actually SPUN
                                       // (>=1 small-gap iteration) during it. A 60fps game spins
                                       // every vblank period -> 60; a 3-vblank game spins in 1 of
                                       // 3 -> 20. Threshold-free (replaced the old fpsgap rule,
                                       // which misread light games and interrupt-handler gaps).
static uint8_t  idle_spun_this_frame;
static uint32_t marker_hits;         // marker breakpoint hits this window

static uint32_t idle_gap_max_mclk;   // HUD_IDLE_GAP_68K * divider (MCLK, precomputed)

// Tier-3 Game-FPS fallback: VRAM-change rate. Used ONLY when there is neither a marker nor an
// armed idle loop (i.e. a game that never waits, like a heavy renderer at CPU 100%) — for such a
// game "the screen changed" ≈ "a frame was rendered". Games that DO wait are handled by the more
// accurate spin-frame tier first, which is what makes the old "pause screen reads 0fps" flaw of
// pure VRAM-counting structurally impossible here (a paused game waits -> spin tier answers).
// Remaining honest limit: CPU-100% + genuinely static output reads low (hence the ~ prefix).
static uint32_t vchange_prev_hash;
static uint8_t  have_vchange_prev;
static uint32_t vchange_count; // VRAM/CRAM/VSRAM changes seen this window

// Rotating-scanline PC sampling (auto idle detection). Sampling at the frame-push moment is
// systematically biased INTO the vint handler (the push happens a few lines after vint while the
// handler is still running — observed: an idle menu "detected" JOY_update). Instead we sample once
// per frame on a rotating target line (89 is coprime with 224, so 224 frames cover every line),
// which time-weights the samples across the whole frame.
static uint16_t hud_target_line;
static uint32_t line_sampled_frame = 0xFFFFFFFFu;

static uint32_t hud_window_start_cycle; // absolute MCLK at window start -> rebased
static uint8_t  have_window_start;
static uint32_t hud_window_frames;      // frames elapsed in the current rollup window

// Continuous auto-calibration histogram (plain arrays, no allocation).
static uint32_t hist_addr[HUD_HIST_SIZE];
static uint32_t hist_cnt[HUD_HIST_SIZE];
static uint8_t  hist_used;
static uint32_t hist_total;
static uint32_t hud_calib_frames;

static char hud_text[96];   // title suffix, kept in sync at each window rollup

// ---- FEATURE B: watchlog state ---------------------------------------------
#define KIT_WATCH_MAX       8
#define KIT_WATCH_FRAME_CAP 64  // max KIT WATCH lines emitted per frame before suppression

uint32_t        kit_watch_count;   // exported: cheap guard read by the interpreter write path
static uint32_t kit_watch_addr[KIT_WATCH_MAX];
static uint32_t kit_watch_emitted;    // lines emitted this frame
static uint32_t kit_watch_suppressed; // lines suppressed this frame (over cap)

static uint32_t clock_divider(void)
{
	if (prof_m68k && prof_m68k->opts && prof_m68k->opts->gen.clock_divider) {
		return prof_m68k->opts->gen.clock_divider;
	}
	return 7; // Genesis 68K default; matches the KDEBUG TIMER convention in vdp.c
}

// Program counter of an m68k context. Only the NEW_CORE interpreter exposes a live ->pc field; the
// JIT struct has no equivalent, so on that (non-runtime) build we degrade to 0. HUD auto-detect and
// watchlog PC stamping are NEW_CORE features; this keeps kit_prof.o compiling in both cores.
static uint32_t kit_ctx_pc(struct m68k_context *c)
{
#ifdef NEW_CORE
	return c->pc;
#else
	(void)c;
	return 0;
#endif
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

// FNV-1a 32-bit over: vdpmem[0..VRAM_SIZE) bytes, then each cram[0..CRAM_SIZE) word
// (low byte then high byte), then each vsram[0..MAX_VSRAM_SIZE) word (low then high).
static uint32_t compute_vramhash(struct vdp_context *vdp)
{
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
	return hash;
}

static void emit_vramhash(struct vdp_context *vdp)
{
	char line[64];
	snprintf(line, sizeof(line), "KIT VHASH frame=%u hash=%08x", vdp->frame, compute_vramhash(vdp));
	kit_emit_line(line);
}

// ===========================================================================
// FEATURE A: HUD -- Game FPS + CPU%.
// ===========================================================================

static void kit_hud_recompute_thresholds(void)
{
	uint32_t div = clock_divider();
	idle_gap_max_mclk = (uint32_t)HUD_IDLE_GAP_68K * div;
}

// GAP method: fires once per idle-loop iteration. A short delta since the previous hit means the
// CPU just went round the wait loop (idle time); a large delta means it left the loop to do real
// work or serviced an interrupt (excluded from idle -- this is why one address suffices and
// interrupt-handler time never counts as idle). Spinning also stamps idle_spun_this_frame, which
// kit_hud_frame folds into the threshold-free spin-frame FPS fallback (see heuristic_frame_count).
// last_idle_hit is an absolute MCLK snapshot, rebased in kit_prof_rebase().
static void kit_hud_idle_handler(void *vcontext, uint32_t pc)
{
	struct m68k_context *context = vcontext;
	(void)pc;
	if (!prof_m68k) {
		prof_m68k = context;
	}
	uint32_t now = context->cycles;
	if (have_last_idle_hit) {
		uint32_t delta = now - last_idle_hit; // MCLK, wraparound-safe
		if (delta <= idle_gap_max_mclk) {
			idle_accum += delta;
			idle_spun_this_frame = 1; // the loop actually spun this frame (see heuristic_frame_count)
		}
	} else {
		have_last_idle_hit = 1;
	}
	last_idle_hit = now;
}

static void kit_hud_marker_handler(void *vcontext, uint32_t pc)
{
	(void)vcontext;
	(void)pc;
	marker_hits++;
}

static void kit_hud_remove_idle_bp(void)
{
	if (hud_idle_armed && prof_m68k) {
		remove_breakpoint(prof_m68k, hud_idle_addr);
	}
	hud_idle_armed = 0;
	hud_idle_addr = 0;
	have_last_idle_hit = 0;
}

static void kit_hud_install_idle_bp(uint32_t addr)
{
	kit_hud_remove_idle_bp();
	if (addr && prof_m68k) {
		insert_breakpoint(prof_m68k, addr, kit_hud_idle_handler);
		hud_idle_addr = addr;
		hud_idle_armed = 1;
	}
}

static void kit_hud_install_marker_bp(uint32_t addr)
{
	if (hud_marker_armed && prof_m68k) {
		remove_breakpoint(prof_m68k, hud_marker_addr);
	}
	hud_marker_armed = 0;
	hud_marker_addr = 0;
	if (addr && prof_m68k) {
		insert_breakpoint(prof_m68k, addr, kit_hud_marker_handler);
		hud_marker_addr = addr;
		hud_marker_armed = 1;
	}
}

// One PC sample per frame into the calibration histogram. At the end of each 120-frame window, if a
// single address holds >= 50% of samples it becomes the armed idle address (re-arming if it moved);
// otherwise the idle breakpoint is disarmed (the game isn't idling). Sampling continues across
// windows so a scene change with a different wait loop re-calibrates within ~2s.
static void kit_hud_sample_pc(void)
{
	if (!prof_m68k) {
		return;
	}
	uint32_t pc = kit_ctx_pc(prof_m68k);
	int slot = -1;
	for (uint8_t i = 0; i < hist_used; i++) {
		if (hist_addr[i] == pc) {
			slot = i;
			break;
		}
	}
	if (slot < 0 && hist_used < HUD_HIST_SIZE) {
		slot = hist_used++;
		hist_addr[slot] = pc;
		hist_cnt[slot] = 0;
	}
	if (slot >= 0) {
		hist_cnt[slot]++;
	}
	hist_total++;

	if (++hud_calib_frames >= HUD_CALIB_FRAMES) {
		// A busy-wait loop spans several instructions, so vblank-moment samples spread across
		// 2-5 nearby addresses and no SINGLE one need reach 50% even on a fully idle game.
		// Judge by CLUSTER: for each sampled address, sum the counts of all samples within
		// +/-HUD_CLUSTER_RADIUS bytes; the dominant cluster decides, and we arm the cluster's
		// modal address (any in-loop instruction hits every iteration, so the mode suffices).
		uint32_t best = 0, best_cluster = 0, best_self = 0;
		for (uint8_t i = 0; i < hist_used; i++) {
			uint32_t cluster = 0;
			for (uint8_t j = 0; j < hist_used; j++) {
				uint32_t d = hist_addr[i] > hist_addr[j] ? hist_addr[i] - hist_addr[j]
																								 : hist_addr[j] - hist_addr[i];
				if (d <= HUD_CLUSTER_RADIUS) {
					cluster += hist_cnt[j];
				}
			}
			if (cluster > best_cluster || (cluster == best_cluster && hist_cnt[i] > best_self)) {
				best_cluster = cluster;
				best_self = hist_cnt[i];
				best = hist_addr[i];
			}
		}
		if (hist_total && best_cluster * 2 >= hist_total) {
			if (!hud_idle_armed || hud_idle_addr != best) {
				kit_hud_install_idle_bp(best);
			}
		} else {
			if (hud_idle_armed) {
				kit_hud_remove_idle_bp();
			}
		}
		hud_auto_calibrated = 1;
		hist_used = 0;
		hist_total = 0;
		hud_calib_frames = 0;
	}
}

// Window rollup: fold the 60-frame accumulators into game_fps + cpu_pct, refresh the title suffix,
// emit a KIT HUD line, and zero the window.
static void kit_hud_rollup(uint32_t frame)
{
	uint32_t total_window = 0;
	if (prof_m68k && have_window_start) {
		total_window = prof_m68k->cycles - hud_window_start_cycle; // MCLK, wraparound-safe
	}
	uint32_t frames = hud_window_frames ? hud_window_frames : 1;
	// Game FPS source: marker (exact) > gap heuristic (needs an ARMED idle bp — the
	// heuristic counter only ticks in the idle handler) > unknown. Without either,
	// 0.0 would be a lie; show "--" instead.
	uint8_t heuristic = 0, have_fps = 1;
	uint32_t count = 0;
	const char *src;
	if (hud_marker_armed) {
		count = marker_hits;
		src = "marker";
	} else if (hud_idle_armed) {
		count = heuristic_frame_count;
		heuristic = 1;
		src = "gap";
	} else if (have_vchange_prev) {
		count = vchange_count; // tier-3: VRAM-change rate (never-waiting game)
		heuristic = 1;
		src = "vchange";
	} else {
		have_fps = 0;
		src = "none";
	}
	float game_fps = have_fps ? (float)count * (float)HUD_WINDOW_FRAMES / (float)frames : -1.0f;

	int cpu_pct;
	if (hud_idle_armed && total_window) {
		uint64_t idle_pct = (uint64_t)idle_accum * 100u / total_window;
		int c = 100 - (int)idle_pct;
		if (c < 0) {
			c = 0;
		}
		if (c > 100) {
			c = 100;
		}
		cpu_pct = c;
	} else if (hud_idle_auto && !hud_auto_calibrated) {
		cpu_pct = -1; // still calibrating -> display "--"
	} else {
		cpu_pct = 100; // no idle loop armed -> treat as fully busy
	}

	char idlebuf[16];
	const char *idle_field;
	if (hud_idle_armed) {
		snprintf(idlebuf, sizeof(idlebuf), "%06x", hud_idle_addr & 0xFFFFFF);
		idle_field = idlebuf;
	} else if (hud_idle_auto && !hud_auto_calibrated) {
		idle_field = "auto-pending";
	} else {
		idle_field = "none";
	}

	char fpsbuf[24];
	if (have_fps) {
		snprintf(fpsbuf, sizeof(fpsbuf), "%sGame %.1f fps", heuristic ? "~" : "", game_fps);
	} else {
		snprintf(fpsbuf, sizeof(fpsbuf), "Game -- fps");
	}
	if (cpu_pct < 0) {
		snprintf(hud_text, sizeof(hud_text), "%s / CPU --%%", fpsbuf);
	} else {
		snprintf(hud_text, sizeof(hud_text), "%s / CPU %d%%", fpsbuf, cpu_pct);
	}

	char line[160];
	snprintf(line, sizeof(line), "KIT HUD frame=%u game_fps=%.1f cpu=%d src=%s idle=%s",
		frame, game_fps, cpu_pct, src, idle_field);
	kit_emit_line(line);

	idle_accum = 0;
	heuristic_frame_count = 0;
	marker_hits = 0;
	vchange_count = 0;
	hud_window_frames = 0;
	if (prof_m68k) {
		hud_window_start_cycle = prof_m68k->cycles;
		have_window_start = 1;
	}
}

static void kit_hud_frame(struct vdp_context *vdp)
{
	uint32_t frame = vdp->frame;
	// PC sampling moved to kit_prof_scanline() (rotating target line) — sampling here, at the
	// frame-push moment, was systematically biased into the vint handler.
	if (idle_spun_this_frame) {
		heuristic_frame_count++; // spin-frame FPS fallback: this vblank period saw real waiting
		idle_spun_this_frame = 0;
	}
	// Tier-3 fallback (see vchange_count): only when neither better source exists.
	if (!hud_marker_armed && !hud_idle_armed) {
		uint32_t h = compute_vramhash(vdp);
		if (have_vchange_prev && h != vchange_prev_hash) {
			vchange_count++;
		}
		vchange_prev_hash = h;
		have_vchange_prev = 1;
	}
	hud_window_frames++;
	if (hud_window_frames >= HUD_WINDOW_FRAMES) {
		kit_hud_rollup(frame);
	}
}

// Called once per output scanline from vdp.c's advance_output_line(). Samples the 68K PC on one
// rotating target line per frame (89 is coprime with 224 -> every line gets sampled over 224
// frames), giving a time-weighted picture of where the CPU spends its frame.
void kit_prof_scanline(struct vdp_context *vdp)
{
	if (!hud_enabled || !hud_idle_auto || !prof_m68k) {
		return;
	}
	if (vdp->vcounter != hud_target_line || line_sampled_frame == vdp->frame) {
		return;
	}
	line_sampled_frame = vdp->frame;
	hud_target_line = (uint16_t)((hud_target_line + 89u) % 224u);
	kit_hud_sample_pc();
}

void kit_hud_on(void)
{
	hud_enabled = 1;
	kit_hud_recompute_thresholds();
	idle_accum = 0;
	heuristic_frame_count = 0;
	marker_hits = 0;
	vchange_count = 0;
	hud_window_frames = 0;
	have_last_idle_hit = 0;
	if (prof_m68k) {
		hud_window_start_cycle = prof_m68k->cycles;
		have_window_start = 1;
	} else {
		have_window_start = 0;
	}
	// Default idle accounting is auto-discovery unless the user pinned a manual address.
	if (hud_idle_auto) {
		hist_used = 0;
		hist_total = 0;
		hud_calib_frames = 0;
		hud_auto_calibrated = 0;
	}
}

void kit_hud_off(void)
{
	hud_enabled = 0;
	hud_text[0] = 0;
	kit_hud_remove_idle_bp();
	kit_hud_install_marker_bp(0);
}

void kit_hud_set_gamefps(uint32_t addr)
{
	kit_hud_install_marker_bp(addr);
	marker_hits = 0;
}

void kit_hud_set_idle_manual(uint32_t addr)
{
	hud_idle_auto = 0;
	kit_hud_install_idle_bp(addr);
	idle_accum = 0;
}

void kit_hud_set_idle_auto(void)
{
	hud_idle_auto = 1;
	kit_hud_remove_idle_bp();
	hist_used = 0;
	hist_total = 0;
	hud_calib_frames = 0;
	hud_auto_calibrated = 0;
	idle_accum = 0;
}

void kit_hud_set_fpsgap(uint32_t cycles68k)
{
	// Obsolete: the spin-frame heuristic replaced the gap threshold (which misread light games
	// and interrupt-handler gaps). Kept so older kit tools sending `hud fpsgap N` stay harmless.
	(void)cycles68k;
	warning("hud fpsgap is obsolete (spin-frame heuristic needs no threshold); ignored\n");
}

const char *kit_prof_hud_text(void)
{
	return hud_enabled ? hud_text : "";
}

// ===========================================================================
// FEATURE B: watchlog -- cycle-stamped RAM write logging (NEW_CORE write path).
// ===========================================================================

void kit_watch_add(uint32_t addr)
{
	if (kit_watch_count >= KIT_WATCH_MAX) {
		warning("kit_watch: table full (max %d), ignoring %06x\n", KIT_WATCH_MAX, addr);
		return;
	}
	for (uint32_t i = 0; i < kit_watch_count; i++) {
		if (kit_watch_addr[i] == addr) {
			return; // already watched
		}
	}
	kit_watch_addr[kit_watch_count++] = addr;
}

void kit_watch_clear(void)
{
	kit_watch_count = 0;
	kit_watch_emitted = 0;
	kit_watch_suppressed = 0;
}

// Called from the interpreter write helpers (m68k_util.c) only when kit_watch_count != 0. A watched
// byte W matches a write to [addr, addr+size). PC is context->pc at write time: in the NEW_CORE
// interpreter that is the prefetch pointer (a couple of bytes past the writing instruction's start),
// so treat it as approximate. cycle is reported in 68K cycles (MCLK / divider).
void kit_watch_check(struct m68k_context *context, uint32_t addr, uint32_t val, uint8_t size)
{
	for (uint32_t i = 0; i < kit_watch_count; i++) {
		uint32_t w = kit_watch_addr[i];
		if (w < addr || w >= addr + size) {
			continue;
		}
		if (kit_watch_emitted >= KIT_WATCH_FRAME_CAP) {
			kit_watch_suppressed++;
			continue;
		}
		uint32_t div = (context->opts && context->opts->gen.clock_divider)
			? context->opts->gen.clock_divider : 7;
		uint32_t maskval = (size == 2) ? (val & 0xFFFF) : (val & 0xFF);
		char line[96];
		snprintf(line, sizeof(line),
			"KIT WATCH frame=%u cycle=%u pc=%06x addr=%06x val=%04x w%u",
			kit_cur_frame, context->cycles / div, kit_ctx_pc(context) & 0xFFFFFF,
			addr & 0xFFFFFF, maskval, (unsigned)(size * 8));
		kit_emit_line(line);
		kit_watch_emitted++;
	}
}

void kit_prof_frame(struct vdp_context *vdp)
{
	if (vdp->frame == kit_last_frame) {
		return; // already handled this frame from the other call site
	}
	kit_last_frame = vdp->frame;
	kit_cur_frame = vdp->frame;

	// watchlog per-frame rollup: flush the suppression tally, then reset the per-frame caps.
	if (kit_watch_suppressed) {
		char line[64];
		snprintf(line, sizeof(line), "KIT WATCH suppressed=%u frame=%u", kit_watch_suppressed, vdp->frame);
		kit_emit_line(line);
	}
	kit_watch_emitted = 0;
	kit_watch_suppressed = 0;

	if (hud_enabled) {
		kit_hud_frame(vdp);
	}

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
	// HUD absolute snapshots.
	if (have_last_idle_hit) {
		last_idle_hit -= deduction;
	}
	if (have_window_start) {
		hud_window_start_cycle -= deduction;
	}
}
