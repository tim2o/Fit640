#!/usr/bin/env python3
"""Pack MacBinary files into a StuffIt 1.5.1 archive (.sit).

    mksit.py output.sit file1.bin [file2.bin ...]

The files are stored uncompressed, in the oldest StuffIt format, so every
version of StuffIt and StuffIt Expander can open the result. Type, creator
and both forks are preserved.
"""
import struct
import sys
import time

MAC_EPOCH_OFFSET = 2082844800  # seconds from 1904 to 1970


def crc16(data, crc=0):
    """CRC-16/ARC, as used by StuffIt 1.5.1."""
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def read_macbinary(path):
    d = open(path, "rb").read()
    name_len = d[1]
    name = d[2:2 + name_len]
    ftype, creator = d[65:69], d[69:73]
    data_len, rsrc_len = struct.unpack(">II", d[83:91])
    data = d[128:128 + data_len]
    rsrc_start = 128 + ((data_len + 127) // 128) * 128
    rsrc = d[rsrc_start:rsrc_start + rsrc_len]
    if len(data) != data_len or len(rsrc) != rsrc_len:
        sys.exit("%s: not a complete MacBinary file" % path)
    return name, ftype, creator, data, rsrc


def entry(path, now):
    name, ftype, creator, data, rsrc = read_macbinary(path)
    hdr = struct.pack(
        ">BBB63s4s4sHIIIIIIHH6x",
        0, 0,                       # both forks stored, no compression
        len(name), name,
        ftype, creator,
        0,                          # Finder flags
        now, now,
        len(rsrc), len(data),       # original lengths
        len(rsrc), len(data),       # stored lengths
        crc16(rsrc), crc16(data))
    assert len(hdr) == 110
    hdr += struct.pack(">H", crc16(hdr))
    return hdr + rsrc + data


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    now = int(time.time()) + MAC_EPOCH_OFFSET
    body = b"".join(entry(p, now) for p in sys.argv[2:])
    total = 22 + len(body)
    header = struct.pack(">4sHI4sB7x", b"SIT!", len(sys.argv) - 2, total,
                         b"rLau", 1)
    assert len(header) == 22
    with open(sys.argv[1], "wb") as f:
        f.write(header + body)


if __name__ == "__main__":
    main()
