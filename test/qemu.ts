// Boots the site's Windows 95 image with the driver under QEMU -vga std, headless, and runs
// the boot test. Screenshots come from QMP screendump.
// Usage: bun test/qemu.ts [--build DIR] [--out DIR] [--boot-polls MINUTES] [--keep-image]
import { mkdir, rm } from "node:fs/promises";
import { join, resolve } from "node:path";
import type { Socket } from "bun";
import { assembleBase, install } from "./image.ts";
import { Lines } from "./lines.ts";
import { type Frame, type Guest, runScenario } from "./scenario.ts";

const REPLY_TIMEOUT_MS = 120_000;
// The site runs v86 with 8 MiB of VRAM.
const VGA_MEMORY_MB = 8;
const MEMORY_SIZE = 64 << 20;

function option(name: string, fallback: string): string {
  const i = process.argv.indexOf(name);
  return resolve(i < 0 ? fallback : process.argv[i + 1]);
}

const build = option("--build", "build");
const out = option("--out", "build/test-qemu");
// Keeps the C: image after the run, to read its registry and logs.
const keepImage = process.argv.includes("--keep-image");
// Minutes the guest gets to start the test program.
const bootPolls = Number(process.argv[process.argv.indexOf("--boot-polls") + 1] ?? "") || 10;
const t0 = performance.now();
const log = (line: string) => console.log(`[${((performance.now() - t0) / 1000).toFixed(1)}s] ${line}`);

/** Accepts the one connection QEMU makes to a chardev socket. */
function listen(onData: (bytes: Uint8Array) => void) {
  let resolveConn: (s: Socket) => void;
  const connected = new Promise<Socket>((r) => (resolveConn = r));
  const server = Bun.listen({
    hostname: "127.0.0.1",
    port: 0,
    socket: { open: (s) => resolveConn(s), data: (_s, d) => onData(d) },
  });
  return { port: server.port, connected, stop: () => server.stop(true) };
}

function parsePpm(bytes: Uint8Array): Frame {
  const header = Buffer.from(bytes.subarray(0, 64)).toString("latin1");
  const m = /^P6\s+(\d+)\s+(\d+)\s+255\s/.exec(header);
  if (!m) throw new Error(`screendump: not a P6 PPM: ${JSON.stringify(header.slice(0, 20))}`);
  const width = Number(m[1]);
  const height = Number(m[2]);
  return { width, height, rgb: bytes.subarray(m[0].length, m[0].length + width * height * 3) };
}

await rm(out, { recursive: true, force: true });
await mkdir(out, { recursive: true });
const image = join(out, "c.img");
log("assembling the C: image");
await assembleBase(image);
await install(image, build, out);

const serial = new Lines("COM1", log);
const qmpLines = new Lines("QMP", log);
const com = listen((d) => serial.push(d));
const qmp = listen((d) => qmpLines.push(d));

const qemu = Bun.spawn(
  [
    // The PC speaker goes to a silent audio backend, never the host's.
    "qemu-system-i386", "-M", "pc,pcspk-audiodev=snd0", "-audiodev", "none,id=snd0",
    "-cpu", "pentium2", "-m", `${MEMORY_SIZE >> 20}M`,
    "-vga", "std", "-global", `VGA.vgamem_mb=${VGA_MEMORY_MB}`, "-display", "none",
    "-drive", `file=${image},format=raw,if=ide,index=0`,
    // The image's NE2000 is configured at I/O 300h; without the card Windows stops at a dialog.
    "-netdev", "user,id=net0,restrict=on", "-device", "ne2k_isa,iobase=0x300,irq=10,netdev=net0",
    "-chardev", `socket,id=com1,host=127.0.0.1,port=${com.port}`, "-serial", "chardev:com1",
    "-chardev", `socket,id=qmp,host=127.0.0.1,port=${qmp.port}`, "-mon", "chardev=qmp,mode=control",
  ],
  { stdout: "inherit", stderr: "inherit" },
);
log(`qemu pid ${qemu.pid}`);

try {
  const [comSock, qmpSock] = await Promise.all([com.connected, qmp.connected]);
  const qmpCall = async (execute: string, args?: object) => {
    qmpSock.write(`${JSON.stringify({ execute, arguments: args })}\n`);
    for (;;) {
      const r = JSON.parse(await qmpLines.next(REPLY_TIMEOUT_MS));
      if ("error" in r) throw new Error(`QMP ${execute}: ${JSON.stringify(r.error)}`);
      if ("return" in r) return r.return;
    }
  };
  await qmpLines.next(REPLY_TIMEOUT_MS); // greeting
  await qmpCall("qmp_capabilities");

  let dumps = 0;
  const guest: Guest = {
    line: (timeoutMs) => serial.next(timeoutMs),
    async send(command) {
      log(`> ${command}`);
      comSock.write(`${command}\n`);
      return serial.next(REPLY_TIMEOUT_MS);
    },
    async memory() {
      const path = join(out, "memory.bin");
      await qmpCall("pmemsave", { val: 0, size: MEMORY_SIZE, filename: path });
      const bytes = await Bun.file(path).bytes();
      await rm(path);
      return bytes;
    },
    async frame() {
      const path = join(out, `dump-${dumps++}.ppm`);
      await qmpCall("screendump", { filename: path });
      const frame = parsePpm(await Bun.file(path).bytes());
      await rm(path);
      return frame;
    },
  };
  await runScenario(guest, out, bootPolls, log);
  log("PASS");
} finally {
  qemu.kill();
  await qemu.exited;
  com.stop();
  qmp.stop();
  if (!keepImage) await rm(image, { force: true });
}
