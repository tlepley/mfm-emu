#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "msg.h"
#include "mfmx.h"

#define MFMX_MAGIC "MFMX"
#define MFMX_VERSION_MAJOR 1u
#define MFMX_VERSION_MINOR 0u
#define MFMX_ENCODING_MFM 1u

#define MFMX_STATUS_PRESENT 0x00000001u
#define MFMX_STATUS_HEADER_BAD 0x00000002u
#define MFMX_STATUS_DATA_BAD_CRC 0x00000004u
#define MFMX_STATUS_DELETED_MARK 0x00000008u
#define MFMX_STATUS_MARKED_BAD_OR_SPARE 0x00000010u

typedef struct {
   uint32_t cyl;
   uint32_t head;
   uint32_t first_sector_index;
   uint32_t sector_count;
   uint32_t track_flags;
   uint32_t reserved0;
} MFMX_TRACK_ENTRY;

typedef struct {
   uint32_t track_index;
   uint32_t physical_index;
   uint32_t sector_id;
   uint32_t cyl_id;
   uint32_t head_id;
   uint32_t sector_size;
   uint32_t status;
   uint32_t retry_count;
   uint64_t data_offset;
   uint32_t data_length;
   uint32_t data_crc32;
   uint32_t reserved1;
   uint32_t reserved2;
} MFMX_SECTOR_ENTRY;

typedef struct __attribute__((packed)) {
   uint8_t magic[4];
   uint32_t version_major;
   uint32_t version_minor;
   uint32_t flags;
   uint32_t encoding_type;
   uint32_t cylinders;
   uint32_t heads;
   uint32_t nominal_sectors_per_track;
   uint32_t nominal_sector_size;
   uint32_t track_count;
   uint32_t sector_count;
   uint64_t off_track_table;
   uint64_t off_sector_table;
   uint64_t off_data_blob;
   uint64_t off_text_block;
   uint64_t len_track_table;
   uint64_t len_sector_table;
   uint64_t len_data_blob;
   uint64_t len_text_block;
   uint32_t header_crc32;
   uint8_t reserved[16];
} MFMX_HEADER;

typedef char MFMX_HEADER_MUST_BE_128_BYTES[(sizeof(MFMX_HEADER) == 128u) ? 1 : -1];

typedef struct {
   int active;
   int data_fd;
   char temp_path[128];
   uint64_t data_len;
   int32_t *logical_to_sector;
   uint32_t logical_count;
   uint32_t sectors_per_track;
   MFMX_TRACK_ENTRY *tracks;
   uint32_t num_tracks;
   uint32_t cap_tracks;
   MFMX_SECTOR_ENTRY *sectors;
   uint32_t num_sectors;
   uint32_t cap_sectors;
   uint32_t *track_merged_order;
   uint32_t *track_merged_count;
   uint32_t *current_read_order;
   uint8_t *current_read_seen;
   uint32_t current_read_count;
   uint32_t current_read_track_index;
   int current_read_track_valid;
} MFMX_CONTEXT;

static MFMX_CONTEXT g_mfmx;


static void reset_current_read_order(void) {
   if (g_mfmx.current_read_seen != NULL) {
      memset(g_mfmx.current_read_seen, 0, g_mfmx.sectors_per_track);
   }
   g_mfmx.current_read_count = 0;
   g_mfmx.current_read_track_valid = 0;
}

