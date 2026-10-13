# SPDX-License-Identifier: BSD-3-Clause
"""Leitura de controles ALSA pela biblioteca já instalada, sem alsa-utils."""
import ctypes as C
from contextlib import contextmanager


class Alsa:
    def __init__(self, card="sofessx8336"):
        self.lib = C.CDLL("libasound.so.2")
        ptr = C.c_void_p
        u = C.c_uint
        i = C.c_int
        signatures = {
            "snd_strerror": (C.c_char_p, [i]),
            "snd_ctl_open": (i, [C.POINTER(ptr), C.c_char_p, i]),
            "snd_ctl_close": (i, [ptr]),
            "snd_ctl_elem_list": (i, [ptr, ptr]),
            "snd_ctl_elem_list_alloc_space": (i, [ptr, u]),
            "snd_ctl_elem_list_free_space": (None, [ptr]),
            "snd_ctl_elem_list_get_count": (u, [ptr]),
            "snd_ctl_elem_list_get_used": (u, [ptr]),
            "snd_ctl_elem_list_get_numid": (u, [ptr, u]),
            "snd_ctl_elem_list_get_name": (C.c_char_p, [ptr, u]),
            "snd_ctl_elem_info": (i, [ptr, ptr]),
            "snd_ctl_elem_info_set_numid": (None, [ptr, u]),
            "snd_ctl_elem_info_get_type": (i, [ptr]),
            "snd_ctl_elem_info_get_count": (u, [ptr]),
            "snd_ctl_elem_info_is_writable": (i, [ptr]),
            "snd_ctl_elem_info_get_min": (C.c_long, [ptr]),
            "snd_ctl_elem_info_get_max": (C.c_long, [ptr]),
            "snd_ctl_elem_read": (i, [ptr, ptr]),
            "snd_ctl_elem_write": (i, [ptr, ptr]),
            "snd_ctl_elem_value_set_numid": (None, [ptr, u]),
            "snd_ctl_elem_value_get_boolean": (i, [ptr, u]),
            "snd_ctl_elem_value_get_integer": (C.c_long, [ptr, u]),
            "snd_ctl_elem_value_get_enumerated": (u, [ptr, u]),
            "snd_ctl_elem_value_set_boolean": (None, [ptr, u, C.c_long]),
            "snd_ctl_elem_value_set_integer": (None, [ptr, u, C.c_long]),
            "snd_ctl_elem_value_set_enumerated": (None, [ptr, u, u]),
            "snd_ctl_elem_id_malloc": (i, [C.POINTER(ptr)]),
            "snd_ctl_elem_id_free": (None, [ptr]),
            "snd_ctl_elem_id_set_numid": (None, [ptr, u]),
            "snd_ctl_get_dB_range": (i, [ptr, ptr, C.POINTER(C.c_long), C.POINTER(C.c_long)]),
            "snd_ctl_convert_from_dB": (i, [ptr, ptr, C.c_long, C.POINTER(C.c_long), i]),
        }
        for kind in ("list", "info", "value"):
            signatures[f"snd_ctl_elem_{kind}_malloc"] = (i, [C.POINTER(ptr)])
            signatures[f"snd_ctl_elem_{kind}_free"] = (None, [ptr])
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, args
        self.handle = ptr()
        self.check(self.lib.snd_ctl_open(C.byref(self.handle), f"hw:{card}".encode(), 0))

    def check(self, status):
        if status < 0:
            raise RuntimeError(self.lib.snd_strerror(status).decode())
        return status

    def close(self):
        if self.handle:
            self.lib.snd_ctl_close(self.handle)
            self.handle = C.c_void_p()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @contextmanager
    def allocated(self, kind):
        obj = C.c_void_p()
        self.check(getattr(self.lib, f"snd_ctl_elem_{kind}_malloc")(C.byref(obj)))
        try:
            yield obj
        finally:
            getattr(self.lib, f"snd_ctl_elem_{kind}_free")(obj)

    def read(self, numid, name=""):
        with self.allocated("info") as info, self.allocated("value") as value:
            self.lib.snd_ctl_elem_info_set_numid(info, numid)
            self.check(self.lib.snd_ctl_elem_info(self.handle, info))
            kind = self.lib.snd_ctl_elem_info_get_type(info)
            count = self.lib.snd_ctl_elem_info_get_count(info)
            result = {"numid": numid, "name": name, "type": kind,
                      "writable": bool(self.lib.snd_ctl_elem_info_is_writable(info))}
            if kind not in (1, 2, 3):
                return result
            self.lib.snd_ctl_elem_value_set_numid(value, numid)
            self.check(self.lib.snd_ctl_elem_read(self.handle, value))
            suffix = {1: "boolean", 2: "integer", 3: "enumerated"}[kind]
            getter = getattr(self.lib, "snd_ctl_elem_value_get_" + suffix)
            result["values"] = [int(getter(value, x)) for x in range(count)]
            if kind == 2:
                result["min"] = self.lib.snd_ctl_elem_info_get_min(info)
                result["max"] = self.lib.snd_ctl_elem_info_get_max(info)
            return result

    def controls(self):
        with self.allocated("list") as listing:
            self.check(self.lib.snd_ctl_elem_list(self.handle, listing))
            count = self.lib.snd_ctl_elem_list_get_count(listing)
            self.check(self.lib.snd_ctl_elem_list_alloc_space(listing, count))
            try:
                self.check(self.lib.snd_ctl_elem_list(self.handle, listing))
                result = []
                for index in range(self.lib.snd_ctl_elem_list_get_used(listing)):
                    numid = int(self.lib.snd_ctl_elem_list_get_numid(listing, index))
                    name = self.lib.snd_ctl_elem_list_get_name(listing, index).decode()
                    try:
                        result.append(self.read(numid, name))
                    except RuntimeError as error:
                        result.append({"numid": numid, "name": name, "error": str(error)})
                return result
            finally:
                self.lib.snd_ctl_elem_list_free_space(listing)

    def write(self, control, values):
        current = self.read(control["numid"], control["name"])
        # Reconfere tipo, quantidade e permissão antes de escrever.
        if not current["writable"] or current["type"] not in (1, 2):
            raise RuntimeError("Controle não gravável ou tipo não permitido: " + current["name"])
        if current["type"] != control["type"] or len(values) != len(current["values"]):
            raise RuntimeError("Controle ALSA mudou desde o backup")
        low, high = (0, 1) if current["type"] == 1 else (current["min"], current["max"])
        if any(not isinstance(v, int) or not low <= v <= high for v in values):
            raise ValueError("Valor fora do intervalo do controle")
        with self.allocated("value") as value:
            self.lib.snd_ctl_elem_value_set_numid(value, current["numid"])
            self.check(self.lib.snd_ctl_elem_read(self.handle, value))
            suffix = "boolean" if current["type"] == 1 else "integer"
            setter = getattr(self.lib, "snd_ctl_elem_value_set_" + suffix)
            for index, item in enumerate(values):
                setter(value, index, item)
            self.check(self.lib.snd_ctl_elem_write(self.handle, value))
        return self.read(current["numid"], current["name"])

    def neutral_volume(self, control):
        """Calcula o nível máximo de até 0 dB pelo TLV do próprio driver."""
        identifier = C.c_void_p()
        self.check(self.lib.snd_ctl_elem_id_malloc(C.byref(identifier)))
        try:
            self.lib.snd_ctl_elem_id_set_numid(identifier, control["numid"])
            low, high, value = C.c_long(), C.c_long(), C.c_long()
            self.check(self.lib.snd_ctl_get_dB_range(self.handle, identifier, C.byref(low), C.byref(high)))
            target = min(0, high.value)
            if target < low.value:
                raise RuntimeError("O controle não oferece nível sem amplificação: " + control["name"])
            self.check(self.lib.snd_ctl_convert_from_dB(self.handle, identifier, target, C.byref(value), -1))
            return value.value
        finally:
            self.lib.snd_ctl_elem_id_free(identifier)
