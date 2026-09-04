"""Discovery for the loadable Nexus FlashRT backend."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path


_LIBRARY_NAMES = (
    "libcapsule_nexus_flashrt.so",
    "libcapsule_nexus_flashrt_abi.so",
)


def find_library(explicit: str | os.PathLike[str] | None = None, *,
                 execution_only: bool = False) -> str:
    """Return a loadable Nexus library name or path.

    Resolution order is explicit path, ``NEXUS_LIB``, installed package/prefix,
    source-checkout build directories, and finally the platform loader.
    """
    requested = explicit or os.environ.get("NEXUS_LIB")
    if requested:
        path = Path(requested).expanduser()
        if not path.is_file():
            raise FileNotFoundError(f"Nexus library not found: {path}")
        return str(path.resolve())

    package_dir = Path(__file__).resolve().parent
    prefix = Path(sys.prefix)
    checkout = package_dir.parent
    roots = (
        package_dir / "lib",
        prefix / "lib",
        prefix / "lib64",
        checkout / "build",
        checkout / "build-release",
    )
    names = (("libcapsule_nexus.so",) + _LIBRARY_NAMES
             if execution_only else _LIBRARY_NAMES)
    candidates = tuple(root / name for root in roots for name in names)
    for path in candidates:
        if path.is_file() and _exports_abi_version(path):
            return str(path.resolve())

    for filename in names:
        name = filename.removeprefix("lib").removesuffix(".so")
        resolved = ctypes.util.find_library(name)
        if resolved:
            return resolved
    raise FileNotFoundError(
        "FlashRT Nexus shared library was not found; build/install the "
        "FlashRT backend or pass producer.nexus_lib")


def _exports_abi_version(path: Path) -> bool:
    try:
        return hasattr(ctypes.CDLL(str(path)), "cap_abi_version")
    except OSError:
        return False


def library_abi_version(
    explicit: str | os.PathLike[str] | None = None,
) -> int:
    """Read the Capsule ABI version exported by the installed library."""
    library = ctypes.CDLL(find_library(explicit))
    fn = library.cap_abi_version
    fn.argtypes = []
    fn.restype = ctypes.c_uint32
    return int(fn())
