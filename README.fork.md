# BlastEm, debug-capable fork

This fork (github.com/ulalume/blastem) is upstream BlastEm (https://www.retrodev.com/repos/blastem, Mercurial) plus
what headless test harnesses and debuggers need. It is what `sgdkx install` downloads, and what genesis-kit
(SGDK + BlastEm project tooling) drives. Fork changes are public domain (COPYING.fork); BlastEm is GPLv3 (COPYING).
Upstream changesets are replayed one by one with `./sync-upstream.sh`; the last sync point is the latest
`sync: upstream blastem @<node>` merge.

## Environment variables

| variable | effect |
|---|---|
| `BLASTEM_CTRL_SOCK=<path>` | bind a Unix-domain control socket: pad input, screenshots, frame scripts, profiler (commands in `ctrl_fifo.c`'s header) |
| `BLASTEM_CTRL_INIT=<file>` | run `<file>`'s control commands after the machine is created, before its first instruction (frame scripts, VRAM hash, profiler, `speed max` from frame 0) |
| `BLASTEM_NO_GUI=1` | errors go to stdout/stderr instead of a modal dialog |
| `BLASTEM_FREEZE_ACTION=fatal\|ignore\|...` | what a machine freeze does, instead of asking |
| `BLASTEM_HIDDEN_WINDOW=1` | create the main window hidden (input and screenshots go through the socket) |
| `BLASTEM_MIXED_WIDTH=0\|1\|2` | frames that mix H32 and H40 lines (below); overrides `video.mixed_width` |
| `BLASTEM_GDB_PORT=<port>` | gdb remote over TCP instead of a pipe (`-D`) |
| `BLASTEM_FONT=<path>` | UI font instead of the embedded Inter Medium |

## Control socket commands (summary)

`pad`, `screenshot`, `burst`, `script` (frame-keyed input and captures), `scriptend hold|run`, `frame`,
`logframes`, `vramhash`, `vramdump`, `prof bracket|func|lap|clear|log`, `hud`, `watchlog`, `speed max|<percent>`,
`pause`, `resume`. Output lines start with `KIT `. Full syntax: the header comments of `ctrl_fifo.c`,
`kit_prof.h` and `kit_script.h`; genesis-kit's `tools/kit/docs/probes.md` and `scenario.md` describe how the kit
uses them.

## Mixed H32/H40 frames

A game can switch between H32 (256 pixels) and H40 (320 pixels) in the middle of a frame, from an H interrupt.
Upstream pushes every frame at one width, picked by the mode most of its lines use, so the other lines are cropped
(H40 lines in an H32 frame) or padded with stale pixels (H32 lines in an H40 frame). With the option on, such a frame
is shown at the H40 width and its H32 lines are stretched to it; frames that use one mode are unchanged.

The option is off by default. Turn it on with either of:

- the config file (`blastem.cfg`, `video` section): `mixed_width nearest` (4 pixels to 5, one repeated) or
  `mixed_width blend` (blended, as the 32X path does); `off` is the default;
- the environment: `BLASTEM_MIXED_WIDTH=1` (nearest), `2` (blend), `0` (off); it wins over the config.

Try it with the test ROM in `kit-tests/mixed_width/` (H40 above line 112, H32 below):

```
cd kit-tests/mixed_width && sgdkx make release
BLASTEM_MIXED_WIDTH=0 ../../blastem out/release/rom.bin    # off: the H40 half is cropped to the H32 width
BLASTEM_MIXED_WIDTH=1 ../../blastem out/release/rom.bin    # on: the whole frame at 320 wide, lower half stretched
```

The digits `0123456789` repeat across every row, so the stretched lower half shows 32 columns spread over the H40
width, under the 40 columns of the upper half.
