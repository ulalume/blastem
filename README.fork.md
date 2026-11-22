# BlastEm, debug-capable fork

This fork (github.com/ulalume/blastem) is upstream BlastEm (https://www.retrodev.com/repos/blastem, Mercurial) plus
what headless test harnesses and debuggers need. It is what `sgdkx install` downloads. Fork changes are public
domain (COPYING.fork); BlastEm is GPLv3 (COPYING). Upstream changesets are replayed one by one with
`./sync-upstream.sh`; the last sync point is the latest `sync: upstream blastem @<node>` merge.

## Environment variables

| variable | effect |
|---|---|
| `BLASTEM_CTRL_SOCK=<path>` | bind a Unix-domain control socket: pad input, screenshots, frame scripts, profiler (commands in `ctrl_fifo.c`'s header) |
| `BLASTEM_CTRL_INIT=<file>` | run `<file>`'s control commands after the machine is created, before its first instruction (frame scripts, VRAM hash, profiler, `speed max` from frame 0) |
| `BLASTEM_NO_GUI=1` | errors go to stdout/stderr instead of a modal dialog (not on Windows) |
| `BLASTEM_FREEZE_ACTION=fatal\|ask\|debug\|ignore` | what a machine freeze does; overrides `ui.machine_freeze_action` |
| `BLASTEM_HIDDEN_WINDOW=1` | create the main window hidden (input and screenshots go through the socket) |
| `BLASTEM_GDB_PORT=<port>` | gdb remote over TCP instead of a pipe (`-D`); Windows always uses TCP, port 1234 by default |
| `BLASTEM_FONT=<path>` | UI font instead of the embedded Inter Medium |
| `BLASTEM_MIXED_WIDTH=0\|1\|2` | mixed H32/H40 frames off / nearest / blend (see Notes); overrides `video.mixed_width` |

## Control socket commands (summary)

`pad`, `screenshot`, `burst`, `script` (frame-keyed input and captures), `scriptend hold|run`, `frame`,
`logframes`, `vramhash`, `vramdump`, `prof bracket|func|lap|clear|log`, `hud`, `watchlog`, `speed max|<percent>`,
`pause`, `resume`. Output lines start with `KIT `. Full syntax: the header comments of `ctrl_fifo.c`,
`kit_prof.h` and `kit_script.h`.

## Notes

- gdb single step keeps the fork's `gdb_m68k_read_long` (`gdb_remote.c`): upstream's `m68k_read_long`
  (8f6ab0bbeae1) also shifts the low word, which puts the breakpoint after `rts`/`rte` at a wrong address.
- Mixed H32/H40 frames (off by default): a frame that switches width mid-frame is shown at the H40 width with its
  H32 lines stretched. `video.mixed_width nearest|blend` in `blastem.cfg`, or `BLASTEM_MIXED_WIDTH`. Test ROM:
  `kit-tests/mixed_width/`.
