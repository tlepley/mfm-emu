# MFMX v1 Specification

`MFMX` is a clean HDD-oriented container inspired by IMD concepts, with:

- physical sector order preserved,
- explicit per-sector integrity flags,
- binary payload separated from metadata tables.

All integer fields are little-endian.

On-disk serialization is canonical and byte-exact: fields are stored exactly at the byte offsets defined by this specification, and no padding bytes are present in the on-disk header representation.

## File Layout

The file is stored as:

1. Header (fixed 128 bytes)
2. Track table
3. Sector table
4. Data blob
5. Optional text block (not used in current implementation)

## Header (128 bytes)

| Field | Type | Description |
|---|---:|---|
| `magic` | `char[4]` | Must be `MFMX` |
| `version_major` | `u32` | Format major version (`1`) |
| `version_minor` | `u32` | Format minor version (`0`) |
| `flags` | `u32` | Global flags (reserved, `0` in v1) |
| `encoding_type` | `u32` | Encoding enum (`1` = MFM) |
| `cylinders` | `u32` | Nominal cylinder count |
| `heads` | `u32` | Nominal head count |
| `nominal_sectors_per_track` | `u32` | Nominal sectors per track |
| `nominal_sector_size` | `u32` | Nominal sector size in bytes |
| `track_count` | `u32` | Number of track entries |
| `sector_count` | `u32` | Number of sector entries |
| `off_track_table` | `u64` | Absolute offset of track table |
| `off_sector_table` | `u64` | Absolute offset of sector table |
| `off_data_blob` | `u64` | Absolute offset of data blob |
| `off_text_block` | `u64` | Absolute offset of text block (`0` if absent) |
| `len_track_table` | `u64` | Size in bytes |
| `len_sector_table` | `u64` | Size in bytes |
| `len_data_blob` | `u64` | Size in bytes |
| `len_text_block` | `u64` | Size in bytes (`0` if absent) |
| `header_crc32` | `u32` | CRC32 of header with this field set to `0` |
| `reserved` | bytes | Reserved/padding to 128 bytes |

## Track Table

Each `TrackEntry` is 24 bytes:

| Field | Type | Description |
|---|---:|---|
| `cyl` | `u32` | Physical cylinder |
| `head` | `u32` | Physical head |
| `first_sector_index` | `u32` | First sector index in sector table |
| `sector_count` | `u32` | Number of sectors linked to this track |
| `track_flags` | `u32` | Reserved (`0` in v1) |
| `reserved0` | `u32` | Reserved |

Tracks are listed in first-seen physical capture order.

## Sector Table

Each `SectorEntry` is 56 bytes:

| Field | Type | Description |
|---|---:|---|
| `track_index` | `u32` | Parent track index |
| `physical_index` | `u32` | Sector order on track (0..N-1) |
| `sector_id` | `u32` | Sector ID from decoded header |
| `cyl_id` | `u32` | Cylinder ID from decoded header |
| `head_id` | `u32` | Head ID from decoded header |
| `sector_size` | `u32` | Sector data length in bytes |
| `status` | `u32` | Status bitfield |
| `retry_count` | `u32` | Retry count (reserved, `0` in v1) |
| `data_offset` | `u64` | Offset relative to start of data blob |
| `data_length` | `u32` | Payload length in bytes |
| `data_crc32` | `u32` | CRC32 of payload |
| `reserved1` | `u32` | Reserved |
| `reserved2` | `u32` | Reserved |

### Sector Status Bits

| Bit | Mask | Meaning |
|---:|---:|---|
| 0 | `0x00000001` | `PRESENT`: payload exists |
| 1 | `0x00000002` | `HEADER_BAD`: header invalid when retained |
| 2 | `0x00000004` | `DATA_BAD_CRC`: data retained with bad CRC |
| 3 | `0x00000008` | `DELETED_MARK` (reserved in current writer) |
| 4 | `0x00000010` | `MARKED_BAD_OR_SPARE` |

## Data Blob

Raw sector payload bytes, concatenated.

Each sector payload is located using:

- `absolute_offset = off_data_blob + data_offset`
- `length = data_length`

## Encoding Enum

| Value | Meaning |
|---:|---|
| `0` | Unknown |
| `1` | MFM |
| `2` | FM |
| `3` | RLL |
| `4` | Other |

Current `mfm_read` writer always sets `encoding_type = 1` (MFM).

## Current Project Integration

`mfm_read` and `mfm_util` automatically write MFMX when:

- `--extracted_data_file` ends with `.mfmx`

For other extensions, legacy raw `.img` output behavior is unchanged.
