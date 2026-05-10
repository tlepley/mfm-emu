# MFM Transition File Format (`.transitions`)

This document describes the transition file format used by `mfm_read` and
`mfm_util` (implementation in `mfm/emu_tran_file.c`).

All values are little-endian.

## Purpose

A transition file stores raw magnetic transition timing as delta counts between
rising edges. It is the highest-fidelity capture format used by this project
for offline decode/redecode.

## High-Level Layout

1. File header
2. Track records (one record per read track, in write order)
3. End-of-file track marker record (`cyl=-1`, `head=-1`)

## File Header

Fields in order:

1. `uint8_t[8]` file id:
   - `EE 4D 46 4D 0A 1A 0A 0D`
2. `uint32_t` file type/version:
   - current value: `0x01020200`
3. `uint32_t` offset to first track header (bytes from file start)
4. `uint32_t` track header size in bytes
5. `uint32_t` cylinder count
6. `uint32_t` head count
7. `uint32_t` sample rate in Hz (current tools support `200000000` only)
8. `uint32_t` command line length in bytes (includes trailing `\0`)
9. `uint8_t[n]` command line string (`\0` terminated)
10. `uint32_t` note length in bytes (includes trailing `\0`)
11. `uint8_t[n]` note string (`\0` terminated)
12. `uint32_t` start time from index in ns (`start_time_ns`)
13. `uint32_t` header checksum

### Header Checksum

Checksum is computed with project CRC settings:

- polynomial: `0x140a0445`
- length: `32`
- initial value: `0xffffffff`

The writer appends the current CRC state as the checksum field.

## Track Record

Each track record is:

1. `int32_t cyl`
2. `int32_t head`
3. `uint32_t transition_data_length_bytes`
4. `uint8_t[data_length]` packed transition data
5. `uint32_t` track checksum (header + data, same CRC settings)

End-of-file marker record:

- `cyl = -1`
- `head = -1`
- `transition_data_length_bytes = 0`
- checksum still present

## Transition Data Packing

Transitions are stored as variable-length integers:

- byte `0..253`: value is this byte
- byte `254`: next 2 bytes are a 16-bit value (little-endian)
- byte `255`: next 3 bytes are a 24-bit value (little-endian)

In current capture path, deltas are 16-bit internally, so `255` is reserved for
future/extended use.

## Important Behavior: Retries

When `mfm_read` performs retries, **each retry read of a track is appended as a
new track record** in the transition file.

That means a `(cyl,head)` may appear multiple times. Offline decode (`mfm_util`)
replays these records and keeps best sector data via decode logic.

## Versioning Notes

- File type is encoded in high byte of version word (`0x01` = transitions).
- Major/minor revisions are encoded in following bytes.
- Reader accepts compatible older/minor variants and uses header-size fields to
  skip unknown future header additions where possible.

## Reference Implementation

- Writer:
  - `tran_file_write_header`
  - `tran_file_write_track_deltas`
- Reader:
  - `tran_file_read_header`
  - `tran_file_read_track_deltas`
- File:
  - `mfm/emu_tran_file.c`
