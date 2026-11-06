"""Build the KCDS client installer from a packaged client zip, with Inno Setup 6.

    python Dev/tools/release/installer.py --zip <dist>/KCDS-Client-<version>.zip --out <dist>

The zip is unpacked into a scratch folder and Dev/tools/release/client-setup.iss makes
KCDS-Setup-<version>.exe next to it. ISCC.exe is looked for in KCDS_ISCC, then in the default Inno
Setup 6 folders. package.py --installer calls this after it made the client zip.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "client-setup.iss"
ISCC_PLACES = [
    Path(r"C:\Program Files (x86)\Inno Setup 6\ISCC.exe"),
    Path(r"C:\Program Files\Inno Setup 6\ISCC.exe"),
]
ZIP_NAME = re.compile(r"^KCDS-Client-(\d+\.\d+\.\d+(?:\.\d+)?)\.zip$")


def fail(message):
    print("installer: " + message, file=sys.stderr)
    sys.exit(1)


def iscc():
    """The Inno Setup compiler, or None."""
    given = os.environ.get("KCDS_ISCC")
    if given:
        return Path(given) if Path(given).is_file() else None
    for place in ISCC_PLACES:
        if place.is_file():
            return place
    return None


def build_installer(zip_path, out):
    """Makes KCDS-Setup-<version>.exe in out from the client zip; returns its path."""
    match = ZIP_NAME.match(zip_path.name)
    if not match:
        fail(f"{zip_path.name} is not a KCDS-Client-<version>.zip")
    version = match.group(1)
    compiler = iscc()
    if compiler is None:
        fail("Inno Setup 6 (ISCC.exe) is not installed; set KCDS_ISCC to its path")
    with tempfile.TemporaryDirectory(prefix="kcds-setup-") as scratch:
        with zipfile.ZipFile(zip_path) as archive:
            archive.extractall(scratch)
        source = Path(scratch) / f"KCDS-Client-{version}"
        if not (source / "kcds-launcher.exe").is_file():
            fail(f"{zip_path.name} has no KCDS-Client-{version}/kcds-launcher.exe")
        out.mkdir(parents=True, exist_ok=True)
        result = subprocess.run(
            [
                str(compiler),
                "/Q",
                f"/DAppVersion={version}",
                f"/DSourceDir={source}",
                f"/DOutputDir={out.resolve()}",
                str(SCRIPT),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(result.stdout + result.stderr, file=sys.stderr)
            fail(f"ISCC failed with exit code {result.returncode}")
    setup = out / f"KCDS-Setup-{version}.exe"
    if not setup.is_file():
        fail(f"ISCC made no {setup.name}")
    return setup


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--zip", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    options = parser.parse_args()
    setup = build_installer(options.zip, options.out)
    print(f"{setup}  {setup.stat().st_size} bytes")


if __name__ == "__main__":
    main()
