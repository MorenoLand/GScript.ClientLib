import re

from gclib_common import GCLib, ROOT


HEADER = ROOT / "include" / "gclib.h"
EXPORTS = sorted(set(re.findall(r"GCLIB_API\s+[\w\s\*]+?(gc_\w+)\s*\(", HEADER.read_text())))


if __name__ == "__main__":
    lib = GCLib().lib
    for name in EXPORTS:
        getattr(lib, name)
    print("exports ok")
