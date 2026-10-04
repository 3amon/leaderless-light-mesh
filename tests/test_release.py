import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts"))
import release


class ReleaseArtifactTests(unittest.TestCase):
    def image(self, tlv_type, value=b"x"):
        header_size = 32
        payload = b"data"
        header = struct.pack(
            "<IIHHIIBBHI4x",
            0x96F3B83D,
            0,
            header_size,
            0,
            len(payload),
            0,
            1,
            2,
            3,
            4,
        )
        tlv = struct.pack("<HH", 0x6907, 4 + 4 + len(value))
        tlv += struct.pack("<BxH", tlv_type, len(value)) + value
        temporary = tempfile.NamedTemporaryFile(delete=False)
        self.addCleanup(pathlib.Path(temporary.name).unlink, missing_ok=True)
        temporary.write(header + payload + tlv)
        temporary.close()
        return pathlib.Path(temporary.name)

    def uf2(self, image):
        payload = image.read_bytes()
        block = bytearray(512)
        struct.pack_into(
            "<IIIIIIII",
            block,
            0,
            release.UF2_MAGIC_START0,
            release.UF2_MAGIC_START1,
            0,
            0x32000,
            len(payload),
            0,
            1,
            0xADA52840,
        )
        block[32 : 32 + len(payload)] = payload
        struct.pack_into("<I", block, 508, release.UF2_MAGIC_END)
        temporary = tempfile.NamedTemporaryFile(delete=False)
        self.addCleanup(pathlib.Path(temporary.name).unlink, missing_ok=True)
        temporary.write(block)
        temporary.close()
        return pathlib.Path(temporary.name)

    def test_signed_image_reports_version_and_signature(self):
        metadata = release.signed_image_metadata(self.image(0x20))
        self.assertEqual(metadata["version"], "1.2.3+4")
        self.assertEqual(metadata["signature_tlvs"], [0x20])

    def test_unsigned_image_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "not signed"):
            release.signed_image_metadata(self.image(0x10))

    def test_canonical_image_ignores_only_signature_value(self):
        first = self.image(0x20, b"first")
        second = self.image(0x20, b"other")
        self.assertEqual(
            release.canonical_signed_image(first),
            release.canonical_signed_image(second),
        )

    def test_canonical_uf2_ignores_only_embedded_signature_value(self):
        first_image = self.image(0x20, b"first")
        second_image = self.image(0x20, b"other")
        first_uf2 = self.uf2(first_image)
        second_uf2 = self.uf2(second_image)
        self.assertEqual(
            release.canonical_uf2(first_uf2, first_image),
            release.canonical_uf2(second_uf2, second_image),
        )
        changed = bytearray(second_uf2.read_bytes())
        changed[32] ^= 1
        second_uf2.write_bytes(changed)
        self.assertNotEqual(
            release.canonical_uf2(first_uf2, first_image),
            release.canonical_uf2(second_uf2, second_image),
        )

    def test_bad_magic_is_rejected(self):
        image = self.image(0x20)
        data = bytearray(image.read_bytes())
        data[0:4] = b"nope"
        image.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "bad MCUboot magic"):
            release.signed_image_metadata(image)

    def test_uf2_address_range_reports_exclusive_end(self):
        image = self.image(0x20)
        path = self.uf2(image)
        self.assertEqual(
            release.uf2_address_range(path),
            (0x32000, 0x32000 + len(image.read_bytes())),
        )

    def test_uf2_address_range_rejects_bad_magic(self):
        path = self.uf2(self.image(0x20))
        data = bytearray(path.read_bytes())
        data[0:4] = b"nope"
        path.write_bytes(data)
        with self.assertRaisesRegex(ValueError, "invalid UF2 block"):
            release.uf2_address_range(path)

    def test_release_separates_fleet_ota_quick_update_and_provisioning_uf2(self):
        fleet = pathlib.Path("fleet")
        provisioning = pathlib.Path("provisioning")
        copies = release.release_copies(fleet, provisioning)

        self.assertEqual(
            copies[fleet / "thread-ring/zephyr/zephyr.signed.bin"],
            "thread-ring.signed.bin",
        )
        self.assertEqual(
            copies[provisioning / "combined.uf2"],
            "thread-ring.combined.uf2",
        )
        self.assertEqual(
            copies[fleet / "combined.uf2"],
            "thread-ring.quick-update.uf2",
        )
        self.assertNotIn(
            provisioning / "thread-ring/zephyr/zephyr.signed.bin", copies
        )


if __name__ == "__main__":
    unittest.main()
