"""Create a local, reviewable OTA release package. Does not upload/publish anything."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import re
from urllib.parse import quote

PRODUCT = "linear-acoustic-lav60ii"
BOARD = "esp32c6"
DEFAULT_SLOT = 0x140000

def version_key(value):
    m = re.fullmatch(r"([0-9]{2})\.([0-9]{2})\.([0-9]{2})\+([1-9][0-9]{0,3})", value)
    if not m:
        raise ValueError("Version must be YY.MM.DD+revision (revision 1..9999)")
    yy, mm, dd, revision = map(int, m.groups())
    datetime.date(2000 + yy, mm, dd)
    return yy, mm, dd, revision

def inspect_image(data, slot_size=DEFAULT_SLOT):
    if len(data) < 64 or data[0] != 0xE9:
        raise ValueError("Not an ESP application image")
    if int.from_bytes(data[12:14], "little") != 13:
        raise ValueError("Image is not for ESP32-C6")
    # App descriptor starts in the first segment, at offset 32. This also
    # rejects bootloaders and merged images, which must not be used for OTA.
    if int.from_bytes(data[32:36], "little") != 0xABCD5432:
        raise ValueError("Expected application .bin, not bootloader/merged.bin")
    if len(data) > slot_size:
        raise ValueError(f"Image ({len(data)} bytes) exceeds OTA slot ({slot_size} bytes)")
    versions = set(m.decode("ascii") for m in re.findall(rb"LA_FW_VERSION:([^\x00]{1,24})\x00", data))
    if len(versions) != 1:
        raise ValueError("Missing or ambiguous embedded LA_FW_VERSION record")
    version = versions.pop()
    version_key(version)
    return version

def prepare(firmware, output, slot_size=DEFAULT_SLOT):
    data = Path(firmware).read_bytes()
    version = inspect_image(data, slot_size)
    output = Path(output)
    if output.exists():
        raise ValueError("Output already exists. Choose a new directory; releases must not be overwritten.")
    relative = Path("releases") / version / "LAV60II.bin"
    manifest = {
        "schema": 1,
        "product": PRODUCT,
        "board": BOARD,
        "version": version,
        "url": "https://raw.githubusercontent.com/davidjetw/LinearAcoustic/main/OTA/ESP32C6/"
               + "/".join(quote(part, safe="") for part in relative.parts),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    output.mkdir(parents=True)
    binary = output / relative
    binary.parent.mkdir(parents=True)
    binary.write_bytes(data)
    # Deliberately not named version.json: publishing is a separate step after
    # USB/hardware testing, not an automatic side effect of packaging.
    (output / "version.candidate.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return manifest

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--slot-size", type=lambda s: int(s, 0), default=DEFAULT_SLOT)
    args = parser.parse_args()
    try:
        manifest = prepare(args.firmware, args.output, args.slot_size)
    except (ValueError, OSError) as error:
        parser.exit(1, f"Release not prepared: {error}\n")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    print("Prepared locally only. Test on hardware before publishing version.json.")

if __name__ == "__main__":
    main()
