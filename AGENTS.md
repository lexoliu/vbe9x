# vbe9x

- Build: `tools/build.sh` (release, `build/`) or `tools/build.sh DEBUG=1` (`build/debug/`, keeps a driver log in guest memory). The script fetches the pinned Open Watcom into `~/.cache/vbe9x`; re-pin by changing `URL` and `SHA256` in `tools/watcom.sh`.
- Test: `cd test && bun install`, then `bun test/qemu.ts [--build build/debug]` or `bun test/v86.ts`. Both boot the lexoliu/lexoliu Windows 95 image (chunks pinned in `test/sources.json`) headless, switch modes through `test/guest/modeset.c` and check each framebuffer. Output, screenshots and `driver.log` land in `build/test-qemu` or `build/test-v86`.
- No Microsoft DDK material in the repository. `src/ddk/` holds boxv9x's own C transcriptions (see `THIRD-PARTY-NOTICES`).
- `segment type data` is ignored by this wlink; the data segment is made FIXED with `segment class 'DATA'` in `src/vbe9x.lnk`.
- Never base a pointer on `__segname("_TEXT")` (or any segment) without a symbol defined there: wlink emits a fixup to an empty segment it then drops, and Windows refuses the module with failure code 0 in BOOTLOG.TXT. `wdump -a build/vbe9x.drv` must show no `invalid segment`. Take a segment from a symbol instead (`SELECTOROF(function)`).
- In `#pragma aux` a DWORD parameter in a register pair other than DX:AX or CX:BX is passed high word first in the order Watcom chooses, not the order written; read the generated code with `wdis` before relying on it.
