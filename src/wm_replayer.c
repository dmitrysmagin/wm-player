#include "wm_replayer.h"
#include <string.h>

#define DBG_CH(fmt, ...) ((void)0)

#include <stdio.h>
#include <stdlib.h>

/* ----------------------------------------------------------------- */
/* Raw frequency table from ASM (at file offset 0x03, accessed via   */
/* CS:0x103).  These are phase-increment values; the replayer        */
/* normalizes to fnum+block by shifting right until < 0x400.         */
/* ----------------------------------------------------------------- */
static const uint16_t asm_freq_tab[] = {
    /*   0 –   7 */ 0x0159,0x016D,0x0183,0x019A,0x01B3,0x01CC,0x01E7,0x0204,
    /*   8 –  15 */ 0x0223,0x0244,0x0266,0x028B,0x02B2,0x02DA,0x0307,0x0333,
    /*  16 –  23 */ 0x0366,0x0398,0x03CF,0x0409,0x0446,0x0487,0x04CC,0x0516,
    /*  24 –  31 */ 0x0565,0x05B4,0x060D,0x0667,0x06CB,0x072F,0x079E,0x0812,
    /*  32 –  39 */ 0x088B,0x090F,0x0998,0x0A2B,0x0AC9,0x0B68,0x0C1B,0x0CCE,
    /*  40 –  47 */ 0x0D96,0x0E5E,0x0F3C,0x1024,0x1116,0x121E,0x1330,0x1457,
    /*  48 –  55 */ 0x1593,0x16CF,0x1836,0x199C,0x1B2C,0x1CBD,0x1E78,0x2047,
    /*  56 –  63 */ 0x222C,0x243B,0x265F,0x28AE,0x2B26,0x2D9E,0x306B,0x3338,
    /*  64 –  71 */ 0x3659,0x397A,0x3CEF,0x408F,0x4458,0x4876,0x4CBF,0x515B,
    /*  72 –  79 */ 0x564C,0x5B3D,0x60D6,0x6670,0x6CB2,0x72F4,0x79DE,0x811D,
    /*  80 –  87 */ 0x88B1,0x90ED,0x997D,0xA2B6,0xAC98,0xB679,0xC1AC,0xCCDF,
    /*  88 –  92 */ 0xD963,0xE5E7,0xF3BD,0x1E60,0xEB06,
    /*  93 – 159 */ 0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
                    0xEB00,0xEB00,0xEB00,0xEB00,
};

/* ASM volume level table at 0x514 (file 0x414): vol 0=quiet(0x32), vol 15=loud(0x00) */
static const uint8_t vol_tab[16] = {
    0x32, 0x2d, 0x29, 0x25, 0x21, 0x1e, 0x1b, 0x18,
    0x15, 0x12, 0x0f, 0x0c, 0x09, 0x06, 0x03, 0x00
};

/* ASM connection-byte table at 0x510 (file 0x410): indexed by inst_flags & 3.
   Each byte is a bitmask selecting which of the 4 operators are carriers
   (need volume scaling). Bit N set = operator N gets attenuation. */
static const uint8_t conn_table[4] = {0x08, 0x0a, 0x09, 0x0d};

/* Per-ASM-channel OPL3 mappings */
static const uint8_t asm_to_nuked_ch[] = {0,1,2,9,10,11};
static const uint8_t ch_bank[] = {0,0,0,1,1,1};
static const uint8_t ch_slot_id[] = {0,1,2,0,1,2};

/* Operator base registers */
static const uint8_t op_base[4] = {0x20, 0x23, 0x28, 0x2B};
static const uint16_t reg_grp[5] = {0x20, 0x40, 0x60, 0x80, 0xE0};

/* Debug counters */
static int apply_count[6] = {0};
void wm_apply_counts(int *dst) { memcpy(dst, apply_count, sizeof(apply_count)); }

int f8_iterations[6] = {0};
int f8_calls[6] = {0};
int f4_push_count[6] = {0};

