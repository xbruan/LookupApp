"""临时：算几条 SHA-256 参照值，给 test_sha256.c 当期望（不手抄、不凭记忆）。"""
import hashlib

for n in (55, 56, 63, 64, 65):
    print(n, hashlib.sha256(b"x" * n).hexdigest())
print("abc", hashlib.sha256(b"abc").hexdigest())
print("nist56", hashlib.sha256(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hexdigest())
print("empty", hashlib.sha256(b"").hexdigest())
print("million_a", hashlib.sha256(b"a" * 1000000).hexdigest())
