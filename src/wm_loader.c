#include "wm_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wm_load(const char *filename, wm_file_t *wm) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 32 || sz > 65536L) { fclose(fp); return -1; }

    wm->data = (uint8_t *)malloc((size_t)sz);
    if (!wm->data) { fclose(fp); return -1; }

    if (fread(wm->data, 1, (size_t)sz, fp) != (size_t)sz) {
        free(wm->data); fclose(fp); return -1;
    }
    fclose(fp);

    wm->size = (size_t)sz;

    /* validate 16-byte signature at offset 0 */
    if (memcmp(wm->data, WM_SIG, WM_SIG_SIZE) != 0) {
        free(wm->data); wm->data = NULL; return -1;
    }

    /* header at offset 0x10: 6× track offset u16, 1× inst offset u16, 1× eof offset u16 */
    if ((size_t)0x10 + 8 * sizeof(uint16_t) > wm->size) {
        free(wm->data); wm->data = NULL; return -1;
    }

    const uint8_t *hdr = wm->data + 0x10;
    for (int i = 0; i < 6; i++) {
        wm->header.track_offsets[i] = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        hdr += 2;
    }
    wm->header.inst_offset  = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8)); hdr += 2;
    wm->header.eof_offset   = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));

    /* clamp offsets to file size */
    for (int i = 0; i < 6; i++) {
        uint32_t off = wm->header.track_offsets[i];
        if (off >= wm->size) { off = 0; wm->header.track_offsets[i] = 0; }
    }
    if (wm->header.inst_offset >= wm->size) wm->header.inst_offset = 0;
    if (wm->header.eof_offset  >= wm->size) wm->header.eof_offset  = (uint16_t)wm->size;

    /* Tracks share a flat buffer in the ASM.  Each channel reads from its
       track_offset through EOF — no per-track boundaries (the ASM has none). */
    uint32_t data_end = wm->header.eof_offset;
    if (data_end == 0 || data_end > wm->size) data_end = (uint32_t)wm->size;
    for (int i = 0; i < 6; i++) {
        uint32_t start = wm->header.track_offsets[i];
        if (start == 0) { wm->tracks[i] = NULL; wm->track_lens[i] = 0; continue; }
        wm->tracks[i] = wm->data + start;
        wm->track_lens[i] = data_end - start;
    }

    /* instrument table: from inst_offset to next boundary (eof or next track) */
    uint32_t inst_start = wm->header.inst_offset;
    if (inst_start > 0 && inst_start < wm->size) {
        uint32_t inst_end = wm->size;
        for (int i = 0; i < 6; i++) {
            if (wm->header.track_offsets[i] > inst_start && wm->header.track_offsets[i] < inst_end)
                inst_end = wm->header.track_offsets[i];
        }
        if (wm->header.eof_offset > inst_start && wm->header.eof_offset < inst_end)
            inst_end = wm->header.eof_offset;
        wm->inst_table = wm->data + inst_start;
        wm->inst_table_len = inst_end - inst_start;
    } else {
        wm->inst_table = NULL;
        wm->inst_table_len = 0;
    }

    return 0;
}

void wm_unload(wm_file_t *wm) {
    free(wm->data);
    wm->data = NULL;
    wm->size = 0;
}