/* ----------------------------------------------------------------- */
/* Tracked OPL3 write — monitors 4-op connect register bits and      */
/* NV register (0x104/0x105) to keep four_op_slave[] up-to-date.     */
/* Also logs all writes for DRO comparison.                          */
/* ----------------------------------------------------------------- */
static void opl_tracked_write(wm_replayer_t *rp, uint16_t reg, uint8_t val)
{
    uint8_t bank = (uint8_t)(reg >> 8) & 1;
    uint8_t r = (uint8_t)(reg & 0xFF);

    /* Track 4-op connect bits: C0-C8 */
    if (r >= 0xC0 && r <= 0xC8) {
        int ch_idx = r & 0x0F;
        if (ch_idx < 9) {
            int abs_ch = ch_idx + (bank ? 9 : 0);
            rp->four_op_conn[abs_ch] = val;
            /* Recompute four_op_slave status */
            rp->four_op_slave[abs_ch] = 0;
            if ((val & 0x10) && rp->four_op_enabled) {
                /* Odd channels in 4-op pairs are slaves */
                if (abs_ch & 1)
                    rp->four_op_slave[abs_ch] = 1;
            }
        }
    }

    /* Track NV register (0x104 / 0x105) */
    if (r == 0x04 && bank == 1) {
        rp->four_op_nv[bank - 1] = val;
        rp->four_op_enabled = (rp->four_op_nv[0] | rp->four_op_nv[1]) != 0;
        /* Recompute all four_op_slave status */
        for (int i = 0; i < 18; i++) {
            rp->four_op_slave[i] = 0;
            if ((rp->four_op_conn[i] & 0x10) && rp->four_op_enabled) {
                if (i & 1)
                    rp->four_op_slave[i] = 1;
            }
        }
    }

    rp->opl_write(rp->opl_ctx, reg, val);
}

static uint16_t opl_reg(int asm_ch, int op, int grp)
{
    uint16_t r = (uint16_t)(op_base[op] - 0x20 + reg_grp[grp]);
    r += ch_slot_id[asm_ch];
    if (ch_bank[asm_ch]) r |= 0x0100;
    return r;
}

static uint8_t rd_byte(const uint8_t **ptr, const uint8_t *end)
{
    if (*ptr >= end) return 0;
    return *(*ptr)++;
}

static int rd_word(const uint8_t **ptr, const uint8_t *end)
{
    int lo = rd_byte(ptr, end);
    int hi = rd_byte(ptr, end);
    return lo | (hi << 8);
}

/* ----------------------------------------------------------------- */
/* Parse instrument entries: each entry is 30 bytes.                  */
/* ----------------------------------------------------------------- */
static int parse_instruments(wm_replayer_t *rp, const uint8_t *data, size_t len)
{
    rp->insts = NULL;
    rp->num_insts = 0;
    if (!data || len < 30) return 0;
    int max_entries = (int)(len / 30);
    if (max_entries > 64) max_entries = 64;
    rp->insts = (wm_inst_entry_t *)calloc((size_t)max_entries, sizeof(wm_inst_entry_t));
    if (!rp->insts) return 0;
    int n = 0;
    for (int i = 0; i < max_entries; i++) {
        const uint8_t *e = data + (size_t)(i * 30);
        if (e + 30 > data + len) break;
        rp->insts[n].id = e[0x15];
        rp->insts[n].flags = e[0x14];
        memcpy(rp->insts[n].regs, e, WM_INST_REGS);
        n++;
    }
    rp->num_insts = n;
    return n;
}

static void apply_channel_c(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch);

int load_counts[6] = {0};

static void load_instrument(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch, uint8_t inst_id)
{
    load_counts[asm_ch]++;
    const uint8_t *regs = NULL;
    uint8_t flags = 0;
    for (int i = 0; i < rp->num_insts; i++) {
        if (rp->insts[i].id == inst_id) {
            regs = rp->insts[i].regs;
            flags = rp->insts[i].flags;
            break;
        }
    }
    if (!regs) return;
    (void)asm_ch; (void)inst_id;
    ch->inst_id = inst_id;
    ch->inst_flags = flags;
    for (int op = 0; op < 4; op++) {
        for (int grp = 0; grp < 5; grp++) {
            int byte_idx = op * 5 + grp;
            uint16_t reg = opl_reg(asm_ch, op, grp);

            opl_tracked_write(rp, reg, regs[byte_idx]);
        }
        ch->op_tl[op] = regs[op * 5 + 1];
    }
    ch->inst_flags = flags;
    ch->c_val_saved = flags;
    apply_channel_c(rp, ch, asm_ch);
    ch->flags |= 0x08;
}

/* XLAT table from MUSICV_2.COM at file offset 0x4B7 (runtime 0x5B7) */
static const uint8_t c_xlat_table[256] = {
    0x00, 0x50, 0xA0, 0xF0, 0x88, 0x44, 0x3C, 0x8B,
    0x1E, 0xE4, 0x09, 0xBA, 0x1E, 0x00, 0xB9, 0x40,
};

/* Write channel C registers using ASM formula (MUSICV_2.COM 0x64B)
   Writes 0xC0+slot and 0xC3+slot for the channel pair */
