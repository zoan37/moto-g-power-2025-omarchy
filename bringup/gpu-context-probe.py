#!/usr/bin/env python3
"""Probe Mali JM context setup directly, without submitting GPU jobs.

Uses the r38p1-compatible UAPI from Mesa's base/include headers. Each probe
opens a fresh fd; closing it releases the context and virtual address zones.
Run explicitly on the native phone, after loading its original GPU modules.
"""
import argparse
import ctypes
import fcntl
import json
import os
import struct


def probe(requested):
    result = {"requested_api": list(requested), "steps": []}
    fd = os.open("/dev/mali0", os.O_RDWR | os.O_CLOEXEC)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                          ctypes.c_int, ctypes.c_int, ctypes.c_long]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    tracking = None
    page_size = os.sysconf("SC_PAGE_SIZE")
    step = "version"
    try:
        version = bytearray(struct.pack("=HH", *requested))
        fcntl.ioctl(fd, 0xc0048000, version, True)
        api = struct.unpack("=HH", version)
        result["negotiated_api"] = list(api)
        result["steps"].append(step)
        if api[0] != 11:
            raise RuntimeError("This diagnostic supports only JM API 11")
        step = "set_flags"
        fcntl.ioctl(fd, 0x40048001, bytearray(struct.pack("=I", 0)))
        result["steps"].append(step)
        step = "map_tracking"
        tracking = libc.mmap(None, page_size, 0, 1, fd, 3 * page_size)
        if tracking == ctypes.c_void_p(-1).value:
            tracking = None
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err))
        result["steps"].append(step)
        step = "get_properties"
        size = fcntl.ioctl(fd, 0x40108003, bytearray(struct.pack("=QII", 0, 0, 0)))
        if not 0 < size < 65536:
            raise RuntimeError("Invalid GPU properties size")
        buffer = ctypes.create_string_buffer(size)
        fcntl.ioctl(fd, 0x40108003,
                    bytearray(struct.pack("=QII", ctypes.addressof(buffer), size, 0)))
        result["property_bytes"] = size
        result["steps"].append(step)
        step = "init_exec_va"
        fcntl.ioctl(fd, 0x40088026, bytearray(struct.pack("=Q", 0x100000)))
        result["steps"].append(step)
        step = "init_jit_va"
        # Reserve virtual address space; this submits no allocation or GPU job.
        fcntl.ioctl(fd, 0x4018800e,
                    bytearray(struct.pack("=QBBB5xQ", 1 << 25, 255, 0, 0, 1 << 25)))
        result["steps"].append(step)
        result["context_setup_passed"] = True
    except (OSError, RuntimeError) as error:
        result.update(context_setup_passed=False, failed_step=step,
                      error=str(error), errno=getattr(error, "errno", None))
    finally:
        if tracking is not None:
            libc.munmap(tracking, page_size)
        os.close(fd)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare-version", action="store_true",
                        help="Also request 11.35 on a fresh fd, versus the driver's zero-version probe")
    args = parser.parse_args()
    versions = [(0, 0), (11, 35)] if args.compare_version else [(0, 0)]
    results = [probe(version) for version in versions]
    print(json.dumps({"gpu_jobs_submitted": False, "probes": results}, indent=2))
    return 0 if all(r["context_setup_passed"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
