# wm-player

A portable C reimplementation of the OPL3 music player from `MUSICV_2.COM`, the
DOS sound driver used by Wolf Team / "WM" (`.WM`) music data. The original is a
16-bit real-mode TSR that programs a Yamaha OPL3 (YMF262) chip directly; this
project reproduces its behaviour on top of the [Nuked OPL3](https://github.com/nukeykt/Nuked-OPL3)
emulator so that `.WM` songs can be played or rendered on a modern host.

The overriding goal is **bit-exact fidelity**: the player aims to emit the same
sequence of OPL3 register writes as the original driver, verified by comparing
against DRO captures of the DOS player running in DOSBox.

## Building

The build uses a standard C99 toolchain plus SDL2 (for live playback):

```sh
cd wm-player
make
```

This produces `wmplay` (`wmplay.exe` on Windows/MSYS2).

## Usage

```text
wmplay <file.wm>                    SDL real-time playback
wmplay --wav [-t N] <file.wm> [out.wav]   render to a 44.1 kHz/16-bit stereo WAV
```

- `-t N` sets the maximum render length in seconds (default 110).
- In `--wav` mode an `opl_writes.log` register trace is also produced for
  comparison against a reference DRO capture (see *Verification* below).

## Source layout

| File | Responsibility |
|------|----------------|
| `src/main.c` | CLI, WAV writer, OPL3 chip wiring, register-trace logging |
| `src/wm_loader.c` / `.h` | Parses the `.WM` container (header, tracks, instruments) |
| `src/wm_replayer.c` / `.h` | The sequencer/driver core — per-channel event interpreter and OPL3 register generation |
| `src/opl3.c` / `.h` | Nuked OPL3 emulator |
| `src/sdl_audio.c` / `.h` | SDL2 audio output backend |
| `src/opl_init.inc` | Captured chip-init register table (reference data; see notes) |

## OPL3 channel model

`.WM` data drives **six** logical channels. Each is realised as a 4-operator
OPL3 channel, grouped as three 4-op pairs in bank 0 and three in bank 1:

| Logical channel | OPL3 channel | Bank | Slot |
|-----------------|--------------|------|------|
| 0 | 0 | 0 | 0 |
| 1 | 1 | 0 | 1 |
| 2 | 2 | 0 | 2 |
| 3 | 9 | 1 | 0 |
| 4 | 10 | 1 | 1 |
| 5 | 11 | 1 | 2 |

Within a channel the four operators use register offsets `0x20, 0x23, 0x28, 0x2B`
(operators 0, 3, 8, 11) plus the channel's *slot* offset. Each operator has five
register groups: `0x20` (AM/VIB/mult), `0x40` (KSL/TL), `0x60` (attack/decay),
`0x80` (sustain/release), `0xE0` (waveform select).

---

## `.WM` file format

All multi-byte integers are **little-endian**. Offsets are absolute from the
start of the file.

### Container layout

```
+0x00  char[16]   signature  = "OPL3 DATA       "   (16 bytes, space-padded)
+0x10  u16[6]     track_offsets[0..5]   byte offset of each channel's event stream
+0x1C  u16        inst_offset           byte offset of the instrument table
+0x1E  u16        eof_offset            byte offset of end-of-data
+0x20  ...        event streams + instrument table (order varies)
```

Notes derived from the original driver:

- A `track_offset` of `0` means the channel is unused (no stream).
- The original keeps all tracks in **one flat buffer with no explicit per-track
  length** — each channel simply reads from its `track_offset` onward and stops
  on an explicit end-of-track event (`0xF0`). The loader therefore treats each
  track as running from its offset up to `eof_offset`.
- The instrument table runs from `inst_offset` to the nearest following boundary
  (`eof_offset` or the lowest track offset above it).
- Valid file size is between 32 bytes and 64 KiB.

### Instrument table

A flat array of **30-byte** entries (the loader reads up to 64 entries). The
first 20 bytes are the raw OPL3 operator register image; the remainder holds
modulation presets:

| Offset | Field | Meaning |
|--------|-------|---------|
| `0x00..0x13` | `regs[20]` | Raw operator register bytes — 4 operators × 5 groups, ordered `[op][group]`: op0(`20,40,60,80,E0`), op1(`23,43,63,83,E3`), op2(`28,48,68,88,E8`), op3(`2B,4B,6B,8B,EB`) |
| `0x14` | `flags` | Connection/algorithm flags. Low 2 bits index `conn_table` (which operators are carriers); bits feed the `Cx` feedback/connection register |
| `0x15` | `id` | Instrument ID referenced by the `0x81` event |
| `0x16` | vibrato rate (low 2 bits) / wait reload (high bits) | Frequency-modulation (vibrato) preset |
| `0x17` | vibrato speed | wait reload value |
| `0x18..0x19` | vibrato amplitude | 16-bit |
| `0x1A` | vibrato delay | initial delay |
| `0x1B` | LFO period | tremolo (amplitude-mod) preset |
| `0x1C` | LFO step magnitude | |
| `0x1D` | LFO amp/delay | initial delay |

When an instrument is loaded, carrier-operator total-level (`0x40` group) bytes
are scaled by the channel volume before being written (see *Volume* below). The
waveform (`0xE0`) write is suppressed if it matches a per-operator shadow copy,
matching the original driver's optimisation.

---

## Event (command) stream format

Each channel stream is a byte sequence interpreted one event at a time. A tick
processes events for a channel until a note is triggered or a wait is issued.

### Note-on: `0x00 .. 0x7F`

A byte below `0x80` is a **note number**, followed by one **duration** byte:

```
NN DD       NN = note (0..127), DD = duration in ticks
```

The note is transposed by the global transpose value, looked up in the phase-
increment frequency table, and the channel is keyed on. The duration also seeds
the *ties* counter (controlled by event `0x85`), which determines when the note
is keyed off relative to its full length.

### Wait / end

| Byte | Args | Meaning |
|------|------|---------|
| `0x80` | `DD` | Wait `DD` ticks (rest); ends event processing for this tick |
| `0xF0` | — | End of track — channel goes inactive |

### Control events `0x81 .. 0x8F`

(dispatched on the low nibble)

| Byte | Args | Meaning |
|------|------|---------|
| `0x81` | `id` | Load instrument `id`; programs all operator registers |
| `0x82` | — | No-op |
| `0x83` | — | Set *skip-ties* flag (note continues / legato setup) |
| `0x84` | `vv` | Set tempo (PIT divisor → tick rate) |
| `0x85` | `vv` | Set ties factor (0..7) — proportion of duration before key-off |
| `0x86` | — | No-op |
| `0x87` | `vv` | Set channel **volume** (0..15); recomputes carrier levels |
| `0x88` | `vv` | Set global **transpose** (signed) |
| `0x89` | `vv` | Set **pan / connection index**; recomputes `Cx` registers |
| `0x8A` | `vv` | Set **expression** (value − 1); recomputes carrier levels |
| `0x8B` | `lo hi` | Set **pitch wheel** (signed 16-bit) |
| `0x8C` | — | Expression increment (+1, max 7) |
| `0x8D` | — | Expression decrement (−1, min 0) |
| `0x8E` | — | Volume increment (+1, max 15) |
| `0x8F` | — | Volume decrement (−1, min 0) |

### Extended events `0x90 .. 0x9F`

| Byte | Args | Meaning |
|------|------|---------|
| `0x90` | `note target dur` | Portamento/slide note-on: glides from `note` to `target` over `dur` ticks |
| `0x91` | `rate speed amp_lo amp_hi` | Set vibrato/portamento parameters and (re)initialise |
| `0x92` | — | Enable vibrato (frequency modulation) |
| `0x93` | — | Disable vibrato |
| `0x94` | `vv` | Set portamento delay |
| `0x95` | `vv` | Reserved — argument consumed, otherwise no-op |
| `0x96..0x9F` | — | No-ops |

### `0xA0 .. 0xEF`

No-ops (all map to RET in the original dispatch table).

### Loop / stack events `0xF1 .. 0xF9`

The driver maintains a per-channel parameter/counter stack (`f4_params`,
depth ≤ 15) and uses several jump primitives. Jump offsets are taken relative to
the offset bytes themselves (matching the original `ADD DI,[DI]` semantics),
not to the byte after them.

| Byte | Args | Meaning |
|------|------|---------|
| `0xF1` | `lo hi` | Relative jump by signed 16-bit word |
| `0xF2` | `cnt lo hi` | Counted repeat: decrement in-stream counter, jump by signed word while > 0 |
| `0xF3` | `b lo hi` | Reserved — 3 bytes consumed, otherwise no-op |
| `0xF4` | `vv` | Push `vv` onto the parameter/counter stack |
| `0xF5` | `lo hi` | If stack top == 1: jump by signed word and pop; else skip the word |
| `0xF6` | `lo hi` | Counted loop: decrement stack top; jump by signed word while > 0, else pop |
| `0xF7` | `off` | If stack top == 1: backward jump by signed byte and pop; else skip the byte |
| `0xF8` | `off` | Decrement stack top; backward jump by signed byte while > 0, else pop |
| `0xF9` | `reg val` | Write a raw OPL3 register (`reg` = value, `val` = data) |

`0xFA..0xFF` are unhandled (no-ops).

---

## Volume, expression and modulation

- **Carrier attenuation** is computed as `vol_tab[volume] − lfo_accum`, clamped to
  `0..63`, and added to each carrier operator's base total-level. `vol_tab` runs
  from `0x32` (volume 0, quietest) to `0x00` (volume 15, loudest).
- **`conn_table` = {0x08, 0x0A, 0x09, 0x0D}** is indexed by `inst_flags & 3`; each
  byte is a bitmask of which of the four operators are carriers (and therefore
  scaled by volume).
- **Frequency** uses a phase-increment table; values are normalised to OPL3
  fnum + block by right-shifting until below `0x400`, then written to the
  `Ax`/`Bx` registers with the key-on bit.
- **Vibrato** (frequency modulation) and **LFO/tremolo** (amplitude modulation)
  are driven by per-instrument presets and the `0x91`/`0x92`/`0x93` events.

## Song-start register writes

At load the player emits a small fixed prologue to match the DOS driver's
power-on sequence:

- `B1:04 = 0x3F` — enable 4-op mode for all six channel pairs
- `B1:05 = 0x01` — OPL3 mode enable
- `B0:BD = 0x40` — tremolo/vibrato depth + rhythm-mode select

When every channel has reached end-of-track the song restarts, mirroring the way
the original game driver re-triggered playback.

## Verification

Fidelity is measured by rendering with `--wav`, which logs every OPL3 write to
`opl_writes.log` (`tick bank reg val`), and comparing the *value sequence* per
`(bank, register)` against a reference DRO capture of the DOS player:

```sh
./wmplay.exe --wav -t 110 ../MUSICV/AYASII.WM out.wav
python3 ../compare2.py        # reads ayasii.dro + opl_writes.log
```

`compare2.py` collapses consecutive identical writes and reports, per register,
how long the two value sequences agree (the common-prefix length) and where they
first diverge.

> **Note on `opl_init.inc`**: this is a captured chip-init register table kept
> for reference. The corresponding `wm_replayer_opl_init()` routine is currently
> **not** invoked — the original driver's true start-up burst is generated (with
> volume-scaled carrier levels and some operators omitted) rather than being a
> verbatim table dump, so the values here do not exactly match a DRO capture.
