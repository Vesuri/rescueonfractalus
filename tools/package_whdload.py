#!/usr/bin/env python3
"""Package the staged ten-file WHDLoad install using native LHa's LH5 encoder."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--lha", default=os.environ.get(
        "LHA", str(Path.home() / ".local/opt/lha/bin/lha")))
    args = parser.parse_args()
    compressor = shutil.which(args.lha)
    if not compressor:
        parser.error("native LHa compressor not found; set LHA=/path/to/lha "
                     "(Homebrew lhasa is extract-only). See docs/whdload-slave.md.")
    compressor = str(Path(compressor).resolve())
    directory = args.directory.resolve()
    # Explicit members match the published RescueOnFractalus.lha. Never include
    # local ROMs, emulator metadata, extra files, or a previous archive.
    files = ["RoF Install.info"] + ["RoF Install/" + name for name in (
        "Install", "Install.info", "Manual", "Manual.info", "ReadMe",
        "ReadMe.info", "RoF", "RoF.inf", "RoF.slave")]
    for name in files:
        if not (directory / name).is_file():
            parser.error(f"missing package member: {name}")
    archive = directory / "RescueOnFractalus.lha"
    with tempfile.TemporaryDirectory(prefix=".pack-", dir=directory) as staging:
        temporary = Path(staging) / archive.name
        # Level 1 headers, as in the published Amiga archive; explicitly LH5.
        # Fresh output prevents a removed member surviving an archive update.
        subprocess.run([compressor, "a1o5q2", str(temporary), *files],
                       cwd=directory, check=True)
        subprocess.run([compressor, "tq2", str(temporary)], check=True)
        temporary.replace(archive)
    print(f"built {archive} ({archive.stat().st_size} bytes, LH5)")


if __name__ == "__main__":
    main()
