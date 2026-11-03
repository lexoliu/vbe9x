// Boots the site's Windows 95 image with the driver under v86, headless, with the v86
// release, BIOSes and VRAM size the site uses, and runs the boot test. Frames are read
// from v86's VGA memory.
// Usage: bun test/v86.ts [--build DIR] [--out DIR] [--boot-polls MINUTES]
import { mkdir, rm } from "node:fs/promises";
import { statSync } from "node:fs";
import { join, resolve } from "node:path";
// @ts-ignore: v86 ships no types for the ES module build.
import { V86 } from "v86/build/libv86.mjs";
import { assembleBase, install, source } from "./image.ts";
import { Lines } from "./lines.ts";
import { DriverLogWatch, type Frame, type Guest, runScenario } from "./scenario.ts";

const REPLY_TIMEOUT_MS = 300_000;
const MEMORY_SIZE = 64 << 20;
const VGA_MEMORY_SIZE = 8 << 20;

function option(name: string, fallback: string): string {
  const i = process.argv.indexOf(name);
  return resolve(i < 0 ? fallback : process.argv[i + 1]);
}

const build = option("--build", "build");
const out = option("--out", "build/test-v86");
// Minutes the guest gets to start the test program.
const bootPolls = Number(process.argv[process.argv.indexOf("--boot-polls") + 1] ?? "") || 10;
const t0 = performance.now();
const log = (line: string) => console.log(`[${((performance.now() - t0) / 1000).toFixed(1)}s] ${line}`);

await rm(out, { recursive: true, force: true });
await mkdir(out, { recursive: true });
const image = join(out, "c.img");
log("assembling the C: image");
await assembleBase(image);
await install(image, build, out);
const [seabios, vgabios] = await Promise.all([source("seabios"), source("vgabios")]);
const wasm = join(import.meta.dir, "node_modules", "v86", "build", "v86.wasm");

// v86 draws into ImageData, which Bun lacks.
(globalThis as { ImageData?: unknown }).ImageData ??= class {
  constructor(public data: Uint8ClampedArray, public width: number, public height: number) {}
};

// v86's NE2000 relays guest traffic through the global fetch; the test runs offline.
globalThis.fetch = (async () => new Response("offline", { status: 503 })) as unknown as typeof fetch;

const serial = new Lines("COM1", log);
const emu = new V86({
  wasm_path: wasm,
  memory_size: MEMORY_SIZE,
  vga_memory_size: VGA_MEMORY_SIZE,
  bios: { url: seabios },
  vga_bios: { url: vgabios },
  hda: { url: image, size: statSync(image).size, async: true },
  network_relay_url: "fetch",
  disable_speaker: true,
  autostart: true,
});
emu.add_listener("serial0-output-byte", (b: number) => serial.push(String.fromCharCode(b)));

const logWatch = new DriverLogWatch(() => emu.v86.cpu.mem8 as Uint8Array);

function frame(): Frame {
  const vga = emu.v86.cpu.devices.vga;
  if (!vga.svga_enabled) {
    // Standard VGA graphics, as Windows shows before the driver runs or after it fails.
    if (!vga.graphical_mode) throw new Error("v86: the display is in text mode");
    vga.screen_fill_buffer();
    const image = vga.image_data;
    const rgb = new Uint8Array(image.width * image.height * 3);
    for (let i = 0; i < image.width * image.height; i++) rgb.set(image.data.subarray(4 * i, 4 * i + 3), 3 * i);
    return { width: image.width, height: image.height, rgb };
  }
  const width: number = vga.svga_width;
  const height: number = vga.svga_height;
  const bpp: number = vga.svga_bpp;
  const mem: Uint8Array = vga.svga_memory;
  const bytes = (bpp + 7) >> 3;
  const base = vga.svga_offset * bytes;
  const rgb = new Uint8Array(width * height * 3);
  for (let i = 0; i < width * height; i++) {
    const a = base + bytes * i;
    let r: number, g: number, b: number;
    if (bpp === 16) {
      const p = mem[a] | (mem[a + 1] << 8);
      r = (p >> 11) & 31;
      g = (p >> 5) & 63;
      b = p & 31;
      // Expand as QEMU does, so both harnesses see the same 8-bit values.
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
    } else if (bpp === 8) {
      const c = vga.vga256_palette[mem[a]];
      r = (c >> 16) & 255;
      g = (c >> 8) & 255;
      b = c & 255;
    } else {
      b = mem[a];
      g = mem[a + 1];
      r = mem[a + 2];
    }
    rgb.set([r, g, b], 3 * i);
  }
  return { width, height, rgb };
}

try {
  const guest: Guest = {
    line: (timeoutMs) => serial.next(timeoutMs),
    async send(command) {
      log(`> ${command}`);
      emu.serial0_send(`${command}\n`);
      return serial.next(REPLY_TIMEOUT_MS);
    },
    async memory() {
      return emu.v86.cpu.mem8 as Uint8Array;
    },
    polledLog: () => logWatch.best,
    async frame() {
      return frame();
    },
  };
  await runScenario(guest, out, bootPolls, log);
  log("PASS");
} finally {
  await emu.destroy();
  await rm(image, { force: true });
}
process.exit(0);