static void merge_current_read_order(void) {
   if (!g_mfmx.current_read_track_valid) {
      return;
   }

   uint32_t track_index = g_mfmx.current_read_track_index;
   uint32_t *merged = g_mfmx.track_merged_order +
      track_index * g_mfmx.sectors_per_track;
   uint32_t *merged_count = &g_mfmx.track_merged_count[track_index];
   uint32_t cursor = 0;
   uint32_t rotated_count = g_mfmx.current_read_count;
   uint32_t rotated[g_mfmx.sectors_per_track];

   if (*merged_count == 0) {
      memcpy(merged, g_mfmx.current_read_order,
         g_mfmx.current_read_count * sizeof(*merged));
      *merged_count = g_mfmx.current_read_count;
      reset_current_read_order();
      return;
   }

   uint32_t best_start = 0;
   uint32_t best_cursor = 0xffffffffu;
   for (uint32_t i = 0; i < g_mfmx.current_read_count; i++) {
      for (uint32_t j = 0; j < *merged_count; j++) {
         if (g_mfmx.current_read_order[i] == merged[j] && j < best_cursor) {
            best_start = i;
            best_cursor = j;
            break;
         }
      }
   }
   if (best_cursor != 0xffffffffu) {
      cursor = best_cursor;
      for (uint32_t i = 0; i < g_mfmx.current_read_count; i++) {
         rotated[i] = g_mfmx.current_read_order[(best_start + i) %
            g_mfmx.current_read_count];
      }
   } else {
      memcpy(rotated, g_mfmx.current_read_order,
         g_mfmx.current_read_count * sizeof(*rotated));
   }

   for (uint32_t i = 0; i < rotated_count; i++) {
      uint32_t sector_id = rotated[i];
      uint32_t j;

      if (cursor < *merged_count && merged[cursor] == sector_id) {
         cursor++;
         continue;
      }
      for (j = cursor; j < *merged_count; j++) {
         if (merged[j] == sector_id) {
            cursor = j + 1;
            break;
         }
      }
      if (j != *merged_count) {
         continue;
      }
      if (*merged_count >= g_mfmx.sectors_per_track) {
         break;
      }
      memmove(&merged[cursor + 1], &merged[cursor],
         (*merged_count - cursor) * sizeof(*merged));
      merged[cursor] = sector_id;
      (*merged_count)++;
      cursor++;
   }
   reset_current_read_order();
}

static void track_current_read_order(uint32_t track_index,
      DRIVE_PARAMS *drive_params, SECTOR_STATUS *sector_status) {

   int rel_sector = sector_status->sector - drive_params->first_sector_number;
   if (rel_sector < 0 || (uint32_t) rel_sector >= g_mfmx.sectors_per_track) {
      return;
   }

   if (g_mfmx.current_read_track_valid &&
         g_mfmx.current_read_track_index != track_index) {
      merge_current_read_order();
   }
   if (!g_mfmx.current_read_track_valid) {
      g_mfmx.current_read_track_index = track_index;
      g_mfmx.current_read_track_valid = 1;
   }
   if (g_mfmx.current_read_seen[rel_sector]) {
      return;
   }
   g_mfmx.current_read_seen[rel_sector] = 1;
   g_mfmx.current_read_order[g_mfmx.current_read_count++] =
      (uint32_t) sector_status->sector;
}

static uint32_t merged_physical_index(uint32_t track_index,
      uint32_t sector_id, uint32_t fallback_index) {
   uint32_t count = g_mfmx.track_merged_count[track_index];
   uint32_t *merged = g_mfmx.track_merged_order +
      track_index * g_mfmx.sectors_per_track;

   for (uint32_t i = 0; i < count; i++) {
      if (merged[i] == sector_id) {
         return i;
      }
   }
   return fallback_index;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t len) {
   uint32_t crc = 0xffffffffu;
   size_t i;
   for (i = 0; i < len; i++) {
      uint32_t c = (crc ^ data[i]) & 0xffu;
      int j;
      for (j = 0; j < 8; j++) {
         c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
      }
      crc = (crc >> 8) ^ c;
   }
   return crc ^ 0xffffffffu;
}

