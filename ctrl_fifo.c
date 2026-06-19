// External control FIFO for scripted/headless use. When the environment
// variable BLASTEM_CTRL_FIFO is set to a path, blastem creates a named pipe
// there and polls it once per frame for newline-terminated commands, so an
// external process can drive the emulated gamepad (and trigger screenshots)
// without window focus or synthetic keyboard events:
//
//   pad <num> down <button>   press a pad button (num matches the gamepad
//   pad <num> up <button>     number in the io config, normally 1 or 2)
//   screenshot <path>         save the next rendered frame (.png or .ppm)
//
// Buttons: up down left right a b c x y z start mode
//
// Input is injected through the same system_header gamepad_down/up entry
// points the SDL keyboard/joystick bindings use.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctrl_fifo.h"

#ifdef _WIN32

void ctrl_fifo_poll(void)
{
}

#else

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "blastem.h"
#include "system.h"
#include "io.h"
#include "render.h"
#include "util.h"

static int fifo_fd = -1;

static uint8_t parse_button(const char *name)
{
	static const char *names[NUM_GAMEPAD_BUTTONS] = {
		[DPAD_UP] = "up", [DPAD_DOWN] = "down", [DPAD_LEFT] = "left", [DPAD_RIGHT] = "right",
		[BUTTON_A] = "a", [BUTTON_B] = "b", [BUTTON_C] = "c", [BUTTON_START] = "start",
		[BUTTON_X] = "x", [BUTTON_Y] = "y", [BUTTON_Z] = "z", [BUTTON_MODE] = "mode"
	};
	for (uint8_t button = DPAD_UP; button < NUM_GAMEPAD_BUTTONS; button++)
	{
		if (!strcmp(names[button], name)) {
			return button;
		}
	}
	return BUTTON_INVALID;
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
			warning("ctrl_fifo: expected 'pad <num> <down|up> <button>'\n");
			return;
		}
		uint8_t button = parse_button(button_name);
		if (button == BUTTON_INVALID) {
			warning("ctrl_fifo: unknown button '%s'\n", button_name);
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
			warning("ctrl_fifo: unknown pad action '%s'\n", action);
		}
	} else if (!strcmp(cmd, "screenshot")) {
		char *path = strtok(NULL, "");
		while (path && (*path == ' ' || *path == '\t')) {
			path++;
		}
		if (path && *path) {
			render_save_screenshot(strdup(path));
		} else {
			warning("ctrl_fifo: expected 'screenshot <path>'\n");
		}
	} else {
		warning("ctrl_fifo: unknown command '%s'\n", cmd);
	}
}

static void ctrl_fifo_init(void)
{
	const char *path = getenv("BLASTEM_CTRL_FIFO");
	if (!path || !*path) {
		return;
	}
	struct stat st;
	if (stat(path, &st)) {
		if (mkfifo(path, 0666)) {
			warning("ctrl_fifo: failed to create FIFO %s: %s\n", path, strerror(errno));
			return;
		}
	} else if (!S_ISFIFO(st.st_mode)) {
		warning("ctrl_fifo: %s exists but is not a FIFO\n", path);
		return;
	}
	fifo_fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fifo_fd < 0) {
		warning("ctrl_fifo: failed to open FIFO %s: %s\n", path, strerror(errno));
	} else {
		debug_message("ctrl_fifo: listening on %s\n", path);
	}
}

void ctrl_fifo_poll(void)
{
	static uint8_t initialized;
	static char buf[256];
	static size_t buffered;
	if (!initialized) {
		initialized = 1;
		ctrl_fifo_init();
	}
	if (fifo_fd < 0) {
		return;
	}
	for (;;)
	{
		ssize_t bytes = read(fifo_fd, buf + buffered, sizeof(buf) - 1 - buffered);
		if (bytes <= 0) {
			// EAGAIN (no data), EOF (no writer right now) or error: try again next frame
			break;
		}
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
	}
}

#endif //_WIN32
