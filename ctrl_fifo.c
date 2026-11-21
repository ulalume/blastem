// External control socket for scripted/headless use. When the environment variable
// BLASTEM_CTRL_SOCK is set to a path, blastem binds a Unix-domain stream socket there and
// polls it once per frame for newline-terminated commands, so an external process can drive
// the emulated gamepad and trigger screenshots without window focus or synthetic key events:
//
//   pad <num> down <button>   press a pad button (num matches the gamepad
//   pad <num> up <button>     number in the io config, normally 1 or 2)
//   screenshot <path>         save the next rendered frame (.png or .ppm)
//   script <path>             load a frame-scripted input file: "<frame> down|up|shot|burst|log"
//                             lines applied at exact video frames, so a driving sequence and
//                             its screenshots replay identically run to run (see kit_script.h)
//   frame                     print "KIT FRAME frame=<N>" for the frame being rendered
//   logframes on|off          tag KDEBUG MESSAGE lines with the frame they came from
//   burst <count> <prefix>    save <count> CONSECUTIVE frames to <prefix>00000.png, ...
//                             ("burst 0" cancels). Every frame is captured, unlike a loop of
//                             screenshot commands, which only samples what the socket
//                             round-trip happens to land on.
//   vramdump <path>           dump a binary VRAM/CRAM/VSRAM/regs snapshot at the next frame
//                             boundary (KITVDMP1 format; see kit_prof.c for the file layout)
//   speed max|<percent>       emulation speed: "max" never waits for the audio device (as fast as
//                             the host allows; audio is dropped), a percentage is the existing
//                             speed setting (100 = real time). Emulated results are the same at
//                             any speed; only wall-clock timing changes. Needs sync_source audio.
//   scriptend hold [N]|run    what happens when a frame script has run its last action: "hold"
//                             pauses emulation once the frame of the last action (or the last
//                             frame of a burst) and N more frames (default 0) have been emulated,
//                             so the log ends at a fixed frame; "run" (default) keeps going.
//                             Sticky until changed.
//   pause / resume            pause or resume emulation; the socket stays live while paused
//   prof ...                  profiler: see kit_prof.h (bracket, func, lap, clear, log)
//
// BLASTEM_CTRL_INIT=<file>: the lines of <file> are run as commands, as if sent over the socket,
// after the machine is created and before its first instruction, so a frame script, VRAM hash,
// profiler or speed setting is in place from reset (frame 0) instead of whenever the socket
// happens to be read. Blank lines and lines starting with '#' are skipped. Prints
// "KIT INIT file=<path> commands=<n>" or "KIT INIT error file=<path> cannot open".
//
// Buttons: up down left right a b c x y z start mode
//
// Unix-domain sockets (AF_UNIX) work identically on Linux, macOS, and Windows 10+, so this is
// one transport for all three. Input is injected through the same gamepad_down/up entry points
// the SDL keyboard/joystick bindings use.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrl_fifo.h"
#include "net.h"
#include "blastem.h"
#include "system.h"
#include "io.h"
#include "render.h"
#include "util.h"
#include "genesis.h"
#include "kit_prof.h"
#include "kit_script.h"

#ifdef _WIN32
#include <winsock2.h>
#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static int listen_fd = -1;
static int client_fd = -1;

// Point the profiler at the running Genesis 68K, once. Safe to call before every prof/vramhash
// command; kit_prof_set_context only latches the first non-null context it is given.
static void kit_prof_bind_system(void)
{
	if (current_system && current_system->type == SYSTEM_GENESIS) {
		genesis_context *gen = (genesis_context *)current_system;
		kit_prof_set_context(gen->m68k);
	}
}

