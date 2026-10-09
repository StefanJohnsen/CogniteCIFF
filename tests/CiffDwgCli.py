"""Public CIFF CLI DWG export and counted point-only mesh omissions."""
from pathlib import Path
import struct
import subprocess
import sys

cli, folder = Path(sys.argv[1]), Path(sys.argv[2])
folder.mkdir(parents=True, exist_ok=True)


def string(value):
    encoded = value.encode("ascii")
    return struct.pack("<I", len(encoded)) + encoded


def mesh_record(vertices, indices):
    payload = struct.pack("<IIB", len(vertices), len(indices) // 3, 0)
    payload += b"".join(struct.pack("<3d", *point) for point in vertices)
    payload += struct.pack("<" + "I" * len(indices), *indices)
    return struct.pack("<qBBII", -1, 0, 3, 1, len(payload)) + payload


data = struct.pack("<II4QI", 0x46443343, 1, 0, 0, 0, 0, 0)
data += string("")  # The reader's zero-section header variant stores one metadata string.
data += struct.pack("<BI3q", 1, 0, 0, 0, 0) + string("Red triangle and omitted point")
data += bytes((1, 255, 0, 0, 255, 0)) + struct.pack("<IqI", 0, 0, 2)
data += mesh_record(((10, 20, 30), (10, 21, 30), (9, 20, 30)), (0, 1, 2))
data += mesh_record(((100, 200, 300),), ())
data += bytes((0,))
source, target = folder / "surface-and-point.ciff", folder / "surface-and-point.dwg"
source.write_bytes(data)
result = subprocess.run([str(cli), str(source), str(target)], capture_output=True, timeout=120)
output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
assert result.returncode == 0, output
assert "omitted 1 CIFF point geometry record(s)" in output, output
assert target.read_bytes()[:6] == b"AC1015", "Missing native binary DWG header"
before = target.read_bytes()
result = subprocess.run([str(cli), str(source), str(target), "-async"], capture_output=True, timeout=120)
assert result.returncode != 0 and target.read_bytes() == before, "Async rejection changed the target"
speed_target = folder / "speedtest.dwg"
result = subprocess.run([str(cli), str(source), str(speed_target), "-speedtest"], capture_output=True, timeout=120)
assert result.returncode == 0 and not speed_target.exists(), "Speedtest published an output"
print("Public CIFF CLI DWG export, point warning, async and speedtest checks passed")
