// External control socket for scripted/headless use. When the environment variable
// BLASTEM_CTRL_SOCK is set to a path, blastem binds a Unix-domain stream socket there and
// polls it once per frame for newline-terminated commands, so an external process can drive
// the emulated gamepad and trigger screenshots without window focus or synthetic key events:
//
//   pad <num> down <button>   press a pad button (num matches the gamepad
//   pad <num> up <button>     number in the io config, normally 1 or 2)
//   screenshot <path>         save the next rendered frame (.png or .ppm)
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
			warning("ctrl_sock: expected 'pad <num> <down|up> <button>'\n");
			return;
		}
		uint8_t button = parse_button(button_name);
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

void ctrl_fifo_poll(void)
{
	static uint8_t initialized;
	static char buf[256];
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