static void apply_channel_c(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    uint8_t bank = ch_bank[asm_ch];
    uint8_t slot = ch_slot_id[asm_ch];
    uint8_t flags = ch->inst_flags;

    uint16_t c_reg1 = (bank << 8) | (0xC0 + slot);
    uint16_t c_reg2 = (bank << 8) | (0xC3 + slot);

    /* ASM formula at 0x64B:
       AL = [si+0x3E] -> xlat table
       CL = flags
       CL = (CL >> 1) | ((CL & 1) << 15)            ; ROR CX,1
       CH = (CL >> 8) & 1                             ; original flags bit 0
       CL_lo = CL & 0xFF                              ; flags >> 1
       CL = CL_lo | translated                        ; combined
       C0 = CL
       C3 = (CL & 0xFE) | CH                         ; bit 0 from flags bit 0 */
    uint8_t xlat_idx = ch->c_xlat_index;
    uint8_t translated = c_xlat_table[xlat_idx & 0x0F];
    uint8_t cl_lo = (flags >> 1);
    uint8_t cl = cl_lo | translated;
    uint8_t ch_bit = (flags & 1);
    uint8_t c0_val = cl;
    uint8_t c3_val = (cl & 0xFE) | ch_bit;

    if (ch->flags & 0x04) {
        /* tied notes: override with delta = 0x0E (max feedback) */
        c0_val = (c0_val & 0xF1) | 0x0E;
        c3_val = (c3_val & 0xF1) | 0x0E;
    }

    opl_tracked_write(rp, c_reg1, c0_val);
    opl_tracked_write(rp, c_reg2, c3_val);
}

static void apply_volume(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    int atten = vol_tab[ch->volume & 0x0F];
    /* conn_table selects which operators are carriers and need volume scaling */
    uint8_t conn = conn_table[ch->c_val_saved & 3];
    for (int op = 0; op < 4; op++) {
        int bit = conn & 1;
        conn >>= 1;
        if (!bit) continue;
        uint16_t reg = opl_reg(asm_ch, op, 1);
        uint8_t tl_val = ch->op_tl[op];
        int new_tl = (tl_val & 0x3F) + atten;
        if (new_tl > 63) new_tl = 63;
        opl_tracked_write(rp, reg, (uint8_t)((tl_val & 0xC0) | (uint8_t)new_tl));
    }
}

/* ----------------------------------------------------------------- */
/* ASM note-off (0x2FB): clear key-on, zero multiplier, max release  */
/* ----------------------------------------------------------------- */
static void channel_note_off(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (!(ch->flags & 0x01)) return;
    ch->flags &= ~(0x01 | 0x02 | 0x04);
    uint8_t bank = ch_bank[asm_ch];
    uint8_t nuked_ch = asm_to_nuked_ch[asm_ch];
    uint8_t ch_in_bank = nuked_ch % 9;
    uint16_t a_reg = 0xA0 + ch_in_bank;
    uint16_t b_reg = 0xB0 + ch_in_bank;
    if (bank) { a_reg |= 0x0100; b_reg |= 0x0100; }
    opl_tracked_write(rp, a_reg, ch->a_reg_val);
    opl_tracked_write(rp, b_reg, ch->b_reg_val);
}

/* ----------------------------------------------------------------- */
/* ASM calc_frequency (0x2E4): look up raw freq, store in channel    */
/* ----------------------------------------------------------------- */
static void calc_frequency(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    int idx = (int)ch->note + rp->transpose;
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    ch->freq_raw = asm_freq_tab[idx];
    ch->flags |= 0x02;
}

/* ----------------------------------------------------------------- */
/* ASM frequency/register write (0x2AD-0x2E1): normalise di to       */
/* block+fnum and write OPL3 A+B registers.                          */
/* di already includes freq_raw + pitch_wheel + effect offsets.      */
/* ----------------------------------------------------------------- */
static void apply_frequency(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch, int di, int write_b)
{
    if (asm_ch >= 0 && asm_ch < 6) apply_count[asm_ch]++;
    uint16_t bank = ch_bank[asm_ch];
    uint8_t nuked_ch = asm_to_nuked_ch[asm_ch];

    int cl = 0;
    while (di > 0x3FF) {
        cl++;
        di >>= 1;
    }
    uint16_t bx = (uint16_t)di;
    uint8_t ch_in_bank = nuked_ch % 9;
    uint16_t a_reg = 0xA0 + ch_in_bank;
    uint16_t b_reg = 0xB0 + ch_in_bank;
    if (bank) { a_reg |= 0x0100; b_reg |= 0x0100; }
    {
        uint8_t new_a_val = (uint8_t)(bx & 0xFF);
        opl_tracked_write(rp, a_reg, new_a_val);
        ch->a_reg_val = new_a_val;
    }
    if (write_b) {
        uint8_t al = (uint8_t)((bx >> 8) & 0x03) | (uint8_t)((cl & 7) << 2);
        ch->b_reg_val = al;
        if (ch->flags & 0x01) al |= 0x20;
        opl_tracked_write(rp, b_reg, al);
    }
}

/* ----------------------------------------------------------------- */
/* Portamento helpers matching ASM semantics (MUSICV_2.COM)          */
/* ----------------------------------------------------------------- */

