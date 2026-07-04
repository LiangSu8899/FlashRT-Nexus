"""Deployment manifest parsing for the Nexus serving shell.

The parser intentionally accepts a small YAML subset: nested mappings with
scalar values. That keeps the first serving entry dependency-free while still
covering the deployment files Nexus needs.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any


class ManifestError(ValueError):
    pass


_ENV = re.compile(r"\$\{([^}:]+)(?::-(.*?))?\}|\$([A-Za-z_][A-Za-z0-9_]*)")


def load_manifest(path: str | os.PathLike[str]) -> dict[str, Any]:
    p = Path(path)
    if not p.exists():
        raise ManifestError(f"manifest not found: {p}")
    data = parse_manifest_text(p.read_text())
    if not isinstance(data, dict):
        raise ManifestError("manifest must be a mapping")
    return data


def parse_manifest_text(text: str) -> dict[str, Any]:
    root: dict[str, Any] = {}
    stack: list[tuple[int, dict[str, Any]]] = [(-1, root)]
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = _strip_comment(raw).rstrip()
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip(" "))
        if indent % 2:
            raise ManifestError(f"line {lineno}: indentation must use 2 spaces")
        item = line.strip()
        if item.startswith("- "):
            raise ManifestError(
                f"line {lineno}: lists are not supported in this manifest subset")
        if ":" not in item:
            raise ManifestError(f"line {lineno}: expected key: value")
        key, value = item.split(":", 1)
        key = key.strip()
        value = value.strip()
        if not key:
            raise ManifestError(f"line {lineno}: empty key")
        while stack and indent <= stack[-1][0]:
            stack.pop()
        if not stack:
            raise ManifestError(f"line {lineno}: invalid indentation")
        parent = stack[-1][1]
        if value == "":
            child: dict[str, Any] = {}
            parent[key] = child
            stack.append((indent, child))
        else:
            parent[key] = _parse_scalar(value)
    return root


def get_section(manifest: dict[str, Any], name: str) -> dict[str, Any]:
    value = manifest.get(name, {})
    if not isinstance(value, dict):
        raise ManifestError(f"{name} must be a mapping")
    return value


def require_str(section: dict[str, Any], key: str, *, section_name: str) -> str:
    value = section.get(key)
    if value is None or value == "":
        raise ManifestError(f"{section_name}.{key} is required")
    return str(value)


def optional_str(section: dict[str, Any], key: str, default: str = "") -> str:
    value = section.get(key, default)
    return default if value is None else str(value)


def optional_int(section: dict[str, Any], key: str, default: int) -> int:
    value = section.get(key, default)
    try:
        return int(value)
    except (TypeError, ValueError) as exc:
        raise ManifestError(f"{key} must be an integer") from exc


def optional_bool(section: dict[str, Any], key: str, default: bool) -> bool:
    value = section.get(key, default)
    if isinstance(value, bool):
        return value
    text = str(value).lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    raise ManifestError(f"{key} must be a boolean")


def _strip_comment(line: str) -> str:
    quote = None
    for i, ch in enumerate(line):
        if ch in ("'", '"'):
            quote = None if quote == ch else ch
        elif ch == "#" and quote is None:
            return line[:i]
    return line


def _parse_scalar(value: str) -> Any:
    value = _unquote(value.strip())
    value = _expand_env(value)
    low = value.lower()
    if low in ("true", "false"):
        return low == "true"
    if low in ("null", "none"):
        return None
    try:
        return int(value)
    except ValueError:
        return value


def _unquote(value: str) -> str:
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def _expand_env(value: str) -> str:
    def repl(match: re.Match[str]) -> str:
        name = match.group(1) or match.group(3)
        default = match.group(2)
        if name in os.environ:
            return os.environ[name]
        if default is not None:
            return default
        return ""

    return _ENV.sub(repl, value)
