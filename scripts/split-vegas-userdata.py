#!/usr/bin/env python3
"""Split the UFS userdata partition into Android userdata + an Omarchy partition.

DO NOT WRITE ITS OUTPUT TO THE PHONE: Motorola's LK refuses to boot any GPT but
its own (notes/internal-storage-20261010.md). Kept as a record of that attempt.

Works on saved GPT copies (primary LBA 0-7, backup LBA last-7..last) and writes
new copies next to them. Only entry 84's last LBA and a new entry 86 change;
the protective MBR, disk GUID and every other entry stay byte-identical, so
the vendor quirks (PMBR size 0, zero disk GUID) are preserved.
"""
import argparse
import struct
import uuid
import zlib
from pathlib import Path

SECTOR = 4096
HEADER = '<8sIIIIQQQQ16sQIII'
ENTRY = '<16s16sQQQ72s'
LINUX_FS = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4')
USERDATA_INDEX = 83   # sdc84, zero-based
NEW_INDEX = 85        # sdc86
FLASHINFO_INDEX = 84  # sdc85, must stay right after the new partition


def parse_header(raw):
    fields = list(struct.unpack(HEADER, raw[:92]))
    return fields


def pack_header(fields):
    fields = list(fields)
    fields[3] = 0
    raw = struct.pack(HEADER, *fields)
    fields[3] = zlib.crc32(raw)
    return struct.pack(HEADER, *fields)


def entry_name(raw):
    return struct.unpack(ENTRY, raw)[5].decode('utf-16-le').rstrip('\0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('backup_dir', type=Path)
    parser.add_argument('--android-gib', type=int, default=24)
    parser.add_argument('--part-uuid', help='fixed unique GUID for the new entry (default: random)')
    args = parser.parse_args()

    primary = bytearray((args.backup_dir / 'sdc-primary-lba0-7.bin').read_bytes())
    backup = bytearray((args.backup_dir / 'sdc-backup-lba31240184-end.bin').read_bytes())
    p_hdr = parse_header(primary[SECTOR:])
    b_hdr = parse_header(backup[7 * SECTOR:])
    sig, _, hsz, _, _, cur, alt, first, last, _, elba, count, esz, ecrc = p_hdr
    assert sig == b'EFI PART' and hsz == 92 and cur == 1 and elba == 2 and esz == 128
    assert b_hdr[5] == alt and b_hdr[6] == 1 and b_hdr[10] == alt - 7 and b_hdr[13] == ecrc
    table_bytes = count * esz
    entries = bytearray(primary[2 * SECTOR:2 * SECTOR + table_bytes])
    assert entries == backup[:table_bytes], 'primary and backup entry arrays differ'
    assert zlib.crc32(entries) == ecrc

    def entry(i):
        return entries[i * esz:(i + 1) * esz]

    user = list(struct.unpack(ENTRY, entry(USERDATA_INDEX)))
    flash = struct.unpack(ENTRY, entry(FLASHINFO_INDEX))
    assert entry_name(entry(USERDATA_INDEX)) == 'userdata'
    assert entry_name(entry(FLASHINFO_INDEX)) == 'flashinfo'
    assert entry(NEW_INDEX) == bytes(esz), 'entry 86 is not empty'
    assert user[3] + 1 == flash[2], 'userdata no longer ends right before flashinfo'

    android_sectors = args.android_gib * (1 << 30) // SECTOR
    new_user_last = user[2] + android_sectors - 1
    new_first = new_user_last + 1
    new_last = flash[2] - 1
    assert new_first % 256 == 0, 'new partition is not 1 MiB aligned'
    assert new_last - new_first > (60 << 30) // SECTOR

    user[3] = new_user_last
    entries[USERDATA_INDEX * esz:(USERDATA_INDEX + 1) * esz] = struct.pack(ENTRY, *user)
    part_uuid = uuid.UUID(args.part_uuid) if args.part_uuid else uuid.uuid4()
    name = 'omarchy'.encode('utf-16-le').ljust(72, b'\0')
    entries[NEW_INDEX * esz:(NEW_INDEX + 1) * esz] = struct.pack(
        ENTRY, LINUX_FS.bytes_le, part_uuid.bytes_le, new_first, new_last, 0, name)

    crc = zlib.crc32(entries)
    p_hdr[13] = b_hdr[13] = crc
    primary[2 * SECTOR:2 * SECTOR + table_bytes] = entries
    primary[SECTOR:SECTOR + 92] = pack_header(p_hdr)
    backup[:table_bytes] = entries
    backup[7 * SECTOR:7 * SECTOR + 92] = pack_header(b_hdr)

    (args.backup_dir / 'sdc-primary-lba0-7.new.bin').write_bytes(primary)
    (args.backup_dir / 'sdc-backup-lba31240184-end.new.bin').write_bytes(backup)
    gib = lambda n: n * SECTOR / (1 << 30)
    print(f'userdata: {user[2]}..{new_user_last} ({gib(new_user_last - user[2] + 1):.2f} GiB)')
    print(f'omarchy:  {new_first}..{new_last} ({gib(new_last - new_first + 1):.2f} GiB) uuid {part_uuid}')
    print(f'entries crc {crc:#010x}')


if __name__ == '__main__':
    main()