/* portamento_init — called after 0x91 command; computes step from   */
/* target/speed, sets effects flag (ASM 0x48D).                     */
static void portamento_init(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    (void)rp; (void)asm_ch;
    uint8_t speed = ch->portamento_speed;                /* [si+0x2F] */
    if (speed == 0) return;
    ch->effects_flags |= 0x02;
    ch->portamento_wait_ctr = speed;                     /* [si+0x2E] = speed */
    ch->portamento_flip_ctr = 0;                         /* [si+0x2B] = 0 */
    int16_t amp = ch->vibrato_amp;                       /* [si+0x3A] — amplitude written by cmd 0x91 */
    uint8_t rate = ch->portamento_rate;
    if (rate == 1) {
        /* ASM 0x3CD: rate==1 — direct copy, no division */
        ch->portamento_step_saved = amp;                 /* [si+0x34] = amp */
    } else {
        /* ASM 0x3AA/0x3BE: rate==0 and rate>=2 — idiv amp by speed */
        ch->portamento_step_saved = (int16_t)((int)amp / speed);  /* [si+0x34] = quotient */
        ch->portamento_target     = (int16_t)((int)amp % speed);  /* [si+0x36] = remainder */
        if (rate >= 2) {
            ch->vibrato_amp = 0;                         /* [si+0x3A] = 0 (rate>=2 zeros after use) */
        }
    }
}

/* portamento_restart — called at note-on from frequency_update       */
/* note-on path (ASM 0x110). Copies saved step to active.            */
static void portamento_restart(wm_channel_t *ch)
{
    if (!(ch->effects_flags & 0x02)) return;
    ch->portamento_flip_ctr = 0;                         /* 0x2B = 0 */
    ch->portamento_wait_ctr = ch->portamento_speed;      /* 0x2E = 0x2F */
    ch->portamento_step = ch->portamento_step_saved;     /* 0x30 = 0x34 */
    ch->portamento_step2 = ch->portamento_target;        /* 0x32 = 0x36 (remainder) */
    ch->portamento_delay_ctr = ch->portamento_delay;     /* 0x2D = 0x2C */
    ch->portamento_accum = 0;                             /* 0x38 = 0 */
}

/* portamento_tick — called from frequency_update effects path       */
/* (ASM 0x13B).  Returns int16_t to add to DI (0 if no change).     */
/* Implements rate=2 handler (ASM 0x262) used by DUNGION.WM.        */
static int16_t portamento_tick(wm_channel_t *ch)
{
    /* Outer delay (0x2D): one-shot; portamento starts after this    */
    /* counts from portamento_delay to 0, then stays 0 forever.      */
    if (ch->portamento_delay_ctr > 0) {
        ch->portamento_delay_ctr--;
        if (ch->portamento_delay_ctr > 0) return 0;
        /* First entry: inner wait counter needs initial reset.      */
        ch->portamento_wait_ctr = ch->portamento_speed;
    }

    /* --- Rate-2 handler begins (ASM 0x262) ----------------------- */
    /* decrement inner wait counter (0x2E) */
    ch->portamento_wait_ctr--;

    if (ch->portamento_wait_ctr > 0) {
        /* Normal tick: accum += step, DI += accum                   */
        ch->portamento_accum += ch->portamento_step;
        return ch->portamento_accum;
    }

    /* Wait counter expired (was 1, now 0): run flip logic           */
    ch->portamento_wait_ctr = ch->portamento_speed;       /* reset (0x2F→0x2E) */
    ch->portamento_flip_ctr = (ch->portamento_flip_ctr + 1) & 3;  /* 0x2B */

    if ((ch->portamento_flip_ctr & 1) == 0) {
        /* even (0 or 2): accum = 0 */
        ch->portamento_accum = 0;
        return 0;
    }
    /* odd (1 or 3): accum += step + step2, negate both              */
    ch->portamento_accum += ch->portamento_step + ch->portamento_step2;
    ch->portamento_step = -ch->portamento_step;                    /* 0x30 */
    ch->portamento_step2 = -ch->portamento_step2;                  /* 0x32 */
    return ch->portamento_accum;
}

/* ----------------------------------------------------------------- */
/* frequency_update (ASM 0x26A) — called every tick for active        */
/* channels with effects or pending key_on.                           */
/* ----------------------------------------------------------------- */
static void frequency_update(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    /* test active bit (ASM: testb $0x80, 0x03(%si)) */
    if (!(ch->flags & 0x80)) return;
    int di = 0;
    int write_b = 0;
    if (ch->flags & 0x02) {
        write_b = 1;
        ch->flags &= ~0x02;                      /* clear key_on */
        ch->flags |= 0x01;                       /* ensure note_on */
        portamento_restart(ch);                  /* call 0x110 */
    } else if (!(ch->flags & 0x01)) {
        return;                                  /* no note_on, skip */
    } else {
        if (ch->effects_flags & 0x04) {          /* vibrato */
            /* call vibrato handler here */
        }
        if (ch->effects_flags & 0x02) {          /* portamento */
            di += portamento_tick(ch);
        }
        if (ch->effects_flags & 0x01) {          /* slide (cmd 0x90) */
            /* ASM at file 0x024D */
            if (ch->wait_remain <= 1) {
                /* snap to target on last tick, clear slide */
                ch->freq_raw = (uint16_t)ch->portamento_target;
                ch->effects_flags &= ~0x01;
            } else {
                ch->portamento_accum += ch->portamento_step;
                di += ch->portamento_accum;
            }
        }
    }
    di += (int)ch->freq_raw;                     /* + 0x0B(%si) */
    di += (int)ch->pitch_wheel;                  /* + 0x10(%si) */

    di = (int)(uint16_t)(int16_t)di;

    apply_frequency(rp, ch, asm_ch, di, write_b);
}

