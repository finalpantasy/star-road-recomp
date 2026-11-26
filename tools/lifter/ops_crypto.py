"""Crypto op signatures shared by lifted C and the interpreter."""
from ops import op

op("crc32", "rd:Wx rn:Rx rm:Rx size castagnoli",
   "WX(rd, ir_crypto_crc32(RX(rn), RX(rm), size, castagnoli));")
op("aese", "rd:RWv rn:Rv", "WV(rd, ir_crypto_aes_round(RV(rd), RV(rn), 0));")
op("aesd", "rd:RWv rn:Rv", "WV(rd, ir_crypto_aes_round(RV(rd), RV(rn), 1));")
op("aesmc", "rd:Wv rn:Rv", "WV(rd, ir_crypto_aes_mix(RV(rn), 0));")
op("aesimc", "rd:Wv rn:Rv", "WV(rd, ir_crypto_aes_mix(RV(rn), 1));")
op("sha1h", "rd:Wv rn:Rv", "WV(rd, ir_crypto_sha1_h(RV(rn)));")
op("sha1su1", "rd:RWv rn:Rv", "WV(rd, ir_crypto_sha1_su1(RV(rd), RV(rn)));")
op("sha256su0", "rd:RWv rn:Rv", "WV(rd, ir_crypto_sha256_su0(RV(rd), RV(rn)));")
for kind, name in enumerate(("sha1c", "sha1p", "sha1m")):
    op(name, "rd:RWv rn:Rv rm:Rv",
       "WV(rd, ir_crypto_sha1_hash(RV(rd), RV(rn), RV(rm), %d));" % kind)
op("sha1su0", "rd:RWv rn:Rv rm:Rv", "WV(rd, ir_crypto_sha1_su0(RV(rd), RV(rn), RV(rm)));")
op("sha256h", "rd:RWv rn:Rv rm:Rv", "WV(rd, ir_crypto_sha256_hash(RV(rd), RV(rn), RV(rm), 1));")
op("sha256h2", "rd:RWv rn:Rv rm:Rv", "WV(rd, ir_crypto_sha256_hash(RV(rn), RV(rd), RV(rm), 0));")
op("sha256su1", "rd:RWv rn:Rv rm:Rv", "WV(rd, ir_crypto_sha256_su1(RV(rd), RV(rn), RV(rm)));")
op("pmull", "rd:Wv rn:Rv rm:Rv high wide", "WV(rd, ir_crypto_pmull(RV(rn), RV(rm), high, wide));")
