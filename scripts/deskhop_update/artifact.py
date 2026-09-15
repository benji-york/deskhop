"""Freeze and revalidate self-contained DeskHop release candidates.

This module never opens a USB device. A candidate is a write-once directory,
not a pointer to a mutable build output. Validation evidence is supplied by the
prepare command that actually ran the checks; old test-result JSON is not proof.
"""
from __future__ import annotations

import hashlib
import io
import json
from pathlib import Path
import re
import stat
import struct
import subprocess
import tarfile
import tempfile
import time
import zlib


SLOT_BYTES = 262144
FIRMWARE_START = 0x10000000
METADATA_OFFSET = 258048
DISK_OFFSET = 192512
SETTINGS_START = 0x101FF000
SETTINGS_BYTES = 4096
UF2_BLOCKS = SLOT_BYTES // 256
SCHEMA = 1
SUPPORTED_UART_FRAME_VERSION = 1
COMPATIBILITY_HEADERS = {"uart_frame_version": ("src/include/packet.h", "UART_FRAME_VERSION"),
                         "config_version": ("src/include/config_migration.h", "CURRENT_CONFIG_VERSION")}

# Include vendored SDK/library content, untracked additions, and the validation
# tooling. Documentation-only edits do not invalidate a prepared binary.
SOURCE_FILES = ("CMakeLists.txt", "Makefile")
SOURCE_DIRS = ("src", "disk", "misc", "webconfig", "pico-sdk", "Pico-PIO-USB",
               "tests", "scripts", "cmake")
SKIP_DIRS = {".git", "__pycache__", ".pytest_cache", "build"}


class ArtifactError(ValueError):
    """A candidate, image, or its provenance cannot safely be accepted."""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _source_paths(repo: Path) -> list[Path]:
    paths: list[Path] = []

    def visit(path: Path) -> None:
        if path.is_symlink():
            raise ArtifactError(f"Source symlink is not supported: {path}")
        if path.is_dir():
            for child in sorted(path.iterdir()):
                if child.name not in SKIP_DIRS:
                    visit(child)
        elif path.is_file():
            if path.suffix not in {".pyc", ".pyo"}:
                paths.append(path)
        elif path.exists():
            raise ArtifactError(f"Source is not a regular file: {path}")

    for name in (*SOURCE_FILES, *SOURCE_DIRS):
        visit(repo / name)
    if repo / "CMakeLists.txt" not in paths:
        raise ArtifactError("Missing CMakeLists.txt")
    return sorted(paths)


def fingerprint_sources(repo: Path) -> dict[str, str]:
    """Hash current build/test inputs, including untracked and vendored files."""
    repo = Path(repo).resolve()
    return {str(path.relative_to(repo)): sha256(path.read_bytes())
            for path in _source_paths(repo)}


def _version(cmake: bytes) -> tuple[str, int]:
    text = re.sub(r"#[^\n]*", "", cmake.decode("utf-8"))
    parts = []
    for component in ("MAJOR", "MINOR"):
        matches = re.findall(rf"\bset\s*\(\s*VERSION_{component}\s+(\d+)\s*\)",
                             text, re.IGNORECASE)
        if len(matches) != 1:
            raise ArtifactError(f"Expected one literal VERSION_{component} in CMakeLists.txt")
        parts.append(int(matches[0]))
    encoded = parts[0] * 1000 + parts[1] + 100
    if parts[1] >= 1000 or not 100 <= encoded <= 65535:
        raise ArtifactError("Firmware version is out of range (minor must be below 1000)")
    return f"{parts[0]}.{parts[1]}", encoded


def _compatibility(source_bytes: dict[str, bytes]) -> dict[str, int]:
    """Read the frozen protocol, not the current checkout or a release number."""
    result = {}
    for key, (path, constant) in COMPATIBILITY_HEADERS.items():
        if path not in source_bytes:
            raise ArtifactError(f"Missing firmware compatibility header: {path}")
        text = source_bytes[path].decode("utf-8")
        text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
        # An expression, duplicate conditional definition, or renamed constant
        # requires deliberate updater review, not optimistic guessing.
        definitions = re.findall(rf"^\s*#\s*define\s+{constant}\b([^\n]*)", text, re.MULTILINE)
        if len(definitions) != 1 or not re.fullmatch(r"\s*[0-9]+[uU]?\s*", definitions[0]):
            raise ArtifactError(f"Expected one literal {constant} in {path}")
        value = int(definitions[0].strip().rstrip("uU"))
        if not 1 <= value <= 0xFFFFFFFF:
            raise ArtifactError(f"Invalid firmware compatibility value: {constant}")
        result[key] = value
    if result["uart_frame_version"] != SUPPORTED_UART_FRAME_VERSION:
        raise ArtifactError("Unsupported UART frame version; automatic propagation requires a reviewed migration")
    return result


