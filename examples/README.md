# Examples

| Category | Files |
|---|---|
| Version 0.85 long-line／PCG／audio | `CPB_V085_LONG_LINE_MEGADEMO.BAS`, `CPB_BACH_MEGADEMO.BAS` |
| Graphics | `graphics.bas`, `Lissajous_Gallery.BAS`, `mandel_graphics.bas`, `julia_graphics.bas` |
| 3D | `3dhat.bas`, `3dhat_mesh.bas` |
| Benchmark | `mandel_text.bas`, `picocalc_mand.bas` |
| Basics | `hello.bas`, `v06_features.bas` |

`CPB_V085_LONG_LINE_MEGADEMO.BAS` requires SD CARD or INTERNAL PSRAM backend in v0.92. Its line 100 body is exactly 2,047 characters and demonstrates indexed-color 8×8 PCG tiles combined into a 16×16 animated character, 3 voice MML, and graphics. The PicoCalc interactive line editor cannot enter this line directly; transfer the file to SD and use `LOAD`, then edit with the full-screen Editor if needed.



## Version 0.92: modes and operations

Copy BAS files with their subdirectories to FAT32 SD. Files can navigate them. LOAD the desired source, RUN; unchanged repeated RUN tests cache. Stored Structured source is edited using EDIT or Editor > New Program > Structured BASIC. Display numbers are not part of BAS. Normal examples must finish; error/BREAK cases below intentionally do not.

| Kind | Files | Expected / operation |
|---|---|---|
| Normal Structured | structured/square.bas | 144 |
| Normal Structured | structured/wrap.bas | [CPB] |
| Normal Structured | structured/local_scope.bas | 16 / 100 |
| Normal Structured | structured/global_scope.bas | 10 / 30 / 30 |
| Normal Structured | structured/factorial.bas | 120 |
| Normal Structured | structured/block_if.bas | GOOD |
| Intentional error | structured/depth_limit.bas | FUNCTION CALL DEPTH, row2, function DEEP, depth16; then RUN square |
| BREAK | structured/break_cleanup.bas | Esc to stop; then RUN square to check cleanup |
| Benchmark | stage3/arithmetic-*.bas / numeric-*.bas / fractal-*.bas | A Classic / B colon / C physical rows / D FUNCTION: equivalent output or pixel/color result across variants |
| Benchmark | stage3/tiny-function.bas | call overhead; preserve source and CPU/storage for comparisons |
| Compatibility | stage3/compatibility.bas | recursion/local/string/global checks; final ROUNDING() is -1 |
| Intentional error | stage3/diagnostic.bas | row/function/caller diagnostic; then RUN a valid example |
| BREAK | stage3/break-loop.bas | Esc/Ctrl-C stop and prompt recovery |

Benchmark source loops/resolution/precision are unchanged. PROFILE OFF for timings, COUNT in separate runs. RUN display measures VM execution, not compile/cache-only latency. Host values are not device timings. Source capacities and IL1536/string pool6144 remain relevant. Mega-demo long lines require SD or INTERNAL PSRAM in v0.92; SRAM fallback cannot hold2047 characters. Graphics/audio require the PicoCalc display/audio hardware; classic hello prints a greeting. Config ships with empty SSID/password.
