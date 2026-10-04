import dataclasses
import hashlib
import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_TOOLS = ROOT / "pi-tools"
SPEC = importlib.util.spec_from_file_location("safe_diagnostics", PI_TOOLS / "safe_diagnostics.py")
SAFE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = SAFE
SPEC.loader.exec_module(SAFE)


@dataclasses.dataclass
class FailedResponse:
    offset: int
    data: bytes


class SafeDiagnosticTests(unittest.TestCase):
    def test_binary_payload_is_replaced_with_length_and_hash(self):
        payload = b"firmware-secret-payload"
        result = SAFE.summarize(FailedResponse(offset=512, data=payload))
        self.assertEqual(result["offset"], 512)
        self.assertEqual(result["data"]["length"], len(payload))
        self.assertEqual(result["data"]["sha256"], hashlib.sha256(payload).hexdigest())
        self.assertNotIn("firmware-secret", repr(result))

    def test_collections_and_text_are_bounded(self):
        result = SAFE.summarize({"message": "x" * 1000, "values": list(range(100))})
        self.assertLessEqual(len(result["message"]), SAFE.MAX_TEXT + 32)
        self.assertEqual(len(result["values"]), SAFE.MAX_ITEMS + 1)

    def test_cycles_are_safe(self):
        value = []
        value.append(value)
        self.assertEqual(SAFE.summarize(value)[0]["truncated"], "cycle")

    def test_process_output_redacts_bytes_literals(self):
        payload = b"firmware-secret\x00payload"
        text = f"transport failed smp_data={payload!r} rc=5"
        result = SAFE.redact_process_output(text)
        self.assertNotIn("firmware-secret", result)
        self.assertIn(f"length={len(payload)}", result)
        self.assertIn(hashlib.sha256(payload).hexdigest(), result)
        self.assertIn("rc=5", result)

    def test_process_output_is_bounded_after_redaction(self):
        result = SAFE.redact_process_output("prefix " + "x" * 2000, limit=100)
        self.assertLessEqual(len(result), 130)
        self.assertTrue(result.endswith("x" * 100))

    def test_identifier_suffix_is_not_mistaken_for_bytes_literal(self):
        text = "field={'blob': 'still visible'}"
        self.assertEqual(SAFE.redact_process_output(text), text)


if __name__ == "__main__":
    unittest.main()
