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
static const uint16_t asm_freq_tab[128] = {
    0x0159,0x016D,0x0183,0x019A,0x01B3,0x01CC,0x01E7,0x0204,
    0x0223,0x0244,0x0266,0x028B,0x02B2,0x02DA,0x0307,0x0333,
    0x0366,0x0398,0x03CF,0x0409,0x0446,0x0487,0x04CC,0x0516,
    0x0565,0x05B4,0x060D,0x0667,0x06CB,0x072F,0x079E,0x0812,
    0x088B,0x090F,0x0998,0x0A2B,0x0AC9,0x0B68,0x0C1B,0x0CCE,
    0x0D96,0x0E5E,0x0F3C,0x1024,0x1116,0x121E,0x1330,0x1457,
    0x1593,0x16CF,0x1836,0x199C,0x1B2C,0x1CBD,0x1E78,0x2047,
    0x222C,0x243B,0x265F,0x28AE,0x2B26,0x2D9E,0x306B,0x3338,
    0x3659,0x397A,0x3CEF,0x408F,0x4458,0x4876,0x4CBF,0x515B,
    0x564C,0x5B3D,0x60D6,0x6670,0x6CB2,0x72F4,0x79DE,0x811D,
    0x88B1,0x90ED,0x997D,0xA2B6,0xAC98,0xB679,0xC1AC,0xCCDF,
    0xD963,0xE5E7,0xF3BD,0x1E60,0xEB06,0xBBCE,0x8D40,0x5E34,
    0x2E60,0xFDF6,0xCCDF,0x9AE0,0x68DB,0x359E,0x0209,0xCDFC,
    0x9961,0x6416,0x2E0D,0xF72A,0xBF5E,0x869B,0x4CCD,0x11E4,
    0xD5D7,0x9894,0x5A0D,0x1A2F,0xD8F0,0x9645,0x521E,0x0C74,
    0xC539,0x7C65,0x31EC,0xE5C7,0x97E6,0x4840,0xF6D1,0xA38B,
};

/* ASM volume attenuation table at 0x410: volume 0-15 */
static const uint8_t vol_tab[16] = {
    0x08, 0x0a, 0x09, 0x0d, 0x32, 0x2d, 0x29, 0x25,
    0x21, 0x1e, 0x1b, 0x18, 0x15, 0x12, 0x0f, 0x0c
};

/* Per-ASM-channel OPL3 mappings */
static const uint8_t asm_to_nuked_ch[] = {0,1,2,9,10,11};
static const uint8_t ch_bank[] = {0,0,0,1,1,1};
static const uint8_t ch_slot_id[] = {0,1,2,0,1,2};

/* Per-slot init C register values (from opl_init.inc) */
static const uint8_t c_base_bank0[] = {0xF0, 0x5E, 0xF0, 0xF1, 0x5F, 0xF0, 0x42, 0x11, 0xC0};
static const uint8_t c_base_bank1[] = {0xFE, 0xFE, 0xA0, 0xFF, 0xFF, 0xA0, 0x42, 0x11, 0xCC};

/* Operator base registers */
static const uint8_t op_base[4] = {0x20, 0x23, 0x28, 0x2B};
static const uint16_t reg_grp[5] = {0x20, 0x40, 0x60, 0x80, 0xE0};

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

static void load_instrument(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch, uint8_t inst_id)
{
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
            rp->opl_write(rp->opl_ctx, reg, regs[byte_idx]);
        }
        ch->op_tl[op] = regs[op * 5 + 1];
    }
    ch->flags |= 0x08;
}

/* Write channel C registers based on instrument flags and skip-ties state */
static void apply_channel_c(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    uint8_t bank = ch_bank[asm_ch];
    uint8_t slot = ch_slot_id[asm_ch];
    const uint8_t *c_base = bank ? c_base_bank1 : c_base_bank0;
    uint8_t flags = ch->inst_flags;
    uint8_t delta = (flags >> 1) & 0x0E;
    uint8_t skip_ties = (ch->flags & 0x04) ? 0 : 1;
    uint16_t c_reg1 = (bank << 8) | (0xC0 + slot);
    rp->opl_write(rp->opl_ctx, c_reg1, c_base[slot] | delta | skip_ties);
    uint16_t c_reg2 = (bank << 8) | (0xC3 + slot);
    rp->opl_write(rp->opl_ctx, c_reg2, c_base[slot + 3] | delta | (1 - skip_ties));
}

