# -*- coding: utf-8 -*-
"""Build the release ZIP.

    py tools/make_release_zip.py [output-directory]

The archive is named from library.properties and its single top-level folder is
`DualSense32`, not `DualSense32-<version>`. That matters: Arduino refuses a
library whose folder name does not match the library name, so the ZIP GitHub
generates from a tag cannot be used as the release -- unzipping it leaves
`DualSense32-1.6.0` and nothing appears under Examples. The manual tells people
their folder is already correct, and this is what makes that true.

CHANGES.md is kept out. It stays on disk and out of git for the same reason.

Lives in the repository rather than in a scratch directory on purpose: the last
copy of this script was somewhere else and disappeared with the tree it sat in.
tools/ is excluded from the archive it builds.
"""
import io
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {".git", "tools"}
SKIP_FILES = {"CHANGES.md", ".gitignore"}


def version():
    p = os.path.join(ROOT, "library.properties")
    m = re.search(r"^version=(.+)$", io.open(p, encoding="utf-8").read(), re.M)
    if not m:
        sys.exit("no version= in library.properties")
    return m.group(1).strip()


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else ROOT
    v = version()
    out = os.path.join(out_dir, "DualSense32-%s.zip" % v)

    n = 0
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(ROOT):
            dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
            for f in sorted(files):
                rel = os.path.relpath(os.path.join(root, f), ROOT)
                rel = rel.replace(os.sep, "/")
                if rel in SKIP_FILES or f.endswith(".zip"):
                    continue
                z.write(os.path.join(root, f), "DualSense32/" + rel)
                n += 1

    print("%s\n  %d files, %d bytes" % (out, n, os.path.getsize(out)))

    # Say out loud what the archive must and must not contain, so a mistake
    # here is visible before it is published rather than after.
    with zipfile.ZipFile(out) as z:
        names = z.namelist()
        roots = {p.split("/")[0] for p in names}
        print("  single top-level folder: %s" % (
            "yes, DualSense32" if roots == {"DualSense32"} else "NO -> %s" % roots))
        for unwanted in ("DualSense32/CHANGES.md", "DualSense32/.gitignore"):
            print("  %s: %s" % (unwanted,
                                "PRESENT (wrong)" if unwanted in names else "absent"))
        print("  library.properties: %s" % (
            "present" if "DualSense32/library.properties" in names else "MISSING"))
        print("  examples: %d sketches" % len([p for p in names
                                               if p.endswith(".ino")]))


if __name__ == "__main__":
    main()
