#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opl3.h"
#include "wm_loader.h"
#include "wm_replayer.h"
#include "sdl_audio.h"

#define SAMPLE_RATE 44100
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
    unsigned max_seconds = MAX_SECONDS;

    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s <file.wm>                         SDL playback\n", argv[0]);
        fprintf(stderr, "  %s --wav [-t N] <file.wm> [.wav]   render to WAV (N = max seconds, default %u)\n", argv[0], MAX_SECONDS);
        return 1;
    }

    int arg = 1;
    while (arg < argc && argv[arg][0] == '-') {
        if (strcmp(argv[arg], "--wav") == 0) {
            wav_mode = 1;
            arg++;
        } else if (strcmp(argv[arg], "--max-seconds") == 0 || strcmp(argv[arg], "-t") == 0) {
            if (arg + 1 >= argc) {
                fprintf(stderr, "Error: --max-seconds requires a number\n");
                return 1;
            }
            max_seconds = (unsigned)atoi(argv[arg + 1]);
            if (max_seconds == 0) max_seconds = 30;
            arg += 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg]);
            return 1;
        }
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

        unsigned max_samples = SAMPLE_RATE * max_seconds;
        int16_t *outbuf = (int16_t *)malloc(max_samples * 2 * sizeof(int16_t));
        if (!outbuf) { wm_unload(&wm); return 1; }

        unsigned buf_pos = 0;
        unsigned ticks_done = 0;
        int song_ended = 0;
        unsigned decay_frames = SAMPLE_RATE; /* 1 second of reverb tail */

        while (buf_pos < max_samples) {
            unsigned cur_rate = pc.rp.tick_rate ? pc.rp.tick_rate : 200;

            if (!song_ended) {
                pc.current_tick = ticks_done;
                if (!wm_replayer_tick(&pc.rp)) {
                    printf("  Song ended at tick %u\n", ticks_done);
                    printf("  f8_iterations: [%d,%d,%d,%d,%d,%d]\n",
                           f8_iterations[0], f8_iterations[1], f8_iterations[2],
                           f8_iterations[3], f8_iterations[4], f8_iterations[5]);
                    printf("  f8_calls:      [%d,%d,%d,%d,%d,%d]\n",
                           f8_calls[0], f8_calls[1], f8_calls[2],
                           f8_calls[3], f8_calls[4], f8_calls[5]);
                    printf("  f4_pushes:    [%d,%d,%d,%d,%d,%d]\n",
                            f4_push_count[0], f4_push_count[1], f4_push_count[2],
                            f4_push_count[3], f4_push_count[4], f4_push_count[5]);
                    int ac[6];
                    wm_apply_counts(ac);
                    printf("  apply_calls:  [%d,%d,%d,%d,%d,%d]\n",
                           ac[0], ac[1], ac[2], ac[3], ac[4], ac[5]);
                    extern int load_counts[6];
                    printf("  load_counts:  [%d,%d,%d,%d,%d,%d]\n",
                           load_counts[0], load_counts[1], load_counts[2],
                           load_counts[3], load_counts[4], load_counts[5]);
                    song_ended = 1;
                }
            } else if (decay_frames == 0) {
                break;
            }

            /* Accumulate and drain samples for one logical tick */
            pc.tick_sub += SAMPLE_RATE;
            unsigned nsamp = pc.tick_sub / cur_rate;
            pc.tick_sub -= nsamp * cur_rate;

            if (buf_pos + nsamp > max_samples)
                nsamp = max_samples - buf_pos;

            OPL3_GenerateStream(&pc.chip, outbuf + buf_pos * 2, nsamp);
            buf_pos += nsamp;
            ticks_done++;

            if (song_ended) {
                if (decay_frames >= nsamp)
                    decay_frames -= nsamp;
                else
                    break;
            }

            if (ticks_done % (cur_rate / 4) == 0) {
                printf("  %u ticks\r", ticks_done);
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
