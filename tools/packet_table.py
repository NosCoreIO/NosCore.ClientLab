"""Extract the receive-packet handler table (header -> handler address) from the decompiled registration function."""
import argparse
import re

import pefile

from xref import load

REGISTER = "FUN_004db314"
CALL = re.compile(REGISTER + r"\(\w+,\s*(\"[^\"]*\"|&?\w+),[^,]+,\s*&?(\w+),")


def read_cstring(pe, va):
    data = pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 64)
    return data.split(b"\0", 1)[0].decode("latin-1")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("exe")
    p.add_argument("--dir", default=r"C:\tools\re\work\decomp")
    p.add_argument("--registrar", default="FUN_0055a628")
    a = p.parse_args()

    pe = pefile.PE(a.exe, fast_load=True)
    funcs = load(a.dir)
    rows = []
    for header, handler in CALL.findall(funcs[a.registrar]["text"]):
        if header.startswith('"'):
            header = header.strip('"')
        else:
            header = read_cstring(pe, int(header.lstrip("&").split("_")[-1], 16))
        addr = handler.split("_")[-1].lower()
        rows.append((header, addr, handler))
    for header, addr, handler in sorted(rows):
        print(f"{header}\t{addr}\t{handler}")


if __name__ == "__main__":
    main()
