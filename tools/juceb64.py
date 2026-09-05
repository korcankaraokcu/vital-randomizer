"""JUCE MemoryBlock base64 variant (LSB-first 6-bit packing, '<size>.' prefix)."""
import numpy as np

TABLE = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
_ENC = np.frombuffer(TABLE.encode(), dtype=np.uint8)
_DEC = np.full(256, 0, dtype=np.uint8)
for _i, _c in enumerate(TABLE):
    _DEC[ord(_c)] = _i
_W = (1 << np.arange(6, dtype=np.uint8)).astype(np.uint8)

def encode(data: bytes) -> str:
    bits = np.unpackbits(np.frombuffer(data, np.uint8), bitorder="little")
    pad = (-len(bits)) % 6
    if pad:
        bits = np.concatenate([bits, np.zeros(pad, np.uint8)])
    vals = (bits.reshape(-1, 6) * _W).sum(axis=1).astype(np.uint8)
    return "%d.%s" % (len(data), _ENC[vals].tobytes().decode("ascii"))

def decode(s: str) -> bytes:
    dot = s.index(".")
    nbytes = int(s[:dot])
    vals = _DEC[np.frombuffer(s[dot + 1:].encode("ascii"), np.uint8)]
    bits = ((vals[:, None] >> np.arange(6, dtype=np.uint8)) & 1).astype(np.uint8).ravel()
    need = nbytes * 8
    bits = bits[:need] if len(bits) >= need else np.concatenate([bits, np.zeros(need - len(bits), np.uint8)])
    return np.packbits(bits, bitorder="little").tobytes()