/* ================================================================= */
/* Command dispatch helpers                                           */
/* ================================================================= */

static void meta_set_tempo(wm_replayer_t *rp, const uint8_t **ptr, const uint8_t *end)
{
    int v = rd_byte(ptr, end) | (rd_byte(ptr, end) << 8);
    if (v > 0) {
        rp->tempo = (uint16_t)v;
        /* ASM recalc_tempo (0x084F):  PIT_divisor = 1493043 / tempo;
           tick_rate = 1193180 / PIT_divisor  (= 200 Hz when tempo ≈ 150) */
        uint32_t pit_div = 1493043u / (uint32_t)v;
        if (pit_div == 0) pit_div = 1;
        rp->tick_rate = (uint16_t)(1193180u / pit_div);
    }
}

static void meta_set_ties_factor(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    ch->ties_factor = rd_byte(ptr, end) & 0x07;
}

static void meta_set_volume(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                            const uint8_t **ptr, const uint8_t *end)
{
    ch->volume = rd_byte(ptr, end) & 0x0F;
    apply_volume(rp, ch, asm_ch);
}

static void meta_set_expression(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                                const uint8_t **ptr, const uint8_t *end)
{
    ch->expression = (uint8_t)(rd_byte(ptr, end) - 1);
    apply_volume(rp, ch, asm_ch);
}

static void meta_expression_inc(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (ch->expression < 7) ch->expression++;
    apply_volume(rp, ch, asm_ch);
}

static void meta_expression_dec(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (ch->expression > 0) ch->expression--;
    apply_volume(rp, ch, asm_ch);
}

static void meta_volume_inc(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (ch->volume < 15) ch->volume++;
    apply_volume(rp, ch, asm_ch);
}

static void meta_volume_dec(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (ch->volume > 0) ch->volume--;
    apply_volume(rp, ch, asm_ch);
}

/* ----------------------------------------------------------------- */
/* Loop/flow command handlers                                         */
/* ----------------------------------------------------------------- */

static void loop_end_track(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                           const uint8_t **ptr, const uint8_t *end)
{
    (void)ptr; (void)end;
    ch->flags &= ~0x80;
    channel_note_off(rp, ch, asm_ch);
}

static void loop_start(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    if (ch->loop_depth < 8) {
        ch->loop_stack[ch->loop_depth] = *ptr;
        ch->loop_depth++;
    }
}

/* F2: loop repeat — decrement in-stream counter (ASM modifies data in-place), jump if >0 */
static void loop_repeat(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                        const uint8_t **ptr, const uint8_t *end)
{
    (void)rp; (void)asm_ch;
    const uint8_t *count_ptr = *ptr;
    uint8_t count = rd_byte(ptr, end);
    int16_t offset = (int16_t)rd_word(ptr, end);
    if (count > 0) {
        uint8_t new_count = count - 1;
        ((uint8_t *)count_ptr)[0] = new_count;      /* dec in-place, matching ASM mutable buffer */
        if (new_count > 0) {
            const uint8_t *tgt = *ptr + offset;
            if (tgt >= ch->stream_start && tgt < ch->stream_end) {
                *ptr = tgt;
            }
        }
    }
}

/* F7: loop end at stream - pop and auto-repeat if depth > 0 */
static void loop_end(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                     const uint8_t **ptr, const uint8_t *end)
{
    (void)rp; (void)asm_ch; (void)end;
    (void)ptr;
    if (ch->loop_depth > 0) {
        ch->loop_depth--;
    }
}

/* Track channel termination reasons */
static void dump_channel_state(const wm_replayer_t *rp, int ch)
{
    const wm_channel_t *c = &rp->channels[ch];
    fprintf(stderr, "  CH%d: flags=0x%02x wait=%d f4_depth=%d loop_depth=%d "
            "stream_rem=%td\n",
            ch, c->flags, c->wait_remain, c->f4_depth, c->loop_depth,
            (ptrdiff_t)(c->stream_end - c->stream));
}

