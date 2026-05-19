# crypto_hmac.py
import hashlib

_BLOCK_SIZE = 64


def hmac_sha256(key, msg):
    if len(key) > _BLOCK_SIZE:
        key = hashlib.sha256(key).digest()

    if len(key) < _BLOCK_SIZE:
        key = key + b"\x00" * (_BLOCK_SIZE - len(key))

    o_key_pad = bytes([b ^ 0x5C for b in key])
    i_key_pad = bytes([b ^ 0x36 for b in key])

    inner = hashlib.sha256(i_key_pad + msg).digest()
    return hashlib.sha256(o_key_pad + inner).digest()