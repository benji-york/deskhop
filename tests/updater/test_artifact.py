"""Hardware-free release candidate integrity and provenance regressions."""
from __future__ import annotations

import copy
import io
import json
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from deskhop_update import artifact


def make_image(version=237) -> bytes:
    image = bytearray(b"\xA5" * artifact.SLOT_BYTES)
    image[artifact.DISK_OFFSET:artifact.METADATA_OFFSET] = b"\x33" * 65536
    struct.pack_into("<IHHI", image, artifact.METADATA_OFFSET, 0xF00D, version, 0,
                     zlib.crc32(image[:artifact.METADATA_OFFSET]))
    return bytes(image)


def make_uf2(image: bytes) -> bytes:
    blocks = []
    for index in range(artifact.UF2_BLOCKS):
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                         artifact.FIRMWARE_START + index * 256, 256, index,
                         artifact.UF2_BLOCKS, 0xE48BFF56)
        block[32:288] = image[index * 256:(index + 1) * 256]
        struct.pack_into("<I", block, 508, 0x0AB16F30)
        blocks.append(block)
    return b"".join(blocks)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="deskhop-artifact-test-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.repo, self.build, self.output = root / "repo", root / "build", root / "candidates"
        self.repo.mkdir()
        self.build.mkdir()
        (self.repo / "CMakeLists.txt").write_text("set(VERSION_MAJOR 0)\nset(VERSION_MINOR 137)\n")
        for name, data in {"src/main.c": b"int main(void) {}\n", "disk/disk.S": b"assembly",
                           "src/include/packet.h": b"#define UART_FRAME_VERSION 1\n",
                           "src/include/config_migration.h": b"#define CURRENT_CONFIG_VERSION 10\n",
                           "disk/disk.img": b"\x33" * 65536,
                           "pico-sdk/src/important.c": b"sdk", "Pico-PIO-USB/src/usb.c": b"pio",
                           "tests/new_test.py": b"test", "scripts/new_tool.py": b"tool",
                           "misc/memory_map.ld": b"linker"}.items():
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.image = make_image()
        self.wire = make_uf2(self.image)
        (self.build / "deskhop.bin").write_bytes(self.image)
        (self.build / "deskhop.uf2").write_bytes(self.wire)
        self.git_mock = patch.object(artifact, "_git_identity", return_value={
            "head": "1" * 40, "branch": "codex/test", "dirty": True,
            "attribution": "working-tree snapshot"})
        self.git_mock.start()
        self.addCleanup(self.git_mock.stop)

    def proof(self):
        sources = artifact.fingerprint_sources(self.repo)
        return sources, {"source_before": sources,
                         "commands": [{"command": ["python3", "tests/run.py", "fast"], "returncode": 0},
                                      {"command": ["cmake", "--build", str(self.build)], "returncode": 0}],
                         "artifacts": {ext: artifact.sha256((self.build / f"deskhop.{ext}").read_bytes())
                                       for ext in ("bin", "uf2")}}

    def freeze(self):
        sources, validation = self.proof()
        return artifact.freeze(self.repo, self.build, self.output,
                               source_before=sources, validation=validation)

    def mutate_manifest(self, path, change):
        doc = json.loads(path.read_bytes())
        change(doc)
        path.chmod(0o644)
        path.write_text(json.dumps(doc))

    def test_freeze_dynamic_version_and_self_contained_load(self):
        path = self.freeze()
        candidate = artifact.load_candidate(path)
        self.assertEqual(candidate["build"], "0.137")
        self.assertEqual(candidate["encoded_version"], 237)
        self.assertEqual(candidate["firmware_compatibility"], {"uart_frame_version": 1, "config_version": 10})
        self.assertEqual(candidate["image"], self.image)
        self.assertEqual(candidate["slot_crc"], f"{zlib.crc32(self.image):08x}")
        self.assertEqual(candidate["bin_path"].parent, path.parent)
        self.assertTrue(candidate["git"]["dirty"])
        self.assertEqual(candidate["validation_record"]["source_before"], candidate["source_files"])
        self.assertEqual(candidate["validation_record"]["artifacts"]["bin"], candidate["bin_sha256"])
        self.assertIn("pico-sdk/src/important.c", candidate["source_files"])
        self.assertIn("Pico-PIO-USB/src/usb.c", candidate["source_files"])
        self.assertIn("tests/new_test.py", candidate["source_files"])
        self.assertIn("scripts/new_tool.py", candidate["source_files"])
        (self.build / "deskhop.bin").write_bytes(b"changed mutable output")
        (self.repo / "src/main.c").write_bytes(b"changed checkout")
        self.assertEqual(artifact.load_candidate(path)["image"], self.image)
        self.assertFalse(path.stat().st_mode & 0o222)

    def test_freeze_never_overwrites_an_existing_candidate(self):
        first, second = self.freeze(), self.freeze()
        self.assertNotEqual(first.parent, second.parent)
        self.assertEqual(artifact.load_candidate(first)["bin_sha256"],
                         artifact.load_candidate(second)["bin_sha256"])

    def test_optional_elf_is_copied_and_checked(self):
        (self.build / "deskhop.elf").write_bytes(b"elf evidence")
        path = self.freeze()
        elf = path.parent / "deskhop.elf"
        elf.chmod(0o644)
        elf.write_bytes(b"different elf")
        with self.assertRaisesRegex(artifact.ArtifactError, "hash mismatch"):
            artifact.load_candidate(path)

    def test_unchanged_incremental_output_may_predate_host_tooling(self):
        (self.repo / "scripts/new_tool.py").write_text("new updater tests\n")
        self.freeze()

    def test_requires_fresh_source_bound_evidence(self):
        with self.assertRaisesRegex(artifact.ArtifactError, "fingerprint"):
            artifact.freeze(self.repo, self.build, self.output)
        sources, validation = self.proof()
        (self.repo / "src/new_untracked.c").write_text("new source\n")
        with self.assertRaisesRegex(artifact.ArtifactError, "changed"):
            artifact.freeze(self.repo, self.build, self.output, validation=validation, source_before=sources)

    def test_failed_empty_or_unbound_validation_is_rejected(self):
        sources, good = self.proof()
        for variation in ("failed", "empty", "source", "artifacts", "argv"):
            proof = copy.deepcopy(good)
            if variation == "failed":
                proof["commands"][0]["returncode"] = 1
            elif variation == "empty":
                proof["commands"] = []
            elif variation == "source":
                proof["source_before"] = {}
            elif variation == "artifacts":
                proof["artifacts"]["bin"] = "0" * 64
            elif variation == "argv":
                proof["commands"][0]["command"] = "not argv"
            with self.subTest(variation=variation), self.assertRaises(artifact.ArtifactError):
                artifact.freeze(self.repo, self.build, self.output, source_before=sources, validation=proof)

    def test_deleted_changed_and_untracked_sources_are_fingerprinted(self):
        before = artifact.fingerprint_sources(self.repo)
        (self.repo / "src/new.c").write_text("new source\n")
        self.assertNotEqual(before, artifact.fingerprint_sources(self.repo))
        (self.repo / "src/new.c").unlink()
        (self.repo / "pico-sdk/src/important.c").unlink()
        self.assertNotEqual(before, artifact.fingerprint_sources(self.repo))

    def test_source_symlinks_are_rejected(self):
        (self.repo / "src/external.c").symlink_to(self.build / "deskhop.bin")
        with self.assertRaisesRegex(artifact.ArtifactError, "symlink"):
            artifact.fingerprint_sources(self.repo)

    def test_caches_and_build_outputs_not_in_fingerprint(self):
        before = artifact.fingerprint_sources(self.repo)
        cache = self.repo / "scripts/__pycache__"
        cache.mkdir()
        (cache / "something.pyc").write_bytes(b"bytecode")
        generated = self.repo / "pico-sdk/build"
        generated.mkdir()
        (generated / "thing.c").write_text("not a source input")
        self.assertEqual(before, artifact.fingerprint_sources(self.repo))

    def test_bin_geometry_magic_reserved_and_crc_checked(self):
        variants = {"short": self.image[:-1], "long": self.image + b"x"}
        for name, offset, data in (("magic", artifact.METADATA_OFFSET, b"\0" * 4),
                                   ("reserved", artifact.METADATA_OFFSET + 6, b"\1\0"),
                                   ("crc", 8, b"\0"), ("version", artifact.METADATA_OFFSET + 4, b"\0\0")):
            image = bytearray(self.image)
            image[offset:offset + len(data)] = data
            variants[name] = bytes(image)
        for name, image in variants.items():
            with self.subTest(name=name), self.assertRaises(artifact.ArtifactError):
                artifact._image_metadata(image)

    def test_version_must_match_source(self):
        (self.repo / "CMakeLists.txt").write_text("set(VERSION_MAJOR 0)\nset(VERSION_MINOR 138)\n")
        with self.assertRaisesRegex(artifact.ArtifactError, "version"):
            self.freeze()

    def test_minor_version_must_be_below_1000(self):
        with self.assertRaisesRegex(artifact.ArtifactError, "minor"):
            artifact._version(b"set(VERSION_MAJOR 0)\nset(VERSION_MINOR 1000)\n")

    def test_unsupported_missing_or_ambiguous_uart_version_rejected_at_freeze(self):
        path = self.repo / "src/include/packet.h"
        for data in (b"#define UART_FRAME_VERSION 2\n", b"#define UART_FRAME_VERSION 0\n",
                     b"#define UART_FRAME_VERSION (1)\n", b"#define UART_FRAME_VERSION SOME_VERSION\n",
                     b"#define UART_FRAME_VERSION 1\n#define UART_FRAME_VERSION 1\n", b"// missing\n"):
            path.write_bytes(data)
            with self.subTest(data=data), self.assertRaises(artifact.ArtifactError):
                self.freeze()
        path.unlink()
        with self.assertRaisesRegex(artifact.ArtifactError, "Missing firmware compatibility"):
            self.freeze()

    def test_missing_invalid_or_ambiguous_config_version_rejected(self):
        path = self.repo / "src/include/config_migration.h"
        for data in (b"#define CURRENT_CONFIG_VERSION 0\n", b"#define CURRENT_CONFIG_VERSION X\n",
                     b"#define CURRENT_CONFIG_VERSION 4294967296\n", b"// missing\n"):
            path.write_bytes(data)
            with self.subTest(data=data), self.assertRaises(artifact.ArtifactError):
                self.freeze()
        path.unlink()
        with self.assertRaisesRegex(artifact.ArtifactError, "Missing firmware compatibility"):
            self.freeze()

    def test_protocol_parser_ignores_comments_but_accepts_unsigned_literal(self):
        (self.repo / "src/include/packet.h").write_text(
            "/* #define UART_FRAME_VERSION 9 */\n#define UART_FRAME_VERSION 1u // normal\n")
        self.assertEqual(artifact.load_candidate(self.freeze())["firmware_compatibility"]["uart_frame_version"], 1)

    def test_manifest_compatibility_must_match_frozen_source(self):
        for compatibility in (None, {"uart_frame_version": 2, "config_version": 10},
                               {"uart_frame_version": 1, "config_version": 11}):
            path = self.freeze()
            self.mutate_manifest(path, lambda doc: doc.update(firmware_compatibility=compatibility))
            with self.subTest(compatibility=compatibility), self.assertRaisesRegex(artifact.ArtifactError, "compatibility"):
                artifact.load_candidate(path)

    def test_unknown_uart_in_rehashed_snapshot_rejected_at_load(self):
        path = self.freeze()
        original = path.parent / "source.tar.gz"
        output = io.BytesIO()
        with tarfile.open(original, mode="r:gz") as old, tarfile.open(fileobj=output, mode="w:gz") as new:
            for member in old:
                stream = old.extractfile(member)
                data = stream.read()
                if member.name == "src/include/packet.h":
                    data = b"#define UART_FRAME_VERSION 2\n"
                member.size = len(data)
                new.addfile(member, io.BytesIO(data))
        data = output.getvalue()
        original.chmod(0o644)
        original.write_bytes(data)
        def update(doc):
            doc["source_snapshot"]["sha256"] = artifact.sha256(data)
            doc["source_files"]["src/include/packet.h"] = artifact.sha256(b"#define UART_FRAME_VERSION 2\n")
            doc["firmware_compatibility"]["uart_frame_version"] = 2
        self.mutate_manifest(path, update)
        with self.assertRaisesRegex(artifact.ArtifactError, "Unsupported UART"):
            artifact.load_candidate(path)

    def test_disk_must_match_source(self):
        (self.repo / "disk/disk.img").write_bytes(b"\0" * 65536)
        with self.assertRaisesRegex(artifact.ArtifactError, "disk"):
            self.freeze()

    def test_uf2_reordered_blocks_are_valid(self):
        reversed_blocks = b"".join(self.wire[i:i + 512] for i in range(len(self.wire) - 512, -1, -512))
        artifact.validate_uf2(reversed_blocks, self.image)

    def test_uf2_missing_extra_partial_duplicate_blocks_are_rejected(self):
        for wire in (self.wire[:-512], self.wire + self.wire[:512], self.wire[:-1],
                     self.wire[:512] + self.wire[:512] + self.wire[1024:]):
            with self.subTest(length=len(wire)), self.assertRaises(artifact.ArtifactError):
                artifact.validate_uf2(wire, self.image)

    def test_uf2_rejects_settings_writes_and_other_header_mutations(self):
        mutations = {"magic0": (0, 0), "magic1": (4, 0), "flags": (8, 0),
                     "settings": (12, artifact.SETTINGS_START), "address": (12, artifact.FIRMWARE_START + 1),
                     "payload_size": (16, 128), "index": (20, artifact.UF2_BLOCKS),
                     "total": (24, 1), "family": (28, 0), "end_magic": (508, 0)}
        for name, (offset, value) in mutations.items():
            wire = bytearray(self.wire)
            struct.pack_into("<I", wire, offset, value)
            with self.subTest(name=name), self.assertRaises(artifact.ArtifactError):
                artifact.validate_uf2(bytes(wire), self.image)

    def test_uf2_payload_must_match_bin(self):
        wire = bytearray(self.wire)
        wire[32] ^= 1
        with self.assertRaisesRegex(artifact.ArtifactError, "reconstruct"):
            artifact.validate_uf2(bytes(wire), self.image)

    def test_tampered_frozen_bin_rejected_even_if_manifest_hash_is_updated(self):
        path = self.freeze()
        bin_path = path.parent / "deskhop.bin"
        bin_path.chmod(0o644)
        changed = bytearray(self.image)
        changed[1234] ^= 1
        bin_path.write_bytes(changed)
        with self.assertRaisesRegex(artifact.ArtifactError, "hash mismatch"):
            artifact.load_candidate(path)
        self.mutate_manifest(path, lambda doc: doc["artifacts"]["bin"].update(sha256=artifact.sha256(changed)))
        with self.assertRaisesRegex(artifact.ArtifactError, "CRC"):
            artifact.load_candidate(path)

    def test_candidate_path_traversal_absolute_and_symlink_rejected(self):
        for target in ("../deskhop.bin", "/tmp/deskhop.bin", "nested/deskhop.bin", "..\\deskhop.bin"):
            path = self.freeze()
            self.mutate_manifest(path, lambda doc: doc["artifacts"]["bin"].update(path=target))
            with self.subTest(target=target), self.assertRaisesRegex(artifact.ArtifactError, "relative filenames"):
                artifact.load_candidate(path)
        path = self.freeze()
        bin_path = path.parent / "deskhop.bin"
        bin_path.unlink()
        bin_path.symlink_to(self.build / "deskhop.bin")
        with self.assertRaisesRegex(artifact.ArtifactError, "symlink"):
            artifact.load_candidate(path)

    def test_manifest_metadata_and_geometry_are_verified(self):
        for field, value in (("schema", 8), ("build", "0.999"), ("encoded_version", 999),
                             ("full_slot_crc32", "00000000"), ("metadata_crc32", "00000000"),
                             ("firmware_start", artifact.SETTINGS_START), ("config_included", True)):
            path = self.freeze()
            self.mutate_manifest(path, lambda doc: doc.update({field: value}))
            with self.subTest(field=field), self.assertRaises(artifact.ArtifactError):
                artifact.load_candidate(path)

    def test_snapshot_hash_and_file_fingerprints_checked(self):
        path = self.freeze()
        self.mutate_manifest(path, lambda doc: doc["source_files"].update({"src/main.c": "0" * 64}))
        with self.assertRaisesRegex(artifact.ArtifactError, "fingerprints"):
            artifact.load_candidate(path)

    def test_source_archive_never_extracts_unsafe_members(self):
        for member_name in ("../outside", "/absolute", "src/../outside"):
            path = self.freeze()
            archive_bytes = io.BytesIO()
            with tarfile.open(fileobj=archive_bytes, mode="w:gz") as archive:
                info = tarfile.TarInfo(member_name)
                info.size = 4
                archive.addfile(info, io.BytesIO(b"evil"))
            data = archive_bytes.getvalue()
            archive_path = path.parent / "source.tar.gz"
            archive_path.chmod(0o644)
            archive_path.write_bytes(data)
            self.mutate_manifest(path, lambda doc: doc["source_snapshot"].update(sha256=artifact.sha256(data)))
            with self.subTest(member=member_name), self.assertRaisesRegex(artifact.ArtifactError, "unsafe"):
                artifact.load_candidate(path)
            self.assertFalse((path.parent.parent / "outside").exists())

    def test_frozen_validation_still_must_match_artifact(self):
        path = self.freeze()
        validation_path = path.parent / "validation.json"
        proof = json.loads(validation_path.read_bytes())
        proof["artifacts"]["bin"] = "0" * 64
        data = json.dumps(proof).encode()
        validation_path.chmod(0o644)
        validation_path.write_bytes(data)
        self.mutate_manifest(path, lambda doc: doc["validation"].update(sha256=artifact.sha256(data)))
        with self.assertRaisesRegex(artifact.ArtifactError, "built artifacts"):
            artifact.load_candidate(path)

    def test_source_changed_during_freeze_rejected(self):
        sources, proof = self.proof()
        with patch.object(artifact, "fingerprint_sources", return_value={"changed": "0" * 64}):
            with self.assertRaisesRegex(artifact.ArtifactError, "while freezing"):
                artifact.freeze(self.repo, self.build, self.output, validation=proof, source_before=sources)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
