#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include "sdl_audio.h"
#include "opl3.h"
#include "wm_loader.h"
#include "wm_replayer.h"

#define SAMPLE_RATE 44100
#define TICK_RATE   200

static opl3_chip    *g_chip;
static wm_replayer_t *g_rp;
static unsigned       g_tick_accum;
static int            g_song_ended;

static void opl_write(void *ctx, uint16_t reg, uint8_t val)
{
    OPL3_WriteRegBuffered((opl3_chip *)ctx, reg, val);
}

static void callback(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;
    int16_t *buf = (int16_t *)stream;
    uint32_t frames = (uint32_t)len / 4;

    for (uint32_t i = 0; i < frames; i++) {
        g_tick_accum += TICK_RATE;
        if (g_tick_accum >= SAMPLE_RATE) {
            g_tick_accum -= SAMPLE_RATE;
            if (!g_song_ended)
                if (!wm_replayer_tick(g_rp))
                    g_song_ended = 1;
        }
        OPL3_GenerateResampled(g_chip, buf + i * 2);
    }
}

int sdl_play(const wm_file_t *wm)
{
    opl3_chip chip;
    wm_replayer_t rp;

    OPL3_Reset(&chip, SAMPLE_RATE);
    wm_replayer_init(&rp, opl_write, &chip);
    wm_replayer_load(&rp, wm);

    g_chip = &chip;
    g_rp   = &rp;
    g_tick_accum = 0;
    g_song_ended = 0;

    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = callback;
    if (SDL_OpenAudio(&want, NULL) < 0) {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    printf("Playing... Press Ctrl+C to stop\n");
    SDL_PauseAudio(0);
    SDL_Delay(110000);
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
}