static void apply_volume(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (!(ch->flags & 0x04)) return;
    int atten = vol_tab[ch->volume & 0x0F];
    int expr_atten = (7 - (ch->expression & 0x07)) * 4;
    int total_atten = atten + expr_atten;
    if (total_atten > 63) total_atten = 63;
    if (total_atten < 0) total_atten = 0;
    for (int op = 0; op < 4; op++) {
        uint16_t reg = opl_reg(asm_ch, op, 1);
        uint8_t tl_val = ch->op_tl[op];
        int new_tl = (tl_val & 0x3F) + total_atten;
        if (new_tl > 63) new_tl = 63;
        rp->opl_write(rp->opl_ctx, reg, (uint8_t)((tl_val & 0xC0) | (uint8_t)new_tl));
    }
}

/* ----------------------------------------------------------------- */
/* ASM note-off (0x2FB): clear key-on, zero multiplier, max release  */
/* ----------------------------------------------------------------- */
static void channel_note_off(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (!(ch->flags & 0x02)) return;
    ch->flags &= ~0x02;
}

/* ----------------------------------------------------------------- */
/* ASM calc_frequency (0x2E4): look up raw freq, store in channel    */
/* ----------------------------------------------------------------- */
static void calc_frequency(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    int idx = (int)ch->note + ch->transpose;
    if (idx < 0) idx = 0;
    if (idx > 127) idx = 127;
    ch->freq_raw = asm_freq_tab[idx];
    ch->flags |= 0x02;
}

/* ----------------------------------------------------------------- */
/* ASM note frequency write (0x2AD-0x2E1): normalize raw freq to     */
/* block/fnum and write to OPL3 registers.  Also handles key-on.     */
/* Called after setting freq_raw (via calc_frequency) and applying    */
/* pitch effects.                                                     */
/* ----------------------------------------------------------------- */
static void apply_frequency(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    uint16_t bank = ch_bank[asm_ch];
    uint8_t nuked_ch = asm_to_nuked_ch[asm_ch];
    int di = (int)ch->freq_raw + (int)ch->pitch_wheel;
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
    rp->opl_write(rp->opl_ctx, a_reg, (uint8_t)(bx & 0xFF));
    uint8_t al = (uint8_t)((bx >> 8) & 0x03) | (uint8_t)((cl & 7) << 2);
    if (ch->flags & 0x01) al |= 0x20;
    rp->opl_write(rp->opl_ctx, b_reg, al & ~0x20);
    rp->opl_write(rp->opl_ctx, b_reg, al);
}

/* ----------------------------------------------------------------- */
/* ASM note_on_setup (0x349) combined: calc freq + write + portamento */
/* ----------------------------------------------------------------- */
/* ================================================================= */
/* Command dispatch helpers                                           */
/* ================================================================= */