static void process_command(char *line)
{
	char *cmd = strtok(line, " \t");
	if (!cmd) {
		return;
	}
	if (!strcmp(cmd, "pad")) {
		char *num = strtok(NULL, " \t");
		char *action = strtok(NULL, " \t");
		char *button_name = strtok(NULL, " \t");
		if (!num || !action || !button_name) {
			warning("ctrl_sock: expected 'pad <num> <down|up> <button>'\n");
			return;
		}
		uint8_t button = kit_parse_button(button_name);
		if (button == BUTTON_INVALID) {
			warning("ctrl_sock: unknown button '%s'\n", button_name);
			return;
		}
		if (!current_system) {
			return;
		}
		if (!strcmp(action, "down")) {
			if (current_system->gamepad_down) {
				current_system->gamepad_down(current_system, atoi(num), button);
			}
		} else if (!strcmp(action, "up")) {
			if (current_system->gamepad_up) {
				current_system->gamepad_up(current_system, atoi(num), button);
			}
		} else {
			warning("ctrl_sock: unknown pad action '%s'\n", action);
		}
	} else if (!strcmp(cmd, "screenshot")) {
		char *path = strtok(NULL, "");
		while (path && (*path == ' ' || *path == '\t')) {
			path++;
		}
		if (path && *path) {
			render_save_screenshot(strdup(path));
		} else {
			warning("ctrl_sock: expected 'screenshot <path>'\n");
		}
	} else if (!strcmp(cmd, "burst")) {
		char *count_str = strtok(NULL, " \t");
		char *prefix = strtok(NULL, "");
		while (prefix && (*prefix == ' ' || *prefix == '\t')) {
			prefix++;
		}
		long count = count_str ? strtol(count_str, NULL, 10) : 0;
		if (count > 0 && prefix && *prefix) {
			render_save_screenshot_burst(strdup(prefix), (uint32_t)count);
		} else if (count_str && !count) {
			render_save_screenshot_burst(NULL, 0);
		} else {
			warning("ctrl_sock: expected 'burst <count> <prefix>' or 'burst 0'\n");
		}
	} else if (!strcmp(cmd, "script")) {
		char *path = strtok(NULL, "");
		while (path && (*path == ' ' || *path == '\t')) {
			path++;
		}
		kit_script_set_capture(render_save_screenshot, render_save_screenshot_burst);
		kit_script_load(path);
	} else if (!strcmp(cmd, "scriptend")) {
		char *mode = strtok(NULL, " \t");
		char *tail = strtok(NULL, " \t");
		if (mode && !strcmp(mode, "hold")) {
			kit_script_set_hold(1, tail ? (uint32_t)strtoul(tail, NULL, 10) : 0);
		} else if (mode && !strcmp(mode, "run")) {
			kit_script_set_hold(0, 0);
		} else {
			warning("ctrl_sock: expected 'scriptend <hold [frames]|run>'\n");
		}
	} else if (!strcmp(cmd, "pause")) {
		if (current_system && !current_system->paused) {
			current_system->paused = 1;
			system_request_exit(current_system, 1);
		}
	} else if (!strcmp(cmd, "resume")) {
		kit_script_resume();
	} else if (!strcmp(cmd, "speed")) {
		char *arg = strtok(NULL, " \t");
		char line[64];
		if (arg && !strcmp(arg, "max")) {
			if (!render_is_audio_sync() || render_is_threaded_video()) {
				warning("ctrl_sock: 'speed max' needs system.sync_source audio; speed unchanged\n");
				return;
			}
			render_set_unthrottled(1);
			kit_emit_line("KIT SPEED max");
		} else if (arg && strtol(arg, NULL, 10) > 0) {
			uint32_t percent = (uint32_t)strtol(arg, NULL, 10);
			render_set_unthrottled(0);
			if (current_system && current_system->set_speed_percent) {
				current_system->set_speed_percent(current_system, percent);
			}
			render_set_speed_percent(percent);
			snprintf(line, sizeof(line), "KIT SPEED %u", percent);
			kit_emit_line(line);
		} else {
			warning("ctrl_sock: expected 'speed <max|percent>'\n");
		}
	} else if (!strcmp(cmd, "frame")) {
		kit_script_report_frame();
	} else if (!strcmp(cmd, "logframes")) {
		char *state = strtok(NULL, " \t");
		if (state && !strcmp(state, "on")) {
			kit_script_set_logframes(1);
		} else if (state && !strcmp(state, "off")) {
			kit_script_set_logframes(0);
		} else {
			warning("ctrl_sock: expected 'logframes <on|off>'\n");
		}
	} else if (!strcmp(cmd, "prof")) {
		char *sub = strtok(NULL, " \t");
		if (!sub) {
			warning("ctrl_sock: expected 'prof <bracket|lap|clear|log> ...'\n");
			return;
		}
		kit_prof_bind_system();
		if (!strcmp(sub, "lap")) {
			char *name = strtok(NULL, " \t");
			char *addr = strtok(NULL, " \t");
			if (!name || !addr) {
				warning("ctrl_sock: expected 'prof lap <name> <addr_hex>'\n");
				return;
			}
			kit_prof_add_lap(name, (uint32_t)strtoul(addr, NULL, 16));
		} else if (!strcmp(sub, "bracket")) {
			char *name = strtok(NULL, " \t");
			char *start = strtok(NULL, " \t");
			char *end = strtok(NULL, " \t");
			if (!name || !start || !end) {
				warning("ctrl_sock: expected 'prof bracket <name> <start_hex> <end_hex>[,<end_hex>...]'\n");
				return;
			}
			uint32_t start_addr = (uint32_t)strtoul(start, NULL, 16);
			// several exits (two rts, a tail jmp): comma-separated end addresses
			uint32_t end_addrs[KIT_PROF_MAX_ENDS];
			uint8_t num_ends = 0;
			for (char *p = end; *p; )
			{
				char *next;
				uint32_t addr = (uint32_t)strtoul(p, &next, 16);
				if (next == p || num_ends == KIT_PROF_MAX_ENDS || (*next && *next != ',')) {
					warning("ctrl_sock: bad end address list '%s' (at most %d, comma-separated)\n", end, KIT_PROF_MAX_ENDS);
					return;
				}
				end_addrs[num_ends++] = addr;
				p = *next ? next + 1 : next;
			}
			kit_prof_add_bracket(name, start_addr, end_addrs, num_ends);
		} else if (!strcmp(sub, "func")) {
			char *name = strtok(NULL, " \t");
			char *entry = strtok(NULL, " \t");
			if (!name || !entry) {
				warning("ctrl_sock: expected 'prof func <name> <entry_hex>'\n");
				return;
			}
			kit_prof_add_func(name, (uint32_t)strtoul(entry, NULL, 16));
		} else if (!strcmp(sub, "clear")) {
			kit_prof_clear();
		} else if (!strcmp(sub, "log")) {
			char *state = strtok(NULL, " \t");
			if (state && !strcmp(state, "on")) {
				kit_prof_set_log(1);
			} else if (state && !strcmp(state, "off")) {
				kit_prof_set_log(0);
			} else {
				warning("ctrl_sock: expected 'prof log <on|off>'\n");
			}
		} else {
			warning("ctrl_sock: unknown prof subcommand '%s'\n", sub);
		}
	} else if (!strcmp(cmd, "vramdump")) {
		char *path = strtok(NULL, "");
		while (path && (*path == ' ' || *path == '\t')) {
			path++;
		}
		if (path && *path) {
			kit_prof_bind_system();
			kit_prof_request_vramdump(path);
		} else {
			warning("ctrl_sock: expected 'vramdump <path>'\n");
		}
	} else if (!strcmp(cmd, "vramhash")) {
		char *state = strtok(NULL, " \t");
		kit_prof_bind_system();
		if (state && !strcmp(state, "on")) {
			kit_prof_set_vramhash(1);
		} else if (state && !strcmp(state, "off")) {
			kit_prof_set_vramhash(0);
		} else {
			warning("ctrl_sock: expected 'vramhash <on|off>'\n");
		}
	} else if (!strcmp(cmd, "hud")) {
		char *sub = strtok(NULL, " \t");
		kit_prof_bind_system();
		if (!sub) {
			warning("ctrl_sock: expected 'hud <on|off|gamefps|idle|fpsgap> ...'\n");
			return;
		}
		if (!strcmp(sub, "on")) {
			kit_hud_on();
		} else if (!strcmp(sub, "off")) {
			kit_hud_off();
		} else if (!strcmp(sub, "gamefps")) {
			char *addr = strtok(NULL, " \t");
			if (!addr) {
				warning("ctrl_sock: expected 'hud gamefps <addr_hex>'\n");
				return;
			}
			kit_hud_set_gamefps((uint32_t)strtoul(addr, NULL, 16));
		} else if (!strcmp(sub, "idle")) {
			char *arg = strtok(NULL, " \t");
			if (!arg) {
				warning("ctrl_sock: expected 'hud idle <addr_hex|auto>'\n");
				return;
			}
			if (!strcmp(arg, "auto")) {
				kit_hud_set_idle_auto();
			} else {
				kit_hud_set_idle_manual((uint32_t)strtoul(arg, NULL, 16));
			}
		} else if (!strcmp(sub, "fpsgap")) {
			char *cyc = strtok(NULL, " \t");
			if (!cyc) {
				warning("ctrl_sock: expected 'hud fpsgap <cycles>'\n");
				return;
			}
			kit_hud_set_fpsgap((uint32_t)strtoul(cyc, NULL, 10));
		} else {
			warning("ctrl_sock: unknown hud subcommand '%s'\n", sub);
		}
	} else if (!strcmp(cmd, "watchlog")) {
#ifdef NEW_CORE
		char *sub = strtok(NULL, " \t");
		kit_prof_bind_system();
		if (sub && !strcmp(sub, "add")) {
			char *addr = strtok(NULL, " \t");
			if (!addr) {
				warning("ctrl_sock: expected 'watchlog add <addr_hex>'\n");
				return;
			}
			kit_watch_add((uint32_t)strtoul(addr, NULL, 16));
		} else if (sub && !strcmp(sub, "clear")) {
			kit_watch_clear();
		} else {
			warning("ctrl_sock: expected 'watchlog <add <addr_hex>|clear>'\n");
		}
#else
		warning("watchlog: unsupported on the JIT core build\n");
#endif
	} else {
		warning("ctrl_sock: unknown command '%s'\n", cmd);
	}
}

