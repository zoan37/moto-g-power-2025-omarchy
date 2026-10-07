#!/usr/bin/env python3
"""Query stock Mali Job Manager version/properties; submits no GPU jobs."""
import ctypes
import fcntl
import json
import os
import struct

fd = os.open('/dev/mali0', os.O_RDWR | os.O_CLOEXEC)
try:
    # r38p1 JM UAPI: _IOWR(0x80, 0, {u16 major, u16 minor}).
    version = bytearray(struct.pack('HH', 11, 38))
    fcntl.ioctl(fd, 0xc0048000, version, True)
    major, minor = struct.unpack('HH', version)
    assert major == 11, 'Unexpected Mali interface family'
    # _IOW(0x80, 3, {u64 buffer, u32 size, u32 flags}); first query size.
    size = fcntl.ioctl(fd, 0x40108003, bytearray(struct.pack('QII', 0, 0, 0)))
    assert 0 < size < 65536
    buffer = ctypes.create_string_buffer(size)
    request = bytearray(struct.pack('QII', ctypes.addressof(buffer), size, 0))
    assert fcntl.ioctl(fd, 0x40108003, request) == size
    data, pos, props = buffer.raw, 0, {}
    while pos < len(data):
        assert pos + 4 <= len(data)
        key = int.from_bytes(data[pos:pos + 4], 'little')
        pos += 4
        width = 1 << (key & 3)
        assert pos + width <= len(data)
        props[key >> 2] = int.from_bytes(data[pos:pos + width], 'little')
        pos += width
    print(json.dumps({'api': [major, minor], 'product_id': hex(props[1]),
                      'shader_present': hex(props[25]),
                      'core_count': props[25].bit_count(),
                      'max_frequency_khz': props.get(6)}, indent=2))
finally:
    os.close(fd)
