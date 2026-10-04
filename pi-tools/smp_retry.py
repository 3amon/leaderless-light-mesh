"""Small async retry primitive shared with the optional SMP transport."""

from __future__ import annotations

from collections.abc import Awaitable, Callable
from typing import TypeVar

T = TypeVar("T")


async def retry_timeout(
    operation: Callable[[], Awaitable[T]],
    *,
    attempts: int,
    on_retry: Callable[[int], None] | None = None,
) -> T:
    """Retry an async operation only when it raises TimeoutError."""
    if attempts < 1:
        raise ValueError("attempts must be positive")
    for attempt in range(1, attempts + 1):
        try:
            return await operation()
        except TimeoutError:
            if attempt == attempts:
                raise
            if on_retry is not None:
                on_retry(attempt + 1)
    raise AssertionError("unreachable")