static void meta_set_tempo(wm_replayer_t *rp, const uint8_t **ptr, const uint8_t *end)
{
    int v = rd_byte(ptr, end) | (rd_byte(ptr, end) << 8);
    if (v > 0) {
        rp->tempo = (uint16_t)v;
        rp->tick_rate = (uint16_t)v;
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

static void meta_set_pan(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                         const uint8_t **ptr, const uint8_t *end)
{
    (void)rp; (void)asm_ch;
    ch->pan = rd_byte(ptr, end);
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

/* F2: loop repeat -- decrement counter, loop back or continue */
static void loop_repeat(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch,
                        const uint8_t **ptr, const uint8_t *end)
{
    uint8_t count = rd_byte(ptr, end);
    int16_t offset = (int16_t)rd_word(ptr, end);
    if (count > 0) {
        uint8_t *p = (uint8_t *)(*ptr);
        uint8_t *tgt = p + offset;
        if (tgt >= ch->stream_start && tgt < ch->stream_end) {
            *ptr = tgt;
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
        if (ch->f4_depth == 0 && ch->outer_loop_remain == 0) {
            ch->outer_loop_remain = val;
        }
        ch->f4_depth++;
        ch->f4_params[ch->f4_depth * 2] = val;
    }
}

/* F5: test top of f4 stack; if value==1, read 2-byte forward jump from stream,
   otherwise skip 2 bytes (ASM 0x0EBB) */
static void f5_skip(wm_channel_t *ch, const uint8_t **ptr, const uint8_t *end)
{
    if (ch->f4_depth > 0) {
        uint8_t val = ch->f4_params[ch->f4_depth * 2];
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
        int idx = ch->f4_depth * 2;
        uint8_t *val = &ch->f4_params[idx];
        if (*val > 0) {
            (*val)--;
        }
        if (*val == 0) {
            // Counter reached zero: skip the signed offset byte and pop the stack
            (*ptr)++; // consume offset
            ch->f4_depth--;
        } else {
            // Decremented counter still > 0: jump back by signed offset
            int8_t offset = (int8_t)(*(*ptr));
            // Consume the offset byte then apply the signed displacement relative to the byte after the offset
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
    rp->opl_write(rp->opl_ctx, reg, val);
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
        rp->opl_write(rp->opl_ctx, (uint16_t)tbl[i].reg | ((uint16_t)tbl[i].bnk << 8), tbl[i].val);
}

/* ================================================================= */
/* wm_replayer public API                                             */
/* ================================================================= */

void wm_replayer_init(wm_replayer_t *rp, opl_write_fn fn, void *ctx)
{
    memset(rp, 0, sizeof(*rp));
    rp->opl_write = fn;
    rp->opl_ctx = ctx;
    rp->tempo = 120;
    rp->tick_rate = 200;
}

void wm_replayer_load(wm_replayer_t *rp, const wm_file_t *wm)
{
    for (int i = 0; i < 6; i++) {
        wm_channel_t *ch = &rp->channels[i];
        ch->flags = 0x80;
        ch->note = 60;
        ch->wait_remain = 0;
        ch->ties_remain = 0;
        ch->ties_factor = 4;
        ch->data_word = 0;
        ch->f4_depth = 0;
        ch->volume = 10;
        ch->expression = 7;
        ch->pan = 7;
        ch->transpose = 0;
        ch->inst_id = 0xFF;
        ch->inst_flags = 0;
        ch->op_tl[0] = 0; ch->op_tl[1] = 0; ch->op_tl[2] = 0; ch->op_tl[3] = 0;
        ch->loop_depth = 0;
        memset(ch->loop_stack, 0, sizeof(ch->loop_stack));
        ch->outer_loop_remain = 0;
        memset(ch->f4_params, 0, sizeof(ch->f4_params));
        ch->pitch_wheel = 0;
        ch->freq_raw = 0;
        ch->stream = wm->tracks[i];
        ch->stream_start = wm->tracks[i];
        ch->stream_end = wm->tracks[i] + wm->track_lens[i];
        ch->stream_offset = wm->header.track_offsets[i];
        ch->stream_len = (uint16_t)wm->track_lens[i];
    }
    parse_instruments(rp, wm->inst_table, wm->inst_table_len);
    wm_replayer_opl_init(rp);
}

void wm_replayer_reset(wm_replayer_t *rp)
{
    for (int i = 0; i < 6; i++) {
        wm_channel_t *ch = &rp->channels[i];
        ch->stream = ch->stream_start;
        ch->wait_remain = 0;
        ch->ties_remain = 0;
        ch->ties_factor = 4;
        ch->data_word = 0;
        ch->f4_depth = 0;
        ch->outer_loop_remain = 0;
        ch->volume = 10;
        ch->expression = 7;
        ch->pan = 7;
        ch->inst_id = 0xFF;
        ch->inst_flags = 0;
        ch->op_tl[0] = 0; ch->op_tl[1] = 0; ch->op_tl[2] = 0; ch->op_tl[3] = 0;
        ch->loop_depth = 0;
        memset(ch->loop_stack, 0, sizeof(ch->loop_stack));
        memset(ch->f4_params, 0, sizeof(ch->f4_params));
        ch->note = 60;
        ch->flags = 0x80;
        ch->pitch_wheel = 0;
        ch->freq_raw = 0;
    }
    rp->current_tick = 0;
}

/* ================================================================= */
/* Main command dispatch matching ASM (MUSICV_2.COM)                  */
/* ================================================================= */

static int process_channel(wm_replayer_t *rp, wm_channel_t *ch, int asm_ch)
{
    if (!(ch->flags & 0x80)) return 0;

    const uint8_t **ptr = &ch->stream;
    const uint8_t *end = ch->stream_end;

    /* Handle end of stream with possible outer loop restart */
    if (*ptr >= end) {
        if (ch->f4_depth > 0 && ch->outer_loop_remain > 1) {
            ch->outer_loop_remain--;
            ch->f4_depth = 0;
            memset(ch->f4_params, 0, sizeof(ch->f4_params));
            *ptr = ch->stream_start;
        } else {
            ch->flags &= ~0x80;
            return 0;
        }
    }

    uint8_t cmd = rd_byte(ptr, end);

    /* ---- cmd == 0x00: NOP ---- */
    if (cmd == 0x00) return 1;

    /* ---- cmd < 0x80: note-on with explicit duration ---- */
    if (cmd < 0x80) {
        ch->note = cmd;
        uint8_t dur = rd_byte(ptr, end);
        ch->wait_remain = dur;
        if (ch->flags & 0x04) {
            ch->ties_remain = dur;
        } else {
            int ties = (int)dur - ((int)dur * (int)ch->ties_factor / 8);
            if (ties < 1) ties = 1;
            ch->ties_remain = (uint8_t)ties;
        }
        if (!(ch->flags & 0x08))
            load_instrument(rp, ch, asm_ch, ch->inst_id);
        calc_frequency(rp, ch, asm_ch);
        ch->flags |= 0x01;
        apply_frequency(rp, ch, asm_ch);
        apply_channel_c(rp, ch, asm_ch);
        return 1;
    }

    /* ---- meta commands 0x80-0x8F (per ASM binary decode) ---- */
    if (cmd >= 0x80 && cmd <= 0x8F) {
        switch (cmd) {
        case 0x80: rd_byte(ptr, end);                      break; /* CONSUME 1 */
        case 0x81: ch->inst_id = rd_byte(ptr, end); ch->flags &= ~0x08; break;
        case 0x82:                                           break; /* IGNORE */
        case 0x83: {
            ch->flags |= 0x04;
            ch->note = rd_byte(ptr, end);
            uint8_t dur = rd_byte(ptr, end);
            ch->wait_remain = dur;
            ch->ties_remain = dur;
            if (!(ch->flags & 0x08))
                load_instrument(rp, ch, asm_ch, ch->inst_id);
            calc_frequency(rp, ch, asm_ch);
            ch->flags |= 0x01;
            apply_frequency(rp, ch, asm_ch);
            apply_channel_c(rp, ch, asm_ch);
            return 1;
        }
        case 0x84: meta_set_tempo(rp, ptr, end);             break;
        case 0x85: meta_set_ties_factor(ch, ptr, end);       break;
        case 0x86:                                           break; /* IGNORE */
        case 0x87: meta_set_volume(rp, ch, asm_ch, ptr, end); break;
        case 0x88: rd_byte(ptr, end); /* global 0x9E9 */     break;
        case 0x89: meta_set_pan(rp, ch, asm_ch, ptr, end);   break;
        case 0x8A: meta_set_expression(rp, ch, asm_ch, ptr, end); break;
        case 0x8B: ch->data_word = (uint16_t)rd_word(ptr, end); break;
        case 0x8C: meta_expression_inc(rp, ch, asm_ch);      break;
        case 0x8D: meta_expression_dec(rp, ch, asm_ch);      break;
        case 0x8E: meta_volume_inc(rp, ch, asm_ch);          break;
        case 0x8F: meta_volume_dec(rp, ch, asm_ch);          break;
        default: break;
        }
        return 1;
    }
    /* ---- sub-commands 0x90-0x9F ---- */
    if (cmd >= 0x90 && cmd <= 0x9F) {
        switch (cmd) {
        case 0x90: {
            int note_num = rd_byte(ptr, end);
            rd_byte(ptr, end);
            uint8_t dur = rd_byte(ptr, end);
            ch->note = (uint8_t)note_num;
            ch->wait_remain = dur;
            if (ch->flags & 0x04) {
                ch->ties_remain = dur;
            } else {
                int ties = (int)dur - ((int)dur * (int)ch->ties_factor / 8);
                if (ties < 1) ties = 1;
                ch->ties_remain = (uint8_t)ties;
            }
            if (!(ch->flags & 0x08))
                load_instrument(rp, ch, asm_ch, ch->inst_id);
            calc_frequency(rp, ch, asm_ch);
            ch->flags |= 0x01;
            apply_frequency(rp, ch, asm_ch);
            apply_channel_c(rp, ch, asm_ch);
            return 1;
        }
        case 0x91:
            rd_byte(ptr, end); rd_byte(ptr, end);
            rd_byte(ptr, end); rd_byte(ptr, end);
            break;
        case 0x92: break;
        case 0x93: break;
        case 0x94: rd_byte(ptr, end); break;
        case 0x95: rd_byte(ptr, end); break;
        default: break;
        }
        return 1;
    }
    /* ---- flow/loop commands 0xF0-0xFF ---- */
    if (cmd >= 0xF0) {
        switch (cmd) {
        case 0xF0: loop_end_track(rp, ch, asm_ch, ptr, end); return 0;
        case 0xF1: loop_start(ch, ptr, end); break;
        case 0xF2: loop_repeat(rp, ch, asm_ch, ptr, end); break;
        case 0xF3: rd_byte(ptr, end); rd_word(ptr, end); break;
        case 0xF4: f4_push(ch, ptr, end); break;
        case 0xF5: f5_skip(ch, ptr, end); break;
        case 0xF6: rd_byte(ptr, end); break;
        case 0xF7: loop_end(rp, ch, asm_ch, ptr, end); break;
        case 0xF8: f8_loop(ch, ptr, end); break;
        case 0xF9: loop_write_raw(rp, ch, asm_ch, ptr, end); break;
        default: break;
        }
        return 1;
    }
    /* ---- raw OPL3 register writes 0xA0-0xEF ---- */
    if (cmd >= 0xA0) {
        uint8_t val = rd_byte(ptr, end);
        uint8_t bank = ch_bank[asm_ch];
        uint16_t reg = cmd;
        if (bank) reg |= 0x100;
        rp->opl_write(rp->opl_ctx, reg, val);
        return 1;
    }

    return 1;
}

/* ----------------------------------------------------------------- */
int wm_replayer_tick(wm_replayer_t *rp)
{
    int any = 0;
    for (int i = 0; i < 6; i++) {
        wm_channel_t *ch = &rp->channels[i];
        int was_active = (ch->flags & 0x80) != 0;

        /* Mirror old code guard: act on active channels with stream data */
        if ((ch->flags & 0x80) && ch->stream < ch->stream_end) {
            if (ch->wait_remain > 0) {
                ch->wait_remain--;
                if (ch->ties_remain > 0) {
                    ch->ties_remain--;
                    if (ch->ties_remain == 0)
                        channel_note_off(rp, ch, i);
                }
                any = 1;
            } else {
                if (process_channel(rp, ch, i)) any = 1;
            }
        }

        if (was_active && !(ch->flags & 0x80))
            dump_channel_state(rp, i);
    }
    rp->current_tick++;
    return any;
}
