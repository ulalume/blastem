#ifndef KIT_SCRIPT_H_
#define KIT_SCRIPT_H_

// Frame-scripted input for a host-side test harness. Pad changes and screenshots are keyed to
// the emulated video-frame counter instead of host wall-clock, so the same script replayed on
// the same ROM produces byte-identical PNGs. Driven over the control socket (ctrl_fifo.c:
// "script <path>", "frame", "logframes on|off").
//
// Script file: one action per line, "#" comments and blank lines ignored.
//
//   <frame> down <port> <buttons>    press (buttons: comma-separated, e.g. "a" or "up,a")
//   <frame> up <port> <buttons>      release
//   <frame> shot <path>              save the frame rendered as <frame>
//   <frame> burst <count> <prefix>   save <count> consecutive frames from <frame>
//   <frame> log <text>               echo "KIT SCRIPT frame=<N> ... log <text>"
//   <frame> vramdump <path>          KITVDMP1 snapshot of the end of <frame> (the state its KIT VHASH
//                                    line hashes and its shot shows); one per frame
//   anchor <text>                    re-origin the frames BELOW this line on the frame in which
//                                    the ROM's KDEBUG output first contains <text>
//
// <frame> counts video frames since reset and must be non-decreasing within its section; equal
// frames run in file order. Lines above an "anchor" are absolute and exist to reach the anchor;
// any of them still pending when the anchor fires are dropped. An action whose frame has already
// passed when the script loads fires at once and reports "KIT SCRIPT warn" -- that run is no
// longer reproducible. A script loaded before the first instruction (BLASTEM_CTRL_INIT) is never
// late: its frame-0 actions apply before frame 0 is emulated.
//
// "scriptend hold [T]" (kit_script_set_hold): once the last action has run and the last frame it
// captures (a shot's frame, a burst's last frame) plus T more frames (default 0) have been
// emulated, print "KIT SCRIPT hold frame=<N>" and pause; "resume" continues. The log then ends at
// frame N on every run, however long the host takes to notice the end.

#include <stdint.h>

// Parse one gamepad button name ("up", "a", "start", ...). BUTTON_INVALID when unknown.
uint8_t kit_parse_button(const char *name);

// Load (and replace) the pending script. A null/empty path clears it. Returns 0 on success.
uint8_t kit_script_load(const char *path);

// Per-frame hook, called from vdp_update_per_frame_debug() with the frame that just finished.
void kit_script_frame(uint32_t finished_frame);

// KDEBUG hook, called from vdp.c with each completed ROM message: arms the anchor.
void kit_script_kdebug(const char *msg, uint32_t frame);

// Emit "KIT FRAME frame=<N>" for the frame currently being rendered.
void kit_script_report_frame(void);

// Append " @f<N>" to KDEBUG MESSAGE lines (vdp.c) so ROM output carries the frame it came from.
void kit_script_set_logframes(uint8_t on);
extern uint8_t kit_logframes;

// Sticky "scriptend hold [tail]|run" setting (default run). See above.
void kit_script_set_hold(uint8_t on, uint32_t tail_frames);

// Leave a pause (a "scriptend hold" or a "pause" command): prints "KIT RESUME frame=<N>".
void kit_script_resume(void);

// Supply the screenshot entry points. Called from ctrl_fifo.c: render_save_screenshot lives in
// the SDL/fbdev renderers, which the core-only link (libblastem) does not have.
void kit_script_set_capture(void (*shot)(char *path), void (*burst)(char *prefix, uint32_t count));

#endif //KIT_SCRIPT_H_