static void grow_tracks_if_needed(void) {
   if (g_mfmx.num_tracks < g_mfmx.cap_tracks) {
      return;
   }
   uint32_t old_cap = g_mfmx.cap_tracks;
   uint32_t new_cap = g_mfmx.cap_tracks ? g_mfmx.cap_tracks * 2u : 64u;
   void *p = realloc(g_mfmx.tracks, new_cap * sizeof(*g_mfmx.tracks));
   if (p == NULL) {
      msg(MSG_FATAL, "MFMX realloc tracks failed\n");
      exit(1);
   }
   g_mfmx.tracks = p;

   p = realloc(g_mfmx.track_merged_order,
      (size_t) new_cap * g_mfmx.sectors_per_track * sizeof(*g_mfmx.track_merged_order));
   if (p == NULL) {
      msg(MSG_FATAL, "MFMX realloc track order failed\n");
      exit(1);
   }
   g_mfmx.track_merged_order = p;

   p = realloc(g_mfmx.track_merged_count,
      new_cap * sizeof(*g_mfmx.track_merged_count));
   if (p == NULL) {
      msg(MSG_FATAL, "MFMX realloc track order counts failed\n");
      exit(1);
   }
   g_mfmx.track_merged_count = p;

   memset(g_mfmx.track_merged_order +
      (size_t) old_cap * g_mfmx.sectors_per_track, 0,
      (size_t) (new_cap - old_cap) * g_mfmx.sectors_per_track *
      sizeof(*g_mfmx.track_merged_order));
   memset(g_mfmx.track_merged_count + old_cap, 0,
      (new_cap - old_cap) * sizeof(*g_mfmx.track_merged_count));

   g_mfmx.cap_tracks = new_cap;
}

static void grow_sectors_if_needed(void) {
   if (g_mfmx.num_sectors < g_mfmx.cap_sectors) {
      return;
   }
   uint32_t new_cap = g_mfmx.cap_sectors ? g_mfmx.cap_sectors * 2u : 1024u;
   void *p = realloc(g_mfmx.sectors, new_cap * sizeof(*g_mfmx.sectors));
   if (p == NULL) {
      msg(MSG_FATAL, "MFMX realloc sectors failed\n");
      exit(1);
   }
   g_mfmx.sectors = p;
   g_mfmx.cap_sectors = new_cap;
}

static uint32_t get_or_create_track(uint32_t cyl, uint32_t head) {
   uint32_t i;
   for (i = 0; i < g_mfmx.num_tracks; i++) {
      if (g_mfmx.tracks[i].cyl == cyl && g_mfmx.tracks[i].head == head) {
         return i;
      }
   }
   grow_tracks_if_needed();
   g_mfmx.tracks[g_mfmx.num_tracks].cyl = cyl;
   g_mfmx.tracks[g_mfmx.num_tracks].head = head;
   g_mfmx.tracks[g_mfmx.num_tracks].first_sector_index = g_mfmx.num_sectors;
   g_mfmx.tracks[g_mfmx.num_tracks].sector_count = 0;
   g_mfmx.tracks[g_mfmx.num_tracks].track_flags = 0;
   g_mfmx.tracks[g_mfmx.num_tracks].reserved0 = 0;
   g_mfmx.num_tracks++;
   return g_mfmx.num_tracks - 1;
}

static uint32_t canonical_physical_index(DRIVE_PARAMS *drive_params,
      SECTOR_STATUS *sector_status, uint32_t track_index) {
   if (drive_params->sector_numbers != NULL) {
      int i;
      for (i = 0; i < drive_params->num_sectors; i++) {
         if (drive_params->sector_numbers[i] == sector_status->sector) {
            return (uint32_t) i;
         }
      }
   }
   return g_mfmx.tracks[track_index].sector_count;
}

static int compare_sector_entries(const void *va, const void *vb) {
   const MFMX_SECTOR_ENTRY *a = va;
   const MFMX_SECTOR_ENTRY *b = vb;

   if (a->track_index != b->track_index) {
      return (a->track_index < b->track_index) ? -1 : 1;
   }
   if (a->physical_index != b->physical_index) {
      return (a->physical_index < b->physical_index) ? -1 : 1;
   }
   if (a->sector_id != b->sector_id) {
      return (a->sector_id < b->sector_id) ? -1 : 1;
   }
   return 0;
}

