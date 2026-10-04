"""Bounded, JSON-safe diagnostic summaries for transport failures."""

from __future__ import annotations

import ast
import dataclasses
import hashlib
from collections.abc import Mapping, Sequence

MAX_DEPTH = 6
MAX_ITEMS = 16
MAX_TEXT = 256
MAX_PROCESS_OUTPUT = 1000


def _redact_bytes_literals(text):
    output = []
    cursor = 0
    while cursor < len(text):
        candidates = []
        for token in ("b'", 'b"'):
            index = text.find(token, cursor)
            while index > 0 and (text[index - 1].isalnum() or text[index - 1] == "_"):
                index = text.find(token, index + 2)
            if index >= 0:
                candidates.append(index)
        if not candidates:
            output.append(text[cursor:])
            break
        start = min(candidates)
        output.append(text[cursor:start])
        quote = text[start + 1]
        end = start + 2
        while end < len(text):
            if text[end] == "\\":
                end += 2
                continue
            if text[end] == quote:
                end += 1
                break
            end += 1
        if end > len(text) or text[end - 1] != quote:
            output.append("<unterminated bytes redacted>")
            break
        literal = text[start:end]
        try:
            payload = ast.literal_eval(literal)
        except (SyntaxError, ValueError):
            payload = None
        if isinstance(payload, bytes):
            output.append(
                "<bytes redacted "
                f"length={len(payload)} sha256={hashlib.sha256(payload).hexdigest()}>"
            )
        else:
            output.append("<bytes redacted>")
        cursor = end
    return "".join(output)


def redact_process_output(value, limit=MAX_PROCESS_OUTPUT):
    """Redact bytes reprs and bound subprocess output before logging it."""
    text = _redact_bytes_literals(str(value))
    text = "".join(char if char in "\r\n\t" or char.isprintable() else "?" for char in text)
    if len(text) > limit:
        return f"<{len(text) - limit} chars omitted>..." + text[-limit:]
    return text


def summarize(value, *, _depth=0, _seen=None):
    """Return useful metadata without dumping payload bytes or unbounded objects."""
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, str):
        if len(value) <= MAX_TEXT:
            return value
        return value[:MAX_TEXT] + f"... <{len(value) - MAX_TEXT} chars omitted>"
    if isinstance(value, (bytes, bytearray, memoryview)):
        payload = bytes(value)
        return {
            "type": "bytes",
            "length": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
        }
    if _depth >= MAX_DEPTH:
        return {"type": type(value).__name__, "truncated": "max depth"}

    seen = set() if _seen is None else _seen
    identity = id(value)
    if identity in seen:
        return {"type": type(value).__name__, "truncated": "cycle"}
    seen.add(identity)
    try:
        if dataclasses.is_dataclass(value):
            return summarize(dataclasses.asdict(value), _depth=_depth + 1, _seen=seen)
        if hasattr(value, "model_dump"):
            return summarize(value.model_dump(), _depth=_depth + 1, _seen=seen)
        if isinstance(value, Mapping):
            items = list(value.items())
            result = {
                str(key)[:MAX_TEXT]: summarize(item, _depth=_depth + 1, _seen=seen)
                for key, item in items[:MAX_ITEMS]
            }
            if len(items) > MAX_ITEMS:
                result["<truncated>"] = f"{len(items) - MAX_ITEMS} items omitted"
            return result
        if isinstance(value, Sequence):
            result = [summarize(item, _depth=_depth + 1, _seen=seen) for item in value[:MAX_ITEMS]]
            if len(value) > MAX_ITEMS:
                result.append(f"<{len(value) - MAX_ITEMS} items omitted>")
            return result
        return {"type": type(value).__name__}
    finally:
        seen.remove(identity)
