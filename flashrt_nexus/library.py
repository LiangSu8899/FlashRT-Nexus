"""Discovery for the loadable Nexus FlashRT backend."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path


_LIBRARY_NAME = "libcapsule_nexus_flashrt.so"


def find_library(explicit: str | os.PathLike[str] | None = None) -> str:
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
    candidates = (
        package_dir / "lib" / _LIBRARY_NAME,
        prefix / "lib" / _LIBRARY_NAME,
        prefix / "lib64" / _LIBRARY_NAME,
        checkout / "build" / _LIBRARY_NAME,
        checkout / "build-release" / _LIBRARY_NAME,
    )
    for path in candidates:
        if path.is_file():
            return str(path.resolve())

    resolved = ctypes.util.find_library("capsule_nexus_flashrt")
    if resolved:
        return resolved
    raise FileNotFoundError(
        "FlashRT Nexus shared library was not found; build/install the "
        "FlashRT backend or pass producer.nexus_lib")


def library_abi_version(
    explicit: str | os.PathLike[str] | None = None,
) -> int:
    """Read the Capsule ABI version exported by the installed library."""
    library = ctypes.CDLL(find_library(explicit))
    fn = library.cap_abi_version
    fn.argtypes = []
    fn.restype = ctypes.c_uint32
    return int(fn())

