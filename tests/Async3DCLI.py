"""CIFF Async .3d CLI regressions; only the Python standard library is needed."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(executable, *arguments, succeeds=True, contains=None):
    result = subprocess.run([str(executable), *(str(arg) for arg in arguments)],
                            capture_output=True, text=True, timeout=90)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != succeeds:
        raise AssertionError(f"Unexpected exit {result.returncode}: {arguments}\n{output}")
    if contains is not None and contains not in output:
        raise AssertionError(f"Missing diagnostic {contains!r}: {arguments}\n{output}")
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--converter", required=True, type=Path)
    parser.add_argument("--fixture-writer", required=True, type=Path)
    arguments = parser.parse_args()
    converter = arguments.converter.resolve()
    with tempfile.TemporaryDirectory(prefix="ciff-async-3d-cli-") as temporary:
        root = Path(temporary)
        fixtures = root / "fixtures"
        run(arguments.fixture_writer.resolve(), "--fixtures", fixtures)
        originals = {path: path.read_bytes() for path in fixtures.glob("*.ciff")}
        run(converter, "--help", contains="ciff, obj and 3d target formats")
        source = fixtures / "scene.ciff"
        serial, parallel = root / "serial.3d", root / "parallel.3d"
        run(converter, source, serial)
        run(converter, source, parallel, "-async", contains="(async processing)")
        expected = serial.read_bytes()
        assert expected[:4] == b"C3DF", "Invalid Falcon3D signature"
        assert parallel.read_bytes() == expected, "Async differs from serial output"
        run(converter, source, "--async", "-format=.3d")
        assert source.with_suffix(".3d").read_bytes() == expected
        output = root / "output-directory"
        output.mkdir()
        run(converter, source, output, "--async", "--format", ".3D")
        assert (output / "scene.3d").read_bytes() == expected
        upper = root / "explicit.3D"
        run(converter, source, upper, "-async", "-format", "nwd")
        assert upper.read_bytes() == expected, "Explicit target must override -format"

        batch, batch_output = root / "batch", root / "batch-output"
        (batch / "subfolder").mkdir(parents=True)
        batch_output.mkdir()
        shutil.copyfile(source, batch / "scene.ciff")
        shutil.copyfile(fixtures / "tiny.ciff", batch / "subfolder" / "tiny.CIFF")
        run(converter, batch, batch_output, "-async", "--format=3d", contains="TOTAL FILES : 2")
        assert (batch_output / "scene.3d").read_bytes() == expected
        run(converter, fixtures / "tiny.ciff", root / "tiny-serial.3d")
        assert (batch_output / "tiny.3d").read_bytes() == (root / "tiny-serial.3d").read_bytes()
        run(converter, batch, batch_output, "--async", "-format", "3d", contains="TOTAL FILES : 2")
        run(converter, batch, "-async", "--format=3d", contains="TOTAL FILES : 2")
        assert (batch / "scene.3d").read_bytes() == expected

        # CIFF batch naming is flat: duplicate basenames must fail before conversion.
        shutil.copyfile(source, batch / "subfolder" / "scene.ciff")
        before = {path: path.read_bytes() for path in batch_output.glob("*.3d")}
        run(converter, batch, batch_output, "-async", "--format=3d", succeeds=False,
            contains="Multiple CIFF source files resolve to the same batch target")
        assert all(path.read_bytes() == content for path, content in before.items())

        sentinel = b"previous successful Falcon3D output"
        for name in ("empty", "invalid"):
            target = root / f"failed-{name}.3d"
            target.write_bytes(sentinel)
            run(converter, fixtures / f"{name}.ciff", target, "-async", succeeds=False)
            assert target.read_bytes() == sentinel, "Failed conversion replaced existing output"
        malformed = root / "malformed.ciff"
        malformed.write_bytes(b"not a CIFF file")
        target = root / "malformed.3d"
        target.write_bytes(sentinel)
        run(converter, malformed, target, "-async", succeeds=False)
        assert target.read_bytes() == sentinel
        run(converter, source, target, "-async", "-mesh", succeeds=False,
            contains="Mesh is only supported for ciff target format")
        assert target.read_bytes() == sentinel
        run(converter, source, root / "unsupported.nwd", "-async", succeeds=False,
            contains="Async is only supported for ciff, obj and 3d target formats")
        run(converter, source, source, "-async", succeeds=False, contains="Source and target files are the same")
        run(converter, batch, root / "single.3d", "-async", succeeds=False,
            contains="Converting a source directory to one target file is not supported")
        for path, content in originals.items():
            assert path.read_bytes() == content, f"CLI rewrote source CIFF: {path}"
        assert not any(path.name.startswith(".conversion-") for path in root.rglob("*")), \
            "Failed conversion left an output workspace"
    print("CIFF Async .3d CLI: binary equality, formats, batch and failure preservation: OK")


if __name__ == "__main__":
    main()