/* F4: reads 1 byte, stores at f4_params[++f4_depth*2] (ASM 0x0EA4) */
static void f4_push(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    uint8_t val = rd_byte(ptr, end);
    if (ch->f4_depth < 15) {
        ch->f4_params[ch->f4_depth * 2] = val;
        ch->f4_depth++;
    }
}

/* F5: test top of f4 stack; if value==1, read 2-byte forward jump from stream,
   otherwise skip 2 bytes (ASM 0x0EBB) */
static void f5_skip(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    if (ch->f4_depth > 0) {
        uint8_t val = ch->f4_params[(ch->f4_depth - 1) * 2];
        if (val == 1) {
            uint16_t skip = rd_word(ptr, end);
            *ptr = *ptr + skip;
        } else {
            rd_byte(ptr, end);
            rd_byte(ptr, end);
        }
    }
}

/* F8: decrement top of f4 stack; if >0, backward jump by signed byte offset.
   If ==0, skip offset byte and pop stack (ASM 0x0F0E) */
static void f8_loop(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    (void)end;
    if (ch->f4_depth > 0) {
        int idx = (ch->f4_depth - 1) * 2;
        uint8_t *val = &ch->f4_params[idx];
        if (*val > 0) {
            (*val)--;
        }
        if (*val == 0) {
            (*ptr)++;
            ch->f4_depth--;
        } else {
            int8_t offset = (int8_t)(*(*ptr));
            *ptr = *ptr + 1 + offset;
        }
    }
}


/* Write raw OPL3 (F9): 2 bytes reg, val */
static void loop_write_raw(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                           const uint8_t **ptr, const uint8_t *end)
{
    uint8_t reg = rd_byte(ptr, end);
    uint8_t val = rd_byte(ptr, end);
    opl_tracked_write(rp, reg, val);
}

/* ================================================================= */
/* OPL3 chip initialization — exact DRO tick-0 write sequence         */
/* ================================================================= */

void wm_replayer_opl_init(wm_replayer_t *rp)
{
    static const struct { uint8_t bnk, reg, val; } tbl[] = {
#include "opl_init.inc"
    };
    for (size_t i = 0; i < sizeof(tbl)/sizeof(tbl[0]); i++)
        opl_tracked_write(rp, (uint16_t)tbl[i].reg | ((uint16_t)tbl[i].bnk << 8), tbl[i].val);
}

static int process_channel(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch);

/* ================================================================= */
/* wm_replayer public API                                             */
/* ================================================================= */

void wm_replayer_init(wm_replayer_t *rp, opl_write_fn fn, void *ctx)
{
    memset(rp, 0, sizeof(*rp));
    rp->opl_write = fn;
    rp->opl_ctx = ctx;
    rp->tempo = 120;
    rp->tick_rate = 200;   /* match DRO capture rate for comparison */
}

static void null_opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    (void)ctx; (void)reg; (void)val;
}

void wm_replayer_load(wm_replayer_t *rp, const wm_file_t *wm)
{
    for (int i = 0; i < 6; i++) {
        wm_channel_t *ch = &rp->channels[i];
        ch->flags = 0x80;
        ch->note = 60;
        ch->wait_remain = 1;
        ch->ties_remain = 0;
        ch->ties_factor = 4;
        ch->data_word = 0;
        ch->f4_depth = 0;
        ch->volume = 10;
        ch->expression = 7;
        ch->pan = 7;
        ch->inst_id = 0xFF;
        ch->inst_flags = 0;
        ch->op_tl[0] = 0; ch->op_tl[1] = 0; ch->op_tl[2] = 0; ch->op_tl[3] = 0;
        ch->loop_depth = 0;
        memset(ch->loop_stack, 0, sizeof(ch->loop_stack));
        memset(ch->f4_params, 0, sizeof(ch->f4_params));
        ch->pitch_wheel = 0;
        ch->freq_raw = 0;
        ch->a_reg_val = 0;
        ch->b_reg_val = 0;
        ch->effects_flags = 0;
        ch->portamento_rate = 0;
        ch->portamento_speed = 0;
        ch->portamento_wait_ctr = 0;
        ch->portamento_delay = 0;
        ch->portamento_delay_ctr = 0;
        ch->portamento_step = 0;
        ch->portamento_step2 = 0;
        ch->portamento_step_saved = 0;
        ch->portamento_accum = 0;
        ch->portamento_target = 0;
        ch->vibrato_amp = 0;
        ch->stream = wm->tracks[i];
        ch->stream_start = wm->tracks[i];
        ch->stream_end = ch->stream + wm->track_lens[i];
        ch->stream_offset = 0;
        ch->stream_len = wm->track_lens[i];
        ch->c_val_saved = 0;
        ch->c_xlat_index = 3;
    }
    parse_instruments(rp, wm->inst_table, wm->inst_table_len);
    rp->current_tick = 0;
    rp->transpose = 0;
}

