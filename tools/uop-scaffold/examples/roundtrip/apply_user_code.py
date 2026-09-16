#!/usr/bin/env python3
"""Apply user_code overlays into a generated uop-scaffold tree.

Each file in <user-code-dir> is named ``<target>.<region>.c`` where target
is ``uop`` (the generated ``<uop>.c``) or ``harness`` (``<uop>_harness.c``),
and its contents become the body of that USER CODE region — exactly what
a developer would type into the region by hand. This is demo glue; the
generator itself only ever reads regions out of the generated files.

Usage: apply_user_code.py <gen-dir> <uop-name> <user-code-dir>
"""
import re
import sys
from pathlib import Path


def main():
    gen = Path(sys.argv[1])
    uop = sys.argv[2]
    uc_dir = Path(sys.argv[3])
    targets = {"uop": gen / f"{uop}.c", "harness": gen / f"{uop}_harness.c"}
    applied = []
    for overlay in sorted(uc_dir.glob("*.c")):
        target, region = overlay.stem.split(".", 1)
        dest = targets[target]
        text = dest.read_text()
        pat = re.compile(
            r"(^[ \t]*/\* USER CODE BEGIN: %s \*/\r?\n)(.*?)"
            r"(^[ \t]*/\* USER CODE END: %s \*/)"
            % (re.escape(region), re.escape(region)),
            re.DOTALL | re.MULTILINE)
        m = pat.search(text)
        if not m:
            sys.exit(f"error: region '{region}' not found in {dest}")
        body = overlay.read_text()
        if not body.endswith("\n"):
            body += "\n"
        dest.write_text(pat.sub(lambda m: m.group(1) + body + m.group(3),
                                text, count=1))
        applied.append(f"{target}.{region}")
    print("applied user code:", ", ".join(applied))


if __name__ == "__main__":
    main()
