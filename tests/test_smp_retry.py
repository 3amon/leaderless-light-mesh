import asyncio
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("smp_retry", ROOT / "pi-tools" / "smp_retry.py")
SMP_RETRY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(SMP_RETRY)


class SmpRetryTests(unittest.TestCase):
    def test_timeout_retries_then_returns(self):
        calls = 0
        retries = []

        async def operation():
            nonlocal calls
            calls += 1
            if calls < 3:
                raise TimeoutError("lost")
            return "response"

        result = asyncio.run(
            SMP_RETRY.retry_timeout(operation, attempts=3, on_retry=retries.append)
        )
        self.assertEqual(result, "response")
        self.assertEqual(calls, 3)
        self.assertEqual(retries, [2, 3])

    def test_final_timeout_is_not_hidden(self):
        async def operation():
            raise TimeoutError("lost")

        with self.assertRaises(TimeoutError):
            asyncio.run(SMP_RETRY.retry_timeout(operation, attempts=2))

    def test_non_timeout_failure_is_not_retried(self):
        calls = 0

        async def operation():
            nonlocal calls
            calls += 1
            raise RuntimeError("protocol error")

        with self.assertRaises(RuntimeError):
            asyncio.run(SMP_RETRY.retry_timeout(operation, attempts=3))
        self.assertEqual(calls, 1)


if __name__ == "__main__":
    unittest.main()
