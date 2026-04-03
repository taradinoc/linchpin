from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--zip-name", required=True)
    parser.add_argument("--bin-input", action="append", default=[])
    parser.add_argument("--atari-binary", required=True)
    parser.add_argument("--amiga-binary", required=True)
    parser.add_argument("--c128-boot", required=True)
    parser.add_argument("--c128-program", required=True)
    parser.add_argument("--sample-dir", required=True)
    parser.add_argument("--license-file", required=True)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def ensure_file(path: Path) -> Path:
    if not path.is_file():
        raise FileNotFoundError(f"Required file not found: {path}")
    return path


def merge_tree(source: Path, destination: Path) -> None:
    for child in source.rglob("*"):
        relative = child.relative_to(source)
        target = destination / relative
        if child.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue

        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            if sha256(child) != sha256(target):
                raise RuntimeError(f"Conflicting file contents for {target.name}")
            continue
        shutil.copy2(child, target)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ensure_file(source), destination)


def create_zip(source_dir: Path, zip_path: Path) -> None:
    with ZipFile(zip_path, "w", compression=ZIP_DEFLATED) as archive:
        for path in sorted(source_dir.rglob("*")):
            if path.is_dir():
                continue
            archive.write(path, path.relative_to(source_dir))


def main() -> None:
    args = parse_args()

    output_dir = Path(args.output_dir)
    staging_dir = output_dir / "staging"
    zip_path = output_dir / args.zip_name
    bin_dir = staging_dir / "bin"
    atari_dir = staging_dir / "atari"
    amiga_dir = staging_dir / "amiga"
    c128_dir = staging_dir / "c128"
    sample_dir = staging_dir / "sample"

    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if zip_path.exists():
        zip_path.unlink()

    for bin_input in args.bin_input:
        source = Path(bin_input)
        if not source.is_dir():
            raise FileNotFoundError(f"Required publish directory not found: {source}")
        merge_tree(source, bin_dir)

    copy_file(Path(args.atari_binary), atari_dir / "LINCHPIN.TTP")
    copy_file(Path(args.amiga_binary), amiga_dir / "LINCHPIN")
    copy_file(Path(args.c128_boot), c128_dir / "boot.prg")
    copy_file(Path(args.c128_program), c128_dir / "linchpin.prg")
    copy_file(Path(args.license_file), staging_dir / "LICENSE")

    examples = sorted(Path(args.sample_dir).glob("*.cas"))
    if not examples:
        raise FileNotFoundError(f"No .cas files found in {args.sample_dir}")
    for example in examples:
        copy_file(example, sample_dir / example.name)

    create_zip(staging_dir, zip_path)


if __name__ == "__main__":
    main()
