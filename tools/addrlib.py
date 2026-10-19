"""Address Library (SE, format 1) reader: map IDs <-> RVAs for SkyrimSE 1.5.97.

Usage:
    python addrlib.py id2rva 37350 ...
    python addrlib.py rva2id 0x7020E0 ...   (also accepts 0x1407020E0)
"""
import struct
import sys
from pathlib import Path

DEFAULT_BIN = Path(
    r"F:\STB\Skyrim\[STB] Mod Organizer\mods\Skyrim Script Extender 64 (SKSE64)"
    r"\SKSE\Plugins\version-1-5-97-0.bin"
)
IMAGE_BASE = 0x140000000


def load(path=DEFAULT_BIN):
    data = Path(path).read_bytes()
    pos = 0

    def take(fmt):
        nonlocal pos
        value = struct.unpack_from(fmt, data, pos)
        pos += struct.calcsize(fmt)
        return value[0]

    if take("<i") != 1:
        raise ValueError("not an SE (format 1) address library")
    pos += 16  # version
    name_len = take("<i")
    pos += name_len  # module name
    ptr_size = take("<i")
    count = take("<i")

    id_to_rva = {}
    prev_id = prev_off = 0
    readers = {2: "<B", 3: "<B", 4: "<H", 5: "<H"}
    for _ in range(count):
        kind = take("<B")
        low, high = kind & 0xF, kind >> 4

        if low == 0:
            cur_id = take("<Q")
        elif low == 1:
            cur_id = prev_id + 1
        elif low in (2, 4):
            cur_id = prev_id + take(readers[low])
        elif low in (3, 5):
            cur_id = prev_id - take(readers[low])
        elif low == 6:
            cur_id = take("<H")
        elif low == 7:
            cur_id = take("<I")
        else:
            raise ValueError(f"bad id encoding {low}")

        base = prev_off // ptr_size if high & 8 else prev_off
        mode = high & 7
        if mode == 0:
            off = take("<Q")
        elif mode == 1:
            off = base + 1
        elif mode in (2, 4):
            off = base + take(readers[mode])
        elif mode in (3, 5):
            off = base - take(readers[mode])
        elif mode == 6:
            off = take("<H")
        else:
            off = take("<I")
        if high & 8:
            off *= ptr_size

        id_to_rva[cur_id] = off
        prev_id, prev_off = cur_id, off
    return id_to_rva


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    table = load()
    if argv[1] == "id2rva":
        for arg in argv[2:]:
            rva = table.get(int(arg, 0))
            print(arg, "->", "missing" if rva is None else f"0x{rva:X} (0x{rva + IMAGE_BASE:X})")
    else:
        reverse = {v: k for k, v in table.items()}
        for arg in argv[2:]:
            rva = int(arg, 16)
            if rva >= IMAGE_BASE:
                rva -= IMAGE_BASE
            print(arg, "->", reverse.get(rva, "missing"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