/* ================================================================= */
/* Main command dispatch matching ASM (MUSICV_2.COM)                  */
/* ================================================================= */

static int process_channel(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (!(ch->flags & 0x80)) return 0;

    /* ASM 0xB4C: if note_on && !ties && wait == ties_remain: keyoff */
    if ((ch->flags & 0x01) && !(ch->flags & 0x04) &&
        ch->wait_remain == ch->ties_remain) {
        channel_note_off(rp, ch, asm_ch);
    }

    /* ASM 0xB63: decrement wait_remain; if > 0, still waiting */
    if (ch->wait_remain > 0) {
        ch->wait_remain--;
        if (ch->wait_remain > 0) return 1;
    }

    /* ASM 0xB6A: wait expired — ties/skip_setup/keyoff cleanup */
    if (ch->flags & 0x04) {
        ch->effects_flags |= 0x08;
        ch->flags |= 0x08;
        ch->flags &= ~0x04;
    } else {
        ch->flags &= ~0x08;
        channel_note_off(rp, ch, asm_ch);
        ch->effects_flags &= ~0x01;    /* clear slide on note end (ASM 0xB85) */
    }

    /* ASM fetch loop (0xB8C..0xBEE): process commands until note-on/0x80/0xF0 */
    const uint8_t **ptr = &ch->stream;
    const uint8_t *end = ch->stream_end;
    uint8_t cmd;

/* NOTE_ON_HANDLER for cmd < 0x80: note=cmd, next byte=duration */
#define NOTE_ON_HANDLER_SMALL(cmd) do { \
    if ((cmd) != ch->note) { \
        ch->effects_flags &= ~0x08; \
        ch->flags &= ~0x08; \
    } \
    ch->note = (cmd); \
    uint8_t dur_ = rd_byte(ptr, end); \
    ch->wait_remain = dur_; \
    if (!(ch->flags & 0x04)) { \
        if (ch->ties_factor == 0) { \
            ch->ties_remain = 1; \
        } else { \
            int ties_ = (int)dur_ - ((int)dur_ * (int)ch->ties_factor / 8); \
            if (ties_ < 1) ties_ = 1; \
            ch->ties_remain = (uint8_t)ties_; \
        } \
    } \
    /* skip_setup (bit3): same note repeated in a tie — keep playing, no re-trigger */ \
    if (ch->flags & 0x08) { ch->flags |= 0x01; return 1; } \
    load_instrument(rp, ch, asm_ch, ch->inst_id); \
    calc_frequency(rp, ch, asm_ch); /* sets freq_raw and key_on (bit1) */ \
    ch->flags |= 0x01; \
    return 1; \
} while(0)

    for (;;) {
        if (*ptr >= end) {
            ch->flags &= ~0x80;
            return 0;
        }
        cmd = rd_byte(ptr, end);

        if (cmd < 0x80) { NOTE_ON_HANDLER_SMALL(cmd); }

        if (cmd == 0x80) {
            ch->wait_remain = rd_byte(ptr, end);
            ch->flags &= ~0x07;
            return 1;
        }

        if (cmd == 0xF0) {
            loop_end_track(rp, ch, asm_ch, ptr, end);
            return 0;
        }

        /* dispatch commands (0x81-0xFF except 0x80/0xF0) */
        if (cmd <= 0x8F) {
            /* 0x81-0x8F: specific commands (cmd & 0x0F for sub-dispatch) */
            switch (cmd & 0x0F) {
            case 0x01: ch->inst_id = rd_byte(ptr, end); ch->flags &= ~0x08; break;
            case 0x02: /* 0x82: NOP */                                     break;
            case 0x03: ch->flags |= 0x04;                                  break; /* 0x83: set skip_ties, 0 bytes */
            case 0x04: meta_set_tempo(rp, ptr, end);                       break;
            case 0x05: meta_set_ties_factor(ch, ptr, end);                 break;
            case 0x06: /* 0x86: NOP */                                     break;
            case 0x07: meta_set_volume(rp, ch, asm_ch, ptr, end);          break;
            case 0x08: rp->transpose = (int8_t)rd_byte(ptr, end);          break;
            case 0x09: /* 0x89: pan — store to c_xlat_index, recompute C regs */
                       ch->c_xlat_index = rd_byte(ptr, end);
                       apply_channel_c(rp, ch, asm_ch);
                       break;
            case 0x0A: meta_set_expression(rp, ch, asm_ch, ptr, end);      break; /* 0x8A: 1 byte, val-1 → expression */
            case 0x0B: ch->pitch_wheel = (int16_t)rd_word(ptr, end);       break;
            case 0x0C: meta_expression_inc(rp, ch, asm_ch);                break;
            case 0x0D: meta_expression_dec(rp, ch, asm_ch);                break;
            case 0x0E: meta_volume_inc(rp, ch, asm_ch);                    break; /* 0x8E: 0 bytes */
            case 0x0F: meta_volume_dec(rp, ch, asm_ch);                    break;
            default: break;
            }
            continue;
        }
        if (cmd <= 0x9F) {
            /* 0x90-0x9F: extended commands (ASM secondary dispatch) */
            switch (cmd) {
            case 0x90: {
                /* ASM 0xCF6: [note][target][dur] — portamento note-on.
                   Sets slide effect (effects_flags bit0) and key_on. */
                int target_word = rd_word(ptr, end);
                uint8_t dur = rd_byte(ptr, end);
                int note_idx   = (target_word & 0xFF) + rp->transpose;
                int target_idx = ((target_word >> 8) & 0xFF) + rp->transpose;
                if (note_idx   < 0) note_idx   = 0; if (note_idx   > 127) note_idx   = 127;
                if (target_idx < 0) target_idx = 0; if (target_idx > 127) target_idx = 127;
                uint16_t freq_note   = asm_freq_tab[note_idx];
                uint16_t freq_target = asm_freq_tab[target_idx];

                ch->wait_remain  = dur;
                ch->ties_remain  = 0;  /* ASM explicitly zeroes ties_remain for slide notes */
                ch->freq_raw     = freq_note;
                ch->portamento_target = (int16_t)freq_target;

                int diff    = (int)freq_target - (int)freq_note;
                int divisor = (int)dur < 1 ? 1 : (int)dur;
                ch->portamento_step  = (int16_t)(diff / divisor);
                ch->portamento_accum = 0;

                ch->effects_flags = (ch->effects_flags | 0x01) & ~0x08;
                load_instrument(rp, ch, asm_ch, ch->inst_id);
                ch->flags = (ch->flags & ~0x01) | 0x02;  /* clear note_on, set key_on */
                return 1;
            }
            case 0x91:
                ch->portamento_rate  = rd_byte(ptr, end);
                ch->portamento_speed = rd_byte(ptr, end);
                ch->vibrato_amp      = (int16_t)rd_word(ptr, end); /* amplitude → [si+0x3A] */
                portamento_init(rp, ch, asm_ch);
                break;
            case 0x92: ch->effects_flags |= 0x02;                   break; /* enable vibrato */
            case 0x93: ch->effects_flags &= ~0x02;                  break;
            case 0x94: ch->portamento_delay = rd_byte(ptr, end);    break;
            case 0x95: rd_byte(ptr, end);                           break;
            default: /* 0x96-0x9F: NOPs per ASM secondary dispatch entries 6-15 */
                break;
            }
            continue;
        }
        /* 0xA0-0xEF: NOPs per ASM primary dispatch entries 2-6 (all RET) */
        if (cmd <= 0xEF) { continue; }
        /* 0xF1-0xFF (0xF0 handled above): loop/cmd handlers */
        switch (cmd) {
        case 0xF1: loop_start(ch, ptr, end);                 break;
        case 0xF2: loop_repeat(rp, ch, asm_ch, ptr, end);    break;
        case 0xF3: rd_byte(ptr, end); rd_word(ptr, end);     break;
        case 0xF4: f4_push(ch, ptr, end);                    break;
        case 0xF5: f5_skip(ch, ptr, end);                    break;
        case 0xF6: rd_byte(ptr, end);                        break;
        case 0xF7: loop_end(rp, ch, asm_ch, ptr, end);       break;
        case 0xF8: f8_loop(ch, ptr, end);                    break;
        case 0xF9: loop_write_raw(rp, ch, asm_ch, ptr, end); break;
        default: break;
        }
    }
