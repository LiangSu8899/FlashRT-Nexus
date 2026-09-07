"""Bounded JSON/array encoding for the execution protocol; never pickle."""

import base64
import json
import math

import numpy as np

MAX_BYTES = 64 * 1024 * 1024
_ARRAY = "__nexus_array__"


def _encode(value):
    if isinstance(value, np.ndarray):
        _dtype(value.dtype.str)
        if value.ndim > 8 or value.nbytes > MAX_BYTES:
            raise ValueError("array exceeds message limit")
        return {_ARRAY: base64.b64encode(value.tobytes()).decode("ascii"),
                "shape": list(value.shape), "dtype": value.dtype.str}
    if isinstance(value, dict):
        if _ARRAY in value or any(not isinstance(k, str) for k in value):
            raise ValueError("invalid object keys")
        return {k: _encode(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_encode(v) for v in value]
    if isinstance(value, np.generic):
        return value.item()
    return value


def _dtype(name):
    dtype = np.dtype(name)
    if dtype.kind not in "biuf" or dtype.itemsize not in {1, 2, 4, 8}:
        raise ValueError("unsupported wire dtype")
    return dtype


def _decode(value):
    if isinstance(value, dict):
        if _ARRAY in value:
            dtype = _dtype(value["dtype"])
            shape = value["shape"]
            if (not isinstance(shape, list) or len(shape) > 8
                    or any(type(d) is not int or d < 0 for d in shape)):
                raise ValueError("invalid array shape")
            size = math.prod(shape) * dtype.itemsize
            if size > MAX_BYTES:
                raise ValueError("array exceeds message limit")
            raw = base64.b64decode(value[_ARRAY], validate=True)
            if len(raw) != size:
                raise ValueError("array byte count differs from shape")
            return np.frombuffer(raw, dtype=dtype).reshape(shape).copy()
        return {k: _decode(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_decode(v) for v in value]
    return value


def dumps(value):
    data = json.dumps(_encode(value), allow_nan=False, separators=(",", ":")).encode()
    if len(data) > MAX_BYTES:
        raise ValueError("message exceeds byte limit")
    return data


def loads(data):
    if len(data) > MAX_BYTES:
        raise ValueError("message exceeds byte limit")
    def reject_constant(value):
        raise ValueError("non-finite JSON scalar")
    return _decode(json.loads(data, parse_constant=reject_constant))