static int32_t logical_index_from_sector(DRIVE_PARAMS *drive_params,
      SECTOR_STATUS *sector_status) {
   if (sector_status->is_lba) {
      if (sector_status->lba_addr < 0 || (uint32_t) sector_status->lba_addr >=
            g_mfmx.logical_count) {
         return -1;
      }
      return sector_status->lba_addr;
   }

   if (sector_status->cyl < 0 || sector_status->head < 0) {
      return -1;
   }
   if (sector_status->cyl >= drive_params->num_cyl ||
         sector_status->head >= drive_params->num_head) {
      return -1;
   }
   int rel_sector = sector_status->sector - drive_params->first_sector_number;
   if (rel_sector < 0 || rel_sector >= drive_params->num_sectors) {
      return -1;
   }

   return rel_sector +
      sector_status->head * drive_params->num_sectors +
      sector_status->cyl * drive_params->num_sectors * drive_params->num_head;
}

int mfmx_is_filename(const char *filename) {
   if (filename == NULL) {
      return 0;
   }
   size_t len = strlen(filename);
   return len >= 5 && strcmp(filename + len - 5, ".mfmx") == 0;
}

int mfmx_is_active(void) {
   return g_mfmx.active;
}

void mfmx_setup(DRIVE_PARAMS *drive_params) {
   memset(&g_mfmx, 0, sizeof(g_mfmx));
   g_mfmx.data_fd = -1;
   if (!mfmx_is_filename(drive_params->extract_filename)) {
      return;
   }

   g_mfmx.logical_count = drive_params->num_cyl * drive_params->num_head *
      drive_params->num_sectors;
   g_mfmx.sectors_per_track = drive_params->num_sectors;
   g_mfmx.logical_to_sector = malloc((size_t) g_mfmx.logical_count * sizeof(int32_t));
   if (g_mfmx.logical_to_sector == NULL) {
      msg(MSG_FATAL, "MFMX alloc map failed\n");
      exit(1);
   }
   for (uint32_t i = 0; i < g_mfmx.logical_count; i++) {
      g_mfmx.logical_to_sector[i] = -1;
   }

   g_mfmx.current_read_order = malloc(
      g_mfmx.sectors_per_track * sizeof(*g_mfmx.current_read_order));
   g_mfmx.current_read_seen = malloc(g_mfmx.sectors_per_track);
   if (g_mfmx.current_read_order == NULL || g_mfmx.current_read_seen == NULL) {
      msg(MSG_FATAL, "MFMX alloc current order failed\n");
      exit(1);
   }
   reset_current_read_order();

   strcpy(g_mfmx.temp_path, "/tmp/mfmx-data-XXXXXX");
   g_mfmx.data_fd = mkstemp(g_mfmx.temp_path);
   if (g_mfmx.data_fd < 0) {
      msg(MSG_FATAL, "MFMX temp file create failed: %s\n", strerror(errno));
      exit(1);
   }
   g_mfmx.active = 1;
}

void mfmx_note_sector_header(DRIVE_PARAMS *drive_params,
      SECTOR_STATUS *sector_status) {
   if (!g_mfmx.active) {
      return;
   }

   uint32_t track_index = get_or_create_track((uint32_t) sector_status->cyl,
      (uint32_t) sector_status->head);
   track_current_read_order(track_index, drive_params, sector_status);
}

