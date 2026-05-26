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
    int8_t   transpose;

    /* instrument state */
    uint8_t  inst_id;
    uint8_t  inst_flags;
    uint8_t  op_tl[4];

    /* F1/F7 loop stack */
    uint8_t  loop_depth;
    const uint8_t *loop_stack[8];

    /* outer track-repeat counter (TEMPORARY HACK) */
    uint8_t  outer_loop_remain;

    /* stream */
    const uint8_t *stream;
    const uint8_t *stream_start;
    const uint8_t *stream_end;
    uint16_t stream_offset;
    uint16_t stream_len;

    /* frequency */
    uint16_t freq_raw;
    int16_t  pitch_wheel;
} wm_channel_t;

#define WM_INST_REGS 20

typedef struct {
    uint8_t id;
    uint8_t flags;
    uint8_t regs[WM_INST_REGS];
} wm_inst_entry_t;

typedef struct {
    wm_channel_t channels[6];
    uint32_t current_tick;
    uint32_t tempo;

    wm_inst_entry_t *insts;
    int num_insts;

    opl_write_fn opl_write;
    void *opl_ctx;
} wm_replayer_t;

void wm_replayer_init(wm_replayer_t *rp, opl_write_fn fn, void *ctx);
void wm_replayer_load(wm_replayer_t *rp, const wm_file_t *wm);
void wm_replayer_reset(wm_replayer_t *rp);
int  wm_replayer_tick(wm_replayer_t *rp);
void wm_replayer_opl_init(wm_replayer_t *rp);

#endif
