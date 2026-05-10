#ifndef MFMX_H_
#define MFMX_H_

#include <stdint.h>

#include "crc_ecc.h"
#include "emu_tran_file.h"
#include "mfm_decoder.h"

int mfmx_is_filename(const char *filename);
void mfmx_setup(DRIVE_PARAMS *drive_params);
int mfmx_is_active(void);
void mfmx_note_sector_header(DRIVE_PARAMS *drive_params,
      SECTOR_STATUS *sector_status);
void mfmx_record_sector(DRIVE_PARAMS *drive_params, SECTOR_STATUS *sector_status,
      const uint8_t *bytes, uint32_t sector_size);
void mfmx_finish_track_read(DRIVE_PARAMS *drive_params, int cyl, int head);
void mfmx_done(DRIVE_PARAMS *drive_params);

#endif