void mfmx_record_sector(DRIVE_PARAMS *drive_params, SECTOR_STATUS *sector_status,
      const uint8_t *bytes, uint32_t sector_size) {
   if (!g_mfmx.active) {
      return;
   }

   int32_t logical_index = logical_index_from_sector(drive_params, sector_status);
   int32_t existing = -1;
   if (logical_index >= 0) {
      existing = g_mfmx.logical_to_sector[logical_index];
   }

   uint32_t track_index = get_or_create_track((uint32_t) sector_status->cyl,
      (uint32_t) sector_status->head);
   uint32_t status = 0;
   if (bytes != NULL) {
      status |= MFMX_STATUS_PRESENT;
   }
   if (sector_status->status & SECT_BAD_HEADER) {
      status |= MFMX_STATUS_HEADER_BAD;
   }
   if (sector_status->status & SECT_BAD_DATA) {
      status |= MFMX_STATUS_DATA_BAD_CRC;
   }
   if (sector_status->status & SECT_SPARE_BAD) {
      status |= MFMX_STATUS_MARKED_BAD_OR_SPARE;
   }

   if (bytes != NULL &&
         write(g_mfmx.data_fd, bytes, sector_size) != (ssize_t) sector_size) {
      msg(MSG_FATAL, "MFMX write payload failed: %s\n", strerror(errno));
      exit(1);
   }

   MFMX_SECTOR_ENTRY entry;
   memset(&entry, 0, sizeof(entry));
   entry.track_index = track_index;
   entry.physical_index = canonical_physical_index(drive_params,
      sector_status, track_index);
   entry.sector_id = (uint32_t) sector_status->sector;
   entry.cyl_id = (uint32_t) sector_status->cyl;
   entry.head_id = (uint32_t) sector_status->head;
   entry.sector_size = sector_size;
   entry.status = status;
   entry.retry_count = 0;
   entry.data_offset = g_mfmx.data_len;
   entry.data_length = 0;
   entry.data_crc32 = 0;

   if (bytes != NULL) {
      entry.data_length = sector_size;
      entry.data_crc32 = crc32_ieee(bytes, sector_size);
      g_mfmx.data_len += sector_size;
   }

   if (existing >= 0) {
      if (bytes == NULL) {
         return;
      }
      entry.physical_index = g_mfmx.sectors[existing].physical_index;
      entry.track_index = g_mfmx.sectors[existing].track_index;
      g_mfmx.sectors[existing] = entry;
   } else {
      grow_sectors_if_needed();
      g_mfmx.sectors[g_mfmx.num_sectors] = entry;
      if (logical_index >= 0) {
         g_mfmx.logical_to_sector[logical_index] = (int32_t) g_mfmx.num_sectors;
      }
      g_mfmx.num_sectors++;
      g_mfmx.tracks[track_index].sector_count++;
   }
}

void mfmx_finish_track_read(DRIVE_PARAMS *drive_params, int cyl, int head) {
   (void) drive_params;
   (void) cyl;
   (void) head;

   if (!g_mfmx.active) {
      return;
   }
   merge_current_read_order();
}