def _image_metadata(image: bytes) -> tuple[int, str, str]:
    if len(image) != SLOT_BYTES:
        raise ArtifactError(f"BIN must contain exactly {SLOT_BYTES} firmware bytes")
    magic, version, reserved, crc = struct.unpack_from("<IHHI", image, METADATA_OFFSET)
    if magic != 0xF00D or reserved != 0 or version < 100:
        raise ArtifactError("Invalid firmware metadata magic, version, or reserved field")
    if zlib.crc32(image[:METADATA_OFFSET]) != crc:
        raise ArtifactError("BIN metadata CRC does not match firmware content")
    return version, f"{crc:08x}", f"{zlib.crc32(image):08x}"


def validate_uf2(wire: bytes, image: bytes) -> None:
    """Require one RP2040 UF2 payload for every byte of the firmware slot only."""
    if len(wire) != UF2_BLOCKS * 512:
        raise ArtifactError("UF2 has a missing, extra, or partial block")
    seen = set()
    restored = bytearray(SLOT_BYTES)
    for offset in range(0, len(wire), 512):
        block = wire[offset:offset + 512]
        m0, m1, flags, address, size, index, total, family = struct.unpack_from("<8I", block)
        if (m0, m1, flags, size, total, family) != (
                0x0A324655, 0x9E5D5157, 0x2000, 256, UF2_BLOCKS, 0xE48BFF56):
            raise ArtifactError("Unsupported UF2 header, flags, family, or payload geometry")
        if struct.unpack_from("<I", block, 508)[0] != 0x0AB16F30:
            raise ArtifactError("Invalid UF2 end magic")
        if index >= UF2_BLOCKS or index in seen:
            raise ArtifactError("UF2 has an out-of-range or duplicate block number")
        if address != FIRMWARE_START + index * 256 or address + size > FIRMWARE_START + SLOT_BYTES:
            raise ArtifactError("UF2 writes outside the firmware slot or has a misaddressed block")
        seen.add(index)
        restored[index * 256:(index + 1) * 256] = block[32:288]
    if bytes(restored) != image:
        raise ArtifactError("UF2 payload does not exactly reconstruct the BIN")


def _git_identity(repo: Path) -> dict:
    def git(*args: str) -> str:
        try:
            return subprocess.check_output(["git", *args], cwd=repo,
                                           stderr=subprocess.PIPE).decode("utf-8").strip()
        except (OSError, subprocess.CalledProcessError) as exc:
            raise ArtifactError("Cannot record repository Git identity") from exc
    return {"head": git("rev-parse", "HEAD"),
            "branch": git("branch", "--show-current"),
            "dirty": bool(git("status", "--porcelain", "--untracked-files=all")),
            "attribution": "Working-tree source snapshot; HEAD alone does not identify dirty inputs."}


def _proof(validation: dict | None, source_before: dict | None, sources: dict,
           images: dict[str, bytes]) -> dict:
    if not isinstance(source_before, dict) or not source_before or source_before != sources:
        raise ArtifactError("Source inputs changed during prepare, or pre-validation fingerprint is missing")
    if not isinstance(validation, dict) or validation.get("source_before") != sources:
        raise ArtifactError("Validation evidence is not bound to these source inputs")
    commands = validation.get("commands")
    if not isinstance(commands, list) or not commands:
        raise ArtifactError("No fresh validation/build command evidence was supplied")
    for result in commands:
        if (not isinstance(result, dict) or result.get("returncode") != 0
                or not isinstance(result.get("command"), list) or not result["command"]
                or not all(isinstance(arg, str) for arg in result["command"])):
            raise ArtifactError("Validation/build evidence contains a failed or invalid command")
    hashes = {ext: sha256(images[ext]) for ext in ("bin", "uf2")}
    if validation.get("artifacts") != hashes:
        raise ArtifactError("Validation evidence is not bound to these built artifacts")
    # Serialize now, rejecting non-JSON evidence before creating a candidate.
    try:
        return json.loads(json.dumps(validation, allow_nan=False))
    except (TypeError, ValueError) as exc:
        raise ArtifactError("Validation evidence is not JSON-serializable") from exc