#undef NOTE_ON_HANDLER_SMALL
#undef NOTE_ON_HANDLER_BIG
}

/* ----------------------------------------------------------------- */
int wm_replayer_tick(wm_replayer_t *rp)
{
    int any = 0;
    int active_count = 0;
    for (int i = 0; i < 6; i++) {
        wm_channel_t *ch = &rp->channels[i];
        int was_active = (ch->flags & 0x80) != 0;
        if (was_active) active_count++;

        if ((ch->flags & 0x80)) {
            if (process_channel(rp, ch, i)) any = 1;
            frequency_update(rp, ch, i);
        }

        if (was_active && !(ch->flags & 0x80))
            dump_channel_state(rp, i);
    }
    rp->current_tick++;
    if ((rp->current_tick % 3000) == 0 && active_count > 0) {
        fprintf(stderr, "  [tick %u] active=%d any=%d\n", rp->current_tick, active_count, any);
        for (int ci = 0; ci < 6; ci++) {
            wm_channel_t *c = &rp->channels[ci];
            if (c->flags & 0x80)
                fprintf(stderr, "    ch%d: f4d=%d flags=%02x sr=%td\n",
                    ci, c->f4_depth, c->flags,
                    (ptrdiff_t)(c->stream_end - c->stream));
        }
        fflush(stderr);
    }
    return any;
}