void mfmx_done(DRIVE_PARAMS *drive_params) {
   if (!g_mfmx.active) {
      return;
   }

   MFMX_HEADER header;
   MFMX_TRACK_ENTRY *sorted_tracks = NULL;
   MFMX_SECTOR_ENTRY *sorted_sectors = NULL;
   uint32_t sector_cursor = 0;
   uint32_t i;
   memset(&header, 0, sizeof(header));

   if (g_mfmx.num_tracks > 0) {
      sorted_tracks = malloc((size_t) g_mfmx.num_tracks * sizeof(*sorted_tracks));
      if (sorted_tracks == NULL) {
         msg(MSG_FATAL, "MFMX alloc sorted tracks failed\n");
         exit(1);
      }
   }
   if (g_mfmx.num_sectors > 0) {
      sorted_sectors = malloc((size_t) g_mfmx.num_sectors * sizeof(*sorted_sectors));
      if (sorted_sectors == NULL) {
         msg(MSG_FATAL, "MFMX alloc sorted sectors failed\n");
         exit(1);
      }
   }

   for (i = 0; i < g_mfmx.num_tracks; i++) {
      uint32_t j;
      uint32_t count = 0;

      sorted_tracks[i] = g_mfmx.tracks[i];
      sorted_tracks[i].first_sector_index = sector_cursor;

      for (j = 0; j < g_mfmx.num_sectors; j++) {
         if (g_mfmx.sectors[j].track_index != i) {
            continue;
         }
         sorted_sectors[sector_cursor + count] = g_mfmx.sectors[j];
         sorted_sectors[sector_cursor + count].physical_index = merged_physical_index(
            i, sorted_sectors[sector_cursor + count].sector_id,
            sorted_sectors[sector_cursor + count].physical_index);
         count++;
      }
      if (count != g_mfmx.tracks[i].sector_count) {
         msg(MSG_FATAL, "MFMX sector count mismatch for track %u\n", i);
         exit(1);
      }
      if (count > 1) {
         qsort(&sorted_sectors[sector_cursor], count,
            sizeof(*sorted_sectors), compare_sector_entries);
      }
      sector_cursor += count;
   }

   const uint64_t len_track_table =
      (uint64_t) g_mfmx.num_tracks * sizeof(MFMX_TRACK_ENTRY);
   const uint64_t len_sector_table =
      (uint64_t) g_mfmx.num_sectors * sizeof(MFMX_SECTOR_ENTRY);

   memcpy(header.magic, MFMX_MAGIC, 4);
   header.version_major = MFMX_VERSION_MAJOR;
   header.version_minor = MFMX_VERSION_MINOR;
   header.flags = 0;
   header.encoding_type = MFMX_ENCODING_MFM;
   header.cylinders = (uint32_t) drive_params->num_cyl;
   header.heads = (uint32_t) drive_params->num_head;
   header.nominal_sectors_per_track = (uint32_t) drive_params->num_sectors;
   header.nominal_sector_size = (uint32_t) drive_params->sector_size;
   header.track_count = g_mfmx.num_tracks;
   header.sector_count = g_mfmx.num_sectors;
   header.off_track_table = (uint64_t) sizeof(MFMX_HEADER);
   header.off_sector_table = header.off_track_table + len_track_table;
   header.off_data_blob = header.off_sector_table + len_sector_table;
   header.len_track_table = len_track_table;
   header.len_sector_table = len_sector_table;
   header.len_data_blob = g_mfmx.data_len;
   header.off_text_block = header.off_data_blob + header.len_data_blob;
   header.len_text_block = 0;

   header.header_crc32 = 0;
   header.header_crc32 = crc32_ieee((const uint8_t *) &header, sizeof(header));

   int out = open(drive_params->extract_filename, O_WRONLY | O_CREAT | O_TRUNC, 0664);
   if (out < 0) {
      msg(MSG_FATAL, "MFMX output open failed: %s\n", strerror(errno));
      exit(1);
   }

   if (write(out, &header, sizeof(header)) != (ssize_t) sizeof(header)) {
      msg(MSG_FATAL, "MFMX write header failed: %s\n", strerror(errno));
      exit(1);
   }
   if (len_track_table > 0 &&
         write(out, sorted_tracks, (size_t) len_track_table) !=
         (ssize_t) len_track_table) {
      msg(MSG_FATAL, "MFMX write track table failed: %s\n", strerror(errno));
      exit(1);
   }
   if (len_sector_table > 0 &&
         write(out, sorted_sectors, (size_t) len_sector_table) !=
         (ssize_t) len_sector_table) {
      msg(MSG_FATAL, "MFMX write sector table failed: %s\n", strerror(errno));
      exit(1);
   }

   if (lseek(g_mfmx.data_fd, 0, SEEK_SET) < 0) {
      msg(MSG_FATAL, "MFMX seek temp data failed: %s\n", strerror(errno));
      exit(1);
   }
   char buf[65536];
   for (;;) {
      ssize_t n = read(g_mfmx.data_fd, buf, sizeof(buf));
      if (n == 0) {
         break;
      }
      if (n < 0) {
         msg(MSG_FATAL, "MFMX read temp data failed: %s\n", strerror(errno));
         exit(1);
      }
      if (write(out, buf, (size_t) n) != n) {
         msg(MSG_FATAL, "MFMX write data blob failed: %s\n", strerror(errno));
         exit(1);
      }
   }

   close(out);
   close(g_mfmx.data_fd);
   unlink(g_mfmx.temp_path);
   free(sorted_tracks);
   free(sorted_sectors);
   free(g_mfmx.current_read_order);
   free(g_mfmx.current_read_seen);
   free(g_mfmx.track_merged_order);
   free(g_mfmx.track_merged_count);
   free(g_mfmx.logical_to_sector);
   free(g_mfmx.tracks);
   free(g_mfmx.sectors);
   memset(&g_mfmx, 0, sizeof(g_mfmx));
}
