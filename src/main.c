#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"
#include "wm_loader.h"
#include "wm_replayer.h"
#include "sdl_audio.h"

#define SAMPLE_RATE 44100
#define TICK_RATE   200
#define MAX_SECONDS 110

typedef struct {
    opl3_chip chip;
    wm_replayer_t rp;
    unsigned tick_sub;
    FILE *log_fp;
    unsigned current_tick;
} player_ctx_t;

static void opl_write_cb(void *ctx, uint16_t reg, uint8_t val)
{
    player_ctx_t *pc = (player_ctx_t *)ctx;
    OPL3_WriteRegBuffered(&pc->chip, reg, val);
    if (pc->log_fp) {
        int bank = (reg >> 8) & 1;
        fprintf(pc->log_fp, "%u %d 0x%02X 0x%02X\n",
                pc->current_tick, bank, reg & 0xFF, val);
    }
}

static void write_wav_header(FILE *fp, unsigned num_frames)
{
    unsigned short channels = 2;
    unsigned short bits = 16;
    unsigned short block_align = channels * (bits / 8);
    unsigned byte_rate = SAMPLE_RATE * block_align;
    unsigned data_bytes = num_frames * block_align;
    unsigned file_size = 36 + data_bytes;
    unsigned fmt_size = 16;
    unsigned short audio_fmt = 1;
    unsigned sample_rate = SAMPLE_RATE;

    fwrite("RIFF", 4, 1, fp);
    fwrite(&file_size, 4, 1, fp);
    fwrite("WAVE", 4, 1, fp);
    fwrite("fmt ", 4, 1, fp);
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

#undef main
int main(int argc, char **argv)
{
    const char *infile;
    const char *outfile = "output.wav";
    int wav_mode = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <file.wm>               SDL playback\n", argv[0]);
        fprintf(stderr, "  %s --wav <file.wm> [.wav]  render to WAV\n", argv[0]);
        return 1;
    }

    int arg = 1;
    if (strcmp(argv[arg], "--wav") == 0) {
        wav_mode = 1;
        arg++;
    }
    if (arg >= argc) {
        fprintf(stderr, "Missing input file\n");
        return 1;
    }
    infile = argv[arg++];
    if (wav_mode && arg < argc)
        outfile = argv[arg];

    /* ---------- Load WM ---------- */
    wm_file_t wm;
    if (wm_load(infile, &wm) != 0) {
        fprintf(stderr, "Error: could not load '%s'\n", infile);
        return 1;
    }
    printf("Loaded '%s': %zu bytes, %d tracks\n", infile, wm.size, WM_NCHANNELS);
    for (int i = 0; i < 6; i++)
        printf("  Track %d: offset=0x%04X, len=%zu bytes\n",
               i, wm.header.track_offsets[i], wm.track_lens[i]);
    printf("  Instrument table: offset=0x%04X, len=%zu bytes\n",
           wm.header.inst_offset, wm.inst_table_len);
    printf("  EOF: 0x%04X\n", wm.header.eof_offset);

    /* ---------- WAV mode ---------- */
    if (wav_mode) {
        player_ctx_t pc;
        memset(&pc, 0, sizeof(pc));
        OPL3_Reset(&pc.chip, SAMPLE_RATE);
        wm_replayer_init(&pc.rp, opl_write_cb, &pc);
        pc.tick_sub = 0;
        pc.current_tick = 0;

        pc.log_fp = fopen("opl_writes.log", "w");
        if (pc.log_fp) printf("Logging OPL3 writes to opl_writes.log\n");

        wm_replayer_load(&pc.rp, &wm);

        printf("Rendering to '%s'...\n", outfile);

        unsigned max_ticks = TICK_RATE * MAX_SECONDS;
        unsigned duration_samples = max_ticks * (SAMPLE_RATE / TICK_RATE + 1) + SAMPLE_RATE;
        int16_t *outbuf = (int16_t *)malloc(duration_samples * 2 * sizeof(int16_t));
        if (!outbuf) { wm_unload(&wm); return 1; }

        unsigned buf_pos = 0;
        unsigned ticks_done = 0;
        int song_ended = 0;

        for (unsigned tick = 0; tick < max_ticks; tick++) {
            pc.current_tick = tick;
            if (!song_ended && !wm_replayer_tick(&pc.rp)) {
                printf("  Song ended at tick %u\n", tick);
                song_ended = 1;
            }
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

        printf("\nRendered %u ticks, %u frames (%.1f sec)\n",
               ticks_done, buf_pos, (float)buf_pos / SAMPLE_RATE);

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

        if (pc.log_fp) fclose(pc.log_fp);
        free(outbuf);
        wm_unload(&wm);
        return 0;
    }

    /* ---------- SDL playback ---------- */
    int ret = sdl_play(&wm);
    wm_unload(&wm);
    return ret;
}