def _write_once(path: Path, data: bytes) -> None:
    with path.open("xb") as output:
        output.write(data)
    path.chmod(stat.S_IRUSR | stat.S_IRGRP | stat.S_IROTH)


def freeze(repo: Path, build_dir: Path, output_root: Path, *, validation: dict | None = None,
           source_before: dict | None = None) -> Path:
    """Freeze a freshly validated build. Return its immutable manifest path.

    ``validation`` must include ``source_before``, successful ``commands`` (argv
    and returncode), and ``artifacts`` (bin/uf2 SHA256). The prepare caller must
    collect these from this run, not import a previous successful test report.
    """
    repo, build_dir, output_root = map(lambda path: Path(path).resolve(),
                                      (repo, build_dir, output_root))
    source_paths = _source_paths(repo)
    source_bytes = {str(path.relative_to(repo)): path.read_bytes() for path in source_paths}
    sources = {name: sha256(data) for name, data in source_bytes.items()}
    images = {ext: (build_dir / f"deskhop.{ext}").read_bytes() for ext in ("bin", "uf2")}
    proof = _proof(validation, source_before, sources, images)
    # A fresh successful CMake invocation may legitimately leave unchanged
    # outputs older than tests/updater scripts. Source-before/after and output
    # hashes bind this run; filesystem timestamps cannot establish that proof.
    build, expected_version = _version(source_bytes["CMakeLists.txt"])
    compatibility = _compatibility(source_bytes)
    version, boot_crc, slot_crc = _image_metadata(images["bin"])
    if version != expected_version:
        raise ArtifactError("BIN version does not match current CMake version")
    disk = source_bytes.get("disk/disk.img")
    if disk is None or images["bin"][DISK_OFFSET:METADATA_OFFSET] != disk:
        raise ArtifactError("BIN embedded configuration disk does not match disk/disk.img")
    validate_uf2(images["uf2"], images["bin"])
    elf = build_dir / "deskhop.elf"
    if elf.exists():
        images["elf"] = elf.read_bytes()
    identity = _git_identity(repo)
    # Recheck the complete live inventory after reads, including new/deleted files.
    if fingerprint_sources(repo) != sources:
        raise ArtifactError("Source inputs changed while freezing the candidate")

    output_root.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=f"deskhop-v{build}-", dir=output_root))
    artifacts = {}
    for ext, data in images.items():
        name = f"deskhop.{ext}"
        _write_once(directory / name, data)
        artifacts[ext] = {"path": name, "sha256": sha256(data), "bytes": len(data)}
    archive_bytes = io.BytesIO()
    with tarfile.open(fileobj=archive_bytes, mode="w:gz") as archive:
        for name, data in source_bytes.items():
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(data), 0o644, 0
            archive.addfile(info, io.BytesIO(data))
    snapshot = archive_bytes.getvalue()
    _write_once(directory / "source.tar.gz", snapshot)
    proof_bytes = (json.dumps(proof, indent=2, sort_keys=True) + "\n").encode()
    _write_once(directory / "validation.json", proof_bytes)
    manifest = {
        "schema": SCHEMA, "created_ns": time.time_ns(), "build": build,
        "encoded_version": version, "metadata_crc32": boot_crc, "full_slot_crc32": slot_crc,
        "firmware_compatibility": compatibility,
        "firmware_start": FIRMWARE_START, "firmware_bytes": SLOT_BYTES,
        "saved_settings_start": SETTINGS_START, "saved_settings_bytes": SETTINGS_BYTES,
        "config_included": False, "git": identity, "source_files": sources,
        "source_snapshot": {"path": "source.tar.gz", "sha256": sha256(snapshot)},
        "validation": {"path": "validation.json", "sha256": sha256(proof_bytes)},
        "artifacts": artifacts,
    }
    path = directory / "manifest.json"
    _write_once(path, (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode())
    load_candidate(path)
    return path


def _entry(directory: Path, entry: dict) -> tuple[Path, bytes]:
    if not isinstance(entry, dict):
        raise ArtifactError("Missing candidate file record")
    name = entry.get("path")
    if (not isinstance(name, str) or not name or Path(name).name != name
            or name in {".", ".."} or "\\" in name):
        raise ArtifactError("Candidate paths must be plain relative filenames")
    path = directory / name
    if path.is_symlink() or not path.is_file():
        raise ArtifactError(f"Candidate file is missing or a symlink: {name}")
    data = path.read_bytes()
    if entry.get("sha256") != sha256(data):
        raise ArtifactError(f"Candidate file hash mismatch: {name}")
    if "bytes" in entry and entry["bytes"] != len(data):
        raise ArtifactError(f"Candidate file size mismatch: {name}")
    return path, data


def load_candidate(manifest: Path) -> dict:
    """Validate frozen content, independent of the current checkout/build tree."""
    manifest = Path(manifest).absolute()
    if manifest.is_symlink():
        raise ArtifactError("Manifest must not be a symlink")
    try:
        document = json.loads(manifest.read_bytes())
        if not isinstance(document, dict) or document.get("schema") != SCHEMA:
            raise ArtifactError("Unsupported candidate manifest schema")
        for key, expected in (("firmware_start", FIRMWARE_START), ("firmware_bytes", SLOT_BYTES),
                              ("saved_settings_start", SETTINGS_START),
                              ("saved_settings_bytes", SETTINGS_BYTES), ("config_included", False)):
            if document.get(key) != expected:
                raise ArtifactError(f"Candidate has invalid {key}")
        directory = manifest.parent
        artifacts = document["artifacts"]
        bin_path, image = _entry(directory, artifacts["bin"])
        uf2_path, wire = _entry(directory, artifacts["uf2"])
        if "elf" in artifacts:
            _entry(directory, artifacts["elf"])
        version, boot_crc, slot_crc = _image_metadata(image)
        if (version != document.get("encoded_version") or boot_crc != document.get("metadata_crc32")
                or slot_crc != document.get("full_slot_crc32")):
            raise ArtifactError("Candidate metadata does not match its manifest")
        validate_uf2(wire, image)
        _, snapshot = _entry(directory, document["source_snapshot"])
        source_files, source_bytes = {}, {}
        with tarfile.open(fileobj=io.BytesIO(snapshot), mode="r:gz") as archive:
            for member in archive:
                name = member.name
                if (not member.isfile() or Path(name).is_absolute() or ".." in Path(name).parts
                        or "\\" in name or name in source_files):
                    raise ArtifactError("Source snapshot has an unsafe or duplicate member")
                stream = archive.extractfile(member)
                assert stream is not None
                data = stream.read()
                source_files[name] = sha256(data)
                if name in {"CMakeLists.txt", "disk/disk.img", *(path for path, _ in COMPATIBILITY_HEADERS.values())}:
                    source_bytes[name] = data
        if not source_files or source_files != document["source_files"]:
            raise ArtifactError("Source snapshot does not match its file fingerprints")
        build, expected_version = _version(source_bytes["CMakeLists.txt"])
        compatibility = _compatibility(source_bytes)
        if document.get("firmware_compatibility") != compatibility:
            raise ArtifactError("Candidate firmware compatibility does not match frozen source configuration")
        if build != document.get("build") or version != expected_version:
            raise ArtifactError("Candidate version does not match frozen source configuration")
        if source_bytes["disk/disk.img"] != image[DISK_OFFSET:METADATA_OFFSET]:
            raise ArtifactError("Candidate disk does not match frozen source configuration")
        _, proof_data = _entry(directory, document["validation"])
        proof = json.loads(proof_data)
        _proof(proof, source_files, source_files, {"bin": image, "uf2": wire})
    except ArtifactError:
        raise
    except (OSError, ValueError, KeyError, TypeError, tarfile.TarError) as exc:
        raise ArtifactError(f"Invalid or incomplete candidate: {exc}") from exc
    return {**document, "validation_record": proof, "manifest_path": manifest.resolve(), "build": build,
            "firmware_compatibility": compatibility,
            "encoded_version": version, "slot_crc": slot_crc, "boot_crc": boot_crc,
            "bin_path": bin_path.resolve(), "uf2_path": uf2_path.resolve(),
            "bin_sha256": sha256(image), "uf2_sha256": sha256(wire), "image": image}
