#!/usr/bin/env python3
"""Write an OpenPS2BIOS 0.7 ROM with uncached OSDSYS ExecPS2 arguments."""

import argparse
import hashlib
from pathlib import Path


RELEASE_SHA256 = "062e563879f6bf12504cef125a4edeea2d5528f0e3c2cf49928942124eb6f150"
ARGV_LUI_OFFSET = 0x3A63C


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release_rom", type=Path)
    parser.add_argument("patched_rom", type=Path)
    args = parser.parse_args()

    try:
        rom = bytearray(args.release_rom.read_bytes())
        if hashlib.sha256(rom).hexdigest() != RELEASE_SHA256:
            parser.error("input is not the supported OpenPS2BIOS 0.7 release ROM")

        # OSDSYS start_disc() writes argv via the cached user mapping, but
        # ExecPS2 copies it through KSEG1. Change the shared base for the
        # stores and the syscall argument from 0x00120000 to 0xa0120000.
        instruction = slice(ARGV_LUI_OFFSET, ARGV_LUI_OFFSET + 4)
        if rom[instruction] != bytes.fromhex("1200023c"):
            parser.error("unexpected OSDSYS argv instruction")
        rom[instruction] = bytes.fromhex("12a0023c")

        with args.patched_rom.open("xb") as output:
            output.write(rom)
    except OSError as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
