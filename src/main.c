#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"
#include "wm_loader.h"
#include "wm_replayer.h"

#define SAMPLE_RATE 44100
#define TICK_RATE   200     /* replayer ticks per second (matches DOS) */
#define MAX_SECONDS 110     /* max output length */

typedef struct {
    opl3_chip chip;
    wm_replayer_t rp;
    unsigned tick_sub;      /* fractional-sample accumulator */
    FILE *log_fp;
    unsigned current_tick;
} player_ctx_t;

static void opl_write_cb(void *ctx, uint16_t reg, uint8_t val) {
    player_ctx_t *pc = (player_ctx_t *)ctx;
    OPL3_WriteRegBuffered(&pc->chip, reg, val);
    if (pc->log_fp) {
        int bank = (reg >> 8) & 1;
        fprintf(pc->log_fp, "%u %d 0x%02X 0x%02X\n", pc->current_tick, bank, reg & 0xFF, val);
    }
}

static void write_wav_header(FILE *fp, unsigned num_frames) {
    unsigned short channels = 2;    /* OPL3 renders stereo */
    unsigned short bits = 16;
    unsigned short block_align = channels * (bits / 8);
    unsigned byte_rate = SAMPLE_RATE * block_align;
    unsigned data_bytes = num_frames * block_align;
    unsigned file_size = 36 + data_bytes;
    fwrite("RIFF", 4, 1, fp);
    fwrite(&file_size, 4, 1, fp);
    fwrite("WAVE", 4, 1, fp);
    fwrite("fmt ", 4, 1, fp);
    unsigned fmt_size = 16;
    unsigned short audio_fmt = 1;
    unsigned sample_rate = SAMPLE_RATE;
    fwrite(&fmt_size, 4, 1, fp);
    fwrite(&audio_fmt, 2, 1, fp);
    fwrite(&channels, 2, 1, fp);
    fwrite(&sample_rate, 4, 1, fp);
    fwrite(&byte_rate, 4, 1, fp);
    fwrite(&block_align, 2, 1, fp);
    fwrite(&bits, 2, 1, fp);
    fwrite("data", 4, 1, fp);
    fwrite(&data_bytes, 4, 1, fp);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.wm> [output.wav]\n", argv[0]);
        return 1;
    }

    const char *infile = argv[1];
    const char *outfile = (argc >= 3) ? argv[2] : "output.wav";

    /* ---------- Load WM ---------- */
    wm_file_t wm;
    if (wm_load(infile, &wm) != 0) {
        fprintf(stderr, "Error: could not load WM file '%s'\n", infile);
        return 1;
    }
    printf("Loaded '%s': %zu bytes, %d tracks\n", infile, wm.size, WM_NCHANNELS);
    for (int i = 0; i < 6; i++) {
        printf("  Track %d: offset=0x%04X, len=%zu bytes\n",
               i, wm.header.track_offsets[i], wm.track_lens[i]);
    }
    printf("  Instrument table: offset=0x%04X, len=%zu bytes\n",
           wm.header.inst_offset, wm.inst_table_len);
    printf("  EOF: 0x%04X\n", wm.header.eof_offset);

    /* ---------- Init player ---------- */
    player_ctx_t pc;
    memset(&pc, 0, sizeof(pc));

    OPL3_Reset(&pc.chip, SAMPLE_RATE);
    wm_replayer_init(&pc.rp, opl_write_cb, &pc);

    pc.log_fp = fopen("opl_writes.log", "w");
    if (pc.log_fp) printf("Logging OPL3 writes to opl_writes.log\n");
    pc.current_tick = 0;

    wm_replayer_load(&pc.rp, &wm);

    pc.tick_sub = 0;

    unsigned max_ticks = TICK_RATE * MAX_SECONDS;
    unsigned duration_samples = max_ticks * (SAMPLE_RATE / TICK_RATE + 1) + SAMPLE_RATE;

    /* pre-allocate output buffer (safe upper bound) — 2 int16_t per stereo frame */
    int16_t *outbuf = (int16_t *)malloc(duration_samples * 2 * sizeof(int16_t));
    if (!outbuf) { wm_unload(&wm); return 1; }

    /* ---------- Render ---------- */
    printf("Rendering...\n");

    unsigned buf_pos = 0;
    unsigned ticks_done = 0;
    int song_ended = 0;

    for (unsigned tick = 0; tick < max_ticks; tick++) {
        pc.current_tick = tick;

        /* keep processing even after channels end (notes ring out) */
        if (!song_ended && !wm_replayer_tick(&pc.rp)) {
            printf("  Song ended at tick %u\n", tick);
            song_ended = 1;
        }

        /* generate audio for this tick */
        pc.tick_sub += SAMPLE_RATE;
        unsigned nsamp = pc.tick_sub / TICK_RATE;
        pc.tick_sub -= nsamp * TICK_RATE;
        if (buf_pos + nsamp > duration_samples) break;
        OPL3_GenerateStream(&pc.chip, outbuf + buf_pos * 2, nsamp);
        buf_pos += nsamp;
        ticks_done++;

        if (tick % (TICK_RATE / 4) == 0) {
            printf("  %u%%\r", (unsigned)(tick * 100 / max_ticks));
            fflush(stdout);
        }
    }

    printf("\nRendered %u ticks, %u samples (%.1f sec)\n",
           ticks_done, buf_pos, (float)buf_pos / SAMPLE_RATE);
    fflush(stdout);

    /* ---------- Write WAV ---------- */
    FILE *fp = fopen(outfile, "wb");
    if (!fp) {
        fprintf(stderr, "Error: could not write '%s'\n", outfile);
        free(outbuf);
        wm_unload(&wm);
        return 1;
    }
    write_wav_header(fp, buf_pos);
    fwrite(outbuf, 4, buf_pos, fp);
    fclose(fp);
    printf("Written to '%s'\n", outfile);

    /* ---------- Cleanup ---------- */
    if (pc.log_fp) fclose(pc.log_fp);
    free(outbuf);
    wm_unload(&wm);
    return 0;
}
