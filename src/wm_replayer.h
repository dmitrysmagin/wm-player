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
    /* effects flags (ASM [si+0x23]): bit2=vibrato, bit1=portamento, bit0=slide */
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
    int16_t  portamento_accum;       /* [si+0x38] — current frequency offset accumulator */
    int16_t  portamento_target;      /* [si+0x36] — original target word, cleared after init */

    /* C register computation (ASM [si+0x3D], [si+0x3E]) */
    uint8_t  c_val_saved;            /* [si+0x3D] — saved parameter (copied from flags at instrument load) */
    uint8_t  c_xlat_index;           /* [si+0x3E] — index into XLAT table, init=3 */
} wm_channel_t;

#define WM_INST_REGS 20

typedef struct {
    uint8_t id;
    uint8_t flags;
    uint8_t regs[WM_INST_REGS];
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
