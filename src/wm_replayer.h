#ifndef WM_REPLAYER_H
#define WM_REPLAYER_H

#include <stdint.h>
#include "wm_loader.h"

#define WM_SAMPLE_RATE 44100
#define WM_OPL_CLOCK  3579545u

typedef void (*opl_write_fn)(void *ctx, uint16_t reg, uint8_t val);

/* Per-channel state matching MUSICV_2.COM ASM semantics */
typedef struct {
    uint8_t  flags;           /* bit7=active, bit3=skip_setup, bit2=skip_ties, bit1=key_on, bit0=note_on */
    uint8_t  note;            /* current note number */
    uint8_t  wait_remain;     /* 8-bit duration counter */
    uint8_t  ties_remain;     /* 8-bit: when 0, issue key-off */
    uint8_t  ties_factor;     /* 0-7, programmed via cmd 0x84 */
    uint16_t data_word;       /* generic word storage (cmd 0x8A) */

    /* F4 param stack */
    uint8_t  f4_depth;
    uint8_t  f4_params[16];

    /* volume controls (ASM: [si+0x40], [si+0x41], [si+0x42]) */
    uint8_t  volume;          /* 0-15 */
    uint8_t  expression;      /* 0-7 */
    uint8_t  pan;             /* 0-15 */

    /* instrument state */
    uint8_t  inst_id;
    uint8_t  inst_flags;
    uint8_t  op_tl[4];
    uint8_t  op_waveform[4]; /* shadow of E-register (waveform select) per operator */

    /* F1/F7 loop stack */
    uint8_t  loop_depth;
    const uint8_t *loop_stack[8];

    /* stream */
    const uint8_t *stream;
    const uint8_t *stream_start;
    const uint8_t *stream_end;
    uint16_t stream_offset;
    uint16_t stream_len;

    /* frequency */
    uint16_t freq_raw;
    int16_t  pitch_wheel;
    uint8_t  b_reg_val;
    uint8_t  a_reg_val;       /* last written A register value */
    /* effects flags (ASM [si+0x23]): bit2=LFO(amp-mod), bit1=vibrato(freq-mod), bit0=slide */
    uint8_t  effects_flags;

    /* portamento/slide (cmd 0x91) — ASM frequency-accumulator model */
    uint8_t  portamento_rate;        /* [si+0x2A] — rate index (0-2) for jump table */
    uint8_t  portamento_flip_ctr;    /* [si+0x2B] — modulo-4 flip counter for rate 2 */
    uint8_t  portamento_delay;       /* [si+0x2C] — initial delay value (set via cmd 0x94) */
    uint8_t  portamento_delay_ctr;   /* [si+0x2D] — outer one-shot delay counter */
    uint8_t  portamento_wait_ctr;    /* [si+0x2E] — inner wait counter (dec every tick in rate-2) */
    uint8_t  portamento_speed;       /* [si+0x2F] — wait reload value (from 0x91 data[1]) */
    int16_t  portamento_step;        /* [si+0x30] — active step (negated on odd flips) */
    int16_t  portamento_step2;       /* [si+0x32] — second step (same as step, negated together) */
    int16_t  portamento_step_saved;  /* [si+0x34] — step = target/speed, computed at 0x91 init */
    int16_t  portamento_target;      /* [si+0x36] — remainder after amp/speed division; also slide target */
    int16_t  portamento_accum;       /* [si+0x38] — current frequency offset accumulator */
    int16_t  vibrato_amp;            /* [si+0x3A] — vibrato amplitude (written by cmd 0x91, read by vibrato_setup) */

    /* slide effect (cmd 0x90) — ASM [si+0x24], [si+0x26], [si+0x28] — separate from vibrato */
    int16_t  slide_accum;           /* [si+0x24] — running slide frequency offset (zeroed at key-on) */
    int16_t  slide_step;            /* [si+0x26] — per-tick slide step */
    int16_t  slide_target;          /* [si+0x28] — slide target frequency */

    /* C register computation (ASM [si+0x3D], [si+0x3E]) */
    uint8_t  c_val_saved;            /* [si+0x3D] — saved parameter (copied from flags at instrument load) */
    uint8_t  c_xlat_index;           /* [si+0x3E] — index into XLAT table, init=3 */

    /* LFO (amplitude modulation / tremolo) preset — loaded from instrument entry[0x1B..0x1D] */
    uint8_t  lfo_lo;          /* [si+0x4B] — period length (ticks per half-cycle) */
    uint8_t  lfo_hi;          /* [si+0x4D] — step magnitude; modified in-place by lfo_setup */
    uint8_t  lfo_amp;         /* [si+0x48] — initial delay before LFO starts */
    uint8_t  lfo_sub_step;    /* [si+0x4F] — sub-step reload value (computed by lfo_setup) */
    /* LFO working registers, reset by lfo_restart on each key-on */
    uint8_t  lfo_delay_ctr;   /* [si+0x49] */
    uint8_t  lfo_period_ctr;  /* [si+0x4A] */
    uint8_t  lfo_step;        /* [si+0x4C] — current step (may be negated each half-cycle) */
    uint8_t  lfo_accum;       /* [si+0x4E] — byte accumulator; subtracted from vol attenuation */
    uint8_t  lfo_sub_ctr;     /* [si+0x50] */
} wm_channel_t;

#define WM_INST_REGS 20

typedef struct {
    uint8_t  id;
    uint8_t  flags;
    uint8_t  regs[WM_INST_REGS];
    /* vibrato (freq-mod) preset — entry bytes 0x16..0x1A */
    uint8_t  vib_rate;        /* entry[0x16] lo — rate index (0-2) */
    uint8_t  vib_speed;       /* entry[0x16] hi — wait reload */
    uint16_t vib_amp;         /* entry[0x18..0x19] — amplitude word */
    uint8_t  vib_delay;       /* entry[0x1A] — initial delay */
    /* LFO (amp-mod / tremolo) preset — entry bytes 0x1B..0x1D */
    uint8_t  lfo_lo;          /* entry[0x1B] — period length */
    uint8_t  lfo_hi;          /* entry[0x1C] — step magnitude */
    uint8_t  lfo_amp;         /* entry[0x1D] — initial delay */
} wm_inst_entry_t;

typedef struct {
    wm_channel_t channels[6];
    int8_t   transpose;       /* global transpose (ASM [0x9E9]) */

    uint32_t current_tick;
    uint32_t tempo;
    uint16_t tick_rate;

    wm_inst_entry_t *insts;
    int num_insts;

    opl_write_fn opl_write;
    void *opl_ctx;

    /* 4-op slave tracking (indexed by nuked channel 0-17) */
    uint8_t four_op_slave[18];
    uint8_t four_op_conn[18];   /* raw Cx register bit4 per channel */
    uint8_t four_op_nv[2];      /* 0x104 and 0x105 NV register values */
    uint8_t four_op_enabled;    /* set if any NV bit is 1 */

    /* song looping — pointer kept so tick can restart when all channels die */
    const wm_file_t *wm_src;
} wm_replayer_t;

extern int f8_iterations[6];
extern int f8_calls[6];
extern int f4_push_count[6];
extern int load_counts[6];

void wm_replayer_init(wm_replayer_t *rp, opl_write_fn fn, void *ctx);
void wm_replayer_load(wm_replayer_t *rp, const wm_file_t *wm);
void wm_replayer_reset(wm_replayer_t *rp);
int  wm_replayer_tick(wm_replayer_t *rp);
void wm_replayer_opl_init(wm_replayer_t *rp);
void wm_apply_counts(int *dst);

#endif
