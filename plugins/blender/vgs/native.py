# SPDX-License-Identifier: GPL-3.0-or-later

"""The decoder library, loaded from bin/ next to this file.

A thin mirror of vgsblender.h. The structs below must match it field for field; the
library reports its API version, and a mismatch refuses to load rather than read a struct
wrongly.
"""

import ctypes
import os
import sys

import numpy as np

API_VERSION = 3

_c_float_p = ctypes.POINTER(ctypes.c_float)


class NativeError(RuntimeError):
    pass


class _Info(ctypes.Structure):
    _fields_ = [
        ("duration", ctypes.c_double),
        ("frame_rate", ctypes.c_double),
        ("start_seconds", ctypes.c_double),
        ("bounds", ctypes.c_double * 6),
        ("frame_count", ctypes.c_uint64),
        ("max_splats", ctypes.c_uint64),
        ("sh_degree", ctypes.c_uint32),
        ("sh_coefficients", ctypes.c_int32),
        ("playback_mode", ctypes.c_int32),
        ("title", ctypes.c_char_p),
        ("author", ctypes.c_char_p),
        ("project", ctypes.c_char_p),
        ("take", ctypes.c_char_p),
        ("studio", ctypes.c_char_p),
        ("copyright", ctypes.c_char_p),
        ("id", ctypes.c_char_p),
        ("uuid", ctypes.c_char_p),
    ]


class _Frame(ctypes.Structure):
    _fields_ = [
        ("seconds", ctypes.c_double),
        ("count", ctypes.c_uint64),
        ("sh_coefficients", ctypes.c_int32),
        ("reserved", ctypes.c_int32),
        ("positions", _c_float_p),
        ("rotations", _c_float_p),
        ("scales", _c_float_p),
        ("radiance", _c_float_p),
        ("sh", _c_float_p),
    ]


_OK, _TIMEOUT = 0, 1

_library = None


def _library_name():
    if sys.platform == "win32":
        return "vgsblender.dll"
    if sys.platform == "darwin":
        return "vgsblender.dylib"
    return "vgsblender.so"


def library():
    """The loaded library, loading it on first use."""
    global _library
    if _library is not None:
        return _library

    path = os.path.join(os.path.dirname(__file__), "bin", _library_name())
    if not os.path.isfile(path):
        raise NativeError("decoder library not found: " + path)
    lib = ctypes.CDLL(path)

    lib.vgsb_api_version.restype = ctypes.c_int
    lib.vgsb_api_version.argtypes = []
    version = lib.vgsb_api_version()
    if version != API_VERSION:
        raise NativeError(
            "decoder library speaks API {:d}, this add-on {:d}: reinstall the add-on".format(
                version, API_VERSION))

    lib.vgsb_last_error.restype = ctypes.c_char_p
    lib.vgsb_last_error.argtypes = []
    lib.vgsb_open.restype = ctypes.c_void_p
    lib.vgsb_open.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
    lib.vgsb_close.restype = None
    lib.vgsb_close.argtypes = [ctypes.c_void_p]
    lib.vgsb_get_info.restype = ctypes.c_int
    lib.vgsb_get_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Info)]
    lib.vgsb_schedule.restype = None
    lib.vgsb_schedule.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.c_int]
    lib.vgsb_acquire.restype = ctypes.c_int
    lib.vgsb_acquire.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_double,
                                 ctypes.POINTER(_Frame)]
    lib.vgsb_release.restype = None
    lib.vgsb_release.argtypes = [ctypes.c_void_p]
    lib.vgsb_set_include_sh.restype = None
    lib.vgsb_set_include_sh.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.vgsb_set_density.restype = None
    lib.vgsb_set_density.argtypes = [ctypes.c_void_p, ctypes.c_float]

    _library = lib
    return lib


def _last_error(lib):
    message = lib.vgsb_last_error()
    return message.decode("utf-8", "replace") if message else "unknown error"


def _text(value):
    return value.decode("utf-8", "replace") if value else ""


class Info:
    """What a capture says about itself, copied out of the library."""

    def __init__(self, raw):
        self.duration = raw.duration
        self.frame_rate = raw.frame_rate
        self.start_seconds = raw.start_seconds
        self.bounds = tuple(raw.bounds)
        self.frame_count = raw.frame_count
        self.max_splats = raw.max_splats
        self.sh_degree = raw.sh_degree
        self.sh_coefficients = raw.sh_coefficients
        self.playback_mode = raw.playback_mode
        self.title = _text(raw.title)
        self.author = _text(raw.author)
        self.project = _text(raw.project)
        self.take = _text(raw.take)
        self.studio = _text(raw.studio)
        self.copyright = _text(raw.copyright)
        self.id = _text(raw.id)
        self.uuid = _text(raw.uuid)


def _view(pointer, length):
    """A numpy view over library memory, no copy. Valid until the frame is released.

    The .view() matters: as_array alone exports the buffer as '<f', which Blender's
    foreach_set does not recognise as a plain float array, and it then copies element by
    element - two hundred times slower than the memcpy it does for 'f'.
    """
    if length == 0 or not pointer:
        return np.empty(0, dtype=np.float32)
    return np.ctypeslib.as_array(pointer, shape=(length,)).view(np.float32)


class Frame:
    """One decoded instant, as views over the library's slot. See vgsblender.h."""

    def __init__(self, raw):
        n = raw.count
        self.seconds = raw.seconds
        self.count = n
        self.positions = _view(raw.positions, n * 3)
        self.rotations = _view(raw.rotations, n * 4)
        self.scales = _view(raw.scales, n * 3)
        self.radiance = _view(raw.radiance, n * 4)
        planes = _view(raw.sh, n * 3 * raw.sh_coefficients)
        self.sh = [planes[k * n * 3:(k + 1) * n * 3] for k in range(raw.sh_coefficients)]


class Player:
    """One open capture and the thread decoding it ahead of the playhead."""

    def __init__(self, path, include_sh=True, slots=4, threads=0):
        lib = library()
        handle = lib.vgsb_open(path.encode("utf-8"), int(include_sh), int(slots), int(threads))
        if not handle:
            raise NativeError(_last_error(lib))
        self._lib = lib
        self._handle = handle
        raw = _Info()
        lib.vgsb_get_info(handle, ctypes.byref(raw))
        self.info = Info(raw)

    def close(self):
        if self._handle:
            self._lib.vgsb_close(self._handle)
            self._handle = None

    def __del__(self):
        self.close()

    def schedule(self, seconds):
        values = (ctypes.c_double * len(seconds))(*seconds)
        self._lib.vgsb_schedule(self._handle, values, len(seconds))

    def acquire(self, seconds, timeout_ms):
        """The frame at `seconds`, or None when it was not ready in time.

        Hold on to the result only until release(): its arrays are the library's.
        """
        raw = _Frame()
        status = self._lib.vgsb_acquire(self._handle, seconds, timeout_ms, ctypes.byref(raw))
        if status == _OK:
            return Frame(raw)
        if status == _TIMEOUT:
            return None
        raise NativeError(_last_error(self._lib))

    def release(self):
        self._lib.vgsb_release(self._handle)

    def set_include_sh(self, include_sh):
        self._lib.vgsb_set_include_sh(self._handle, int(include_sh))

    def set_density(self, density):
        """The fraction of splats frames carry, 0.01 to 1. See vgsblender.h."""
        self._lib.vgsb_set_density(self._handle, float(density))
