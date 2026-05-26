#ifndef WM_LOADER_H
#define WM_LOADER_H

#include <stdint.h>
#include <stddef.h>

#define WM_NCHANNELS 6
#define WM_SIG_SIZE 16
#define WM_SIG "OPL3 DATA       "

typedef struct {
    uint16_t track_offsets[6];
    uint16_t inst_offset;
    uint16_t eof_offset;
} wm_header_t;

typedef struct {
    uint8_t *data;
    size_t size;
    wm_header_t header;
    uint8_t *tracks[6];
    size_t track_lens[6];
    uint8_t *inst_table;
    size_t inst_table_len;
} wm_file_t;

int wm_load(const char *filename, wm_file_t *wm);
void wm_unload(wm_file_t *wm);

#endif