static void ctrl_sock_init(void)
{
	const char *path = getenv("BLASTEM_CTRL_SOCK");
	if (!path || !*path) {
		return;
	}
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		warning("ctrl_sock: path too long (max %d): %s\n", (int)sizeof(addr.sun_path) - 1, path);
		return;
	}
	strcpy(addr.sun_path, path);

	socket_init();
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		warning("ctrl_sock: socket() failed (err %d)\n", socket_last_error());
		return;
	}
	remove(path); // clear any stale socket file from a previous run
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		warning("ctrl_sock: failed to bind %s (err %d)\n", path, socket_last_error());
		socket_close(fd);
		return;
	}
	if (listen(fd, 1) < 0) {
		warning("ctrl_sock: failed to listen on %s (err %d)\n", path, socket_last_error());
		socket_close(fd);
		return;
	}
	socket_blocking(fd, 0);
	listen_fd = fd;
	debug_message("ctrl_sock: listening on %s\n", path);
}

void ctrl_fifo_run_init(void)
{
	static uint8_t done;
	const char *path = getenv("BLASTEM_CTRL_INIT");
	if (done || !path || !*path) {
		return;
	}
	done = 1;
	char line[4200];
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(line, sizeof(line), "KIT INIT error file=%s cannot open", path);
		kit_emit_line(line);
		return;
	}
	uint32_t count = 0;
	while (fgets(line, sizeof(line), f))
	{
		char *end = line + strlen(line);
		while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
			*--end = 0;
		}
		char *start = line;
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		if (!*start || *start == '#') {
			continue;
		}
		process_command(start);
		count++;
	}
	fclose(f);
	snprintf(line, sizeof(line), "KIT INIT file=%s commands=%u", path, count);
	kit_emit_line(line);
}

