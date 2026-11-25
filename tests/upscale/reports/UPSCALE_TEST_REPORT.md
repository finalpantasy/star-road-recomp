# Upscale test report (task-23)

Run 2026-11-25 11:52. Result: PASS.

| Check | Result | Detail |
|---|---|---|
| fetch_fsr pin: files present and matching | PASS | FSR v1.0.2 already present and matching in A:\AI\Recomps\thirdparty\fsr |
| AC1 off: presented image equals today's blit at every hashed frame | PASS | 8 equal, 0 different |
| AC2 fsr1 4K: validation layer on, no errors or warnings | PASS | 0 messages |
| AC2 fsr1 4K: pass ran at 1920x1080 -> 3840x2160 | PASS |  |
| AC2 fsr1 4K: PNG pair on A: | PASS | A:\AI\Recomps\smrpg-work\captures\task23\test\upscale_000900_in.png ((1920, 1080), (3840, 2160)) |
| AC3 fsr1 4K: GPU time logged | PASS | easu 0.107, rcas 0.042, copy/blit 0.034, total 0.183 ms (worst 0.220) |
| fsr2 requested: falls back to the plain blit with a reason | PASS |  |