void ctrl_fifo_poll(void)
{
	static uint8_t initialized;
	// long enough for "script <absolute path>" lines (the old 256 silently dropped long ones)
	static char buf[4096];
	static size_t buffered;
	if (!initialized) {
		initialized = 1;
		ctrl_sock_init();
	}
	if (listen_fd < 0) {
		return;
	}
	// Serve one client at a time (the harness connects, sends a few lines, disconnects).
	if (client_fd < 0) {
		int c = accept(listen_fd, NULL, NULL);
		if (c < 0) {
			return; // no pending connection this frame (or would-block)
		}
		socket_blocking(c, 0);
		client_fd = c;
		buffered = 0;
	}
	for (;;)
	{
		// recv works on a socket fd on both POSIX and Windows (unlike read on Windows)
		int bytes = (int)recv(client_fd, buf + buffered, sizeof(buf) - 1 - buffered, 0);
		if (bytes > 0) {
			buffered += bytes;
			buf[buffered] = 0;
			char *start = buf, *newline;
			while ((newline = strchr(start, '\n')))
			{
				*newline = 0;
				process_command(start);
				start = newline + 1;
			}
			buffered -= start - buf;
			if (buffered >= sizeof(buf) - 1) {
				// overlong line with no newline, discard it
				buffered = 0;
			}
			memmove(buf, start, buffered);
		} else if (bytes == 0) {
			// client closed the connection; go back to accepting
			socket_close(client_fd);
			client_fd = -1;
			break;
		} else {
			if (socket_error_is_wouldblock()) {
				break; // no more data right now, try again next frame
			}
			// real error: drop the client
			socket_close(client_fd);
			client_fd = -1;
			break;
		}
	}
}
