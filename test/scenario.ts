// The boot test, run against a guest under QEMU or v86: switch through the target sizes
// and back, checking the framebuffer of each against the test pattern MODESET.EXE draws.
import { deflateSync } from "node:zlib";
import { join } from "node:path";

/** The displayed framebuffer, 8-bit RGB. */
export interface Frame {
  width: number;
  height: number;
  rgb: Uint8Array;
}

export interface Guest {
  /** Waits up to `timeoutMs` for MODESET.EXE's next line. */
  line(timeoutMs: number): Promise<string>;
  /** Sends one command and returns MODESET.EXE's reply. */
  send(command: string): Promise<string>;
  /** Captures the displayed framebuffer. */
  frame(): Promise<Frame>;
  /** Copies guest RAM. */
  memory(): Promise<Uint8Array>;
  /** The longest driver log seen while polling RAM, where the emulator allows it. */
  polledLog?: () => string;
}

const LOG_TAG = Buffer.from("VBE9XLOG:");

/** The debug build's driver log (src/dbgprint.c), the longest tagged copy in guest RAM. */
export function driverLog(memory: Uint8Array): string {
  return driverLogs(memory).best;
}

/** Every tagged copy of the driver log in guest RAM, and the longest. */
function driverLogs(memory: Uint8Array): { copies: number; best: string } {
  const mem = Buffer.from(memory.buffer, memory.byteOffset, memory.length);
  let best = "";
  let copies = 0;
  for (let i = mem.indexOf(LOG_TAG); i >= 0; i = mem.indexOf(LOG_TAG, i + 1)) {
    copies++;
    const end = mem.indexOf(0, i);
    const text = mem.toString("latin1", i + LOG_TAG.length, end < 0 ? mem.length : end);
    if (text.length > best.length) best = text;
  }
  return { copies, best };
}

/** Keeps the longest driver log seen in guest RAM: the driver's data segment is freed
 * and reused when Windows rejects the driver, so v86 polls RAM while the guest runs.
 */
export class DriverLogWatch {
  best = "";
  constructor(memory: () => Uint8Array) {
    setInterval(() => {
      const { best } = driverLogs(memory());
      if (best.length > this.best.length) this.best = best;
    }, 50).unref();
  }
}

/** Sizes switched to in order, the last being the boot size. */
export const SIZES: [number, number][] = [
  [390, 844],
  [1512, 794],
  [2560, 1080],
  [1024, 768],
];

/** Widest padding the driver adds past a DWORD-aligned width (DISPI_MAX_PAD + alignment). */
const MAX_PAD = 16 + 3;
const BOOT_DELAY_MS = 4000;
const BOOT_POLL_MS = 60_000;
const REDRAW_DELAY_MS = 4000;

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

function crc32(buf: Uint8Array): number {
  let crc = 0xffffffff;
  for (const byte of buf) {
    let c = (crc ^ byte) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

export function png(frame: Frame): Buffer {
  const { width: w, height: h, rgb } = frame;
  const raw = new Uint8Array((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) raw.set(rgb.subarray(y * w * 3, (y + 1) * w * 3), y * (w * 3 + 1) + 1);
  const chunk = (type: string, data: Uint8Array) => {
    const b = Buffer.alloc(12 + data.length);
    b.writeUInt32BE(data.length, 0);
    b.write(type, 4, "latin1");
    Buffer.from(data).copy(b, 8);
    b.writeUInt32BE(crc32(b.subarray(4, 8 + data.length)), 8 + data.length);
    return b;
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    chunk("IHDR", ihdr),
    chunk("IDAT", deflateSync(raw)),
    chunk("IEND", new Uint8Array()),
  ]);
}

const RED = 0xff0000;
const GREEN = 0x00ff00;
const BLUE = 0x0000ff;

/**
 * Checks that `frame` shows the test pattern for a `cx` x `cy` desktop: every row has the
 * red border at columns 0 and cx-1, the top and bottom rows are green between them, and
 * everything else inside is blue. Columns past cx are padding and not checked; neither is
 * (cx-1, cy-1), where MODESET.EXE parks the pointer. Returns the problems found.
 */
export function checkPattern(frame: Frame, cx: number, cy: number): string[] {
  const problems: string[] = [];
  if (frame.height !== cy) problems.push(`height ${frame.height}, expected ${cy}`);
  if (frame.width < cx || frame.width > cx + MAX_PAD) problems.push(`width ${frame.width} for a ${cx} wide desktop`);
  if (problems.length) return problems;
  let wrong = 0;
  for (let y = 0; y < cy; y++) {
    for (let x = 0; x < cx; x++) {
      if (x === cx - 1 && y === cy - 1) continue;
      const want = x === 0 || x === cx - 1 ? RED : y === 0 || y === cy - 1 ? GREEN : BLUE;
      const i = (y * frame.width + x) * 3;
      const got = (frame.rgb[i] << 16) | (frame.rgb[i + 1] << 8) | frame.rgb[i + 2];
      if (got !== want && wrong++ < 8) {
        problems.push(`(${x},${y}) is ${got.toString(16).padStart(6, "0")}, expected ${want.toString(16).padStart(6, "0")}`);
      }
    }
  }
  if (wrong > 8) problems.push(`${wrong} wrong pixels in all`);
  return problems;
}

/** Runs the boot test; writes a PNG of each desktop to `outDir`. Throws on the first failure.
 * The guest gets `bootPolls` minutes to start MODESET.EXE.
 */
export async function runScenario(guest: Guest, outDir: string, bootPolls: number, log: (line: string) => void): Promise<void> {
  const save = async (name: string) => {
    const frame = await guest.frame();
    const path = join(outDir, `${name}.png`);
    await Bun.write(path, png(frame));
    log(`${path}: ${frame.width}x${frame.height}`);
    return frame;
  };

  const saveLog = async () => {
    const now = driverLog(await guest.memory());
    const polled = guest.polledLog?.() ?? "";
    await Bun.write(join(outDir, "driver.log"), polled.length > now.length ? polled : now);
  };
  try {
    await steps(guest, bootPolls, save, saveLog, log);
  } catch (e) {
    await save("failure").catch((f) => log(`no failure frame: ${f}`));
    throw e;
  } finally {
    await saveLog();
  }
}

async function steps(
  guest: Guest,
  bootPolls: number,
  save: (name: string) => Promise<Frame>,
  saveLog: () => Promise<unknown>,
  log: (line: string) => void,
): Promise<void> {
  let ready = "";
  for (let i = 0; i < bootPolls && !ready; i++) {
    ready = await guest.line(BOOT_POLL_MS).catch(() => "");
    if (ready) break;
    await save(`booting-${i}`).catch((e) => log(`no frame while booting: ${e}`));
    await saveLog();
  }
  if (ready !== "READY") throw new Error(`MODESET.EXE did not start: ${JSON.stringify(ready)}`);
  await sleep(BOOT_DELAY_MS);
  await save("boot");
  log(await guest.send("ENUM"));

  for (const [w, h] of SIZES) {
    const reply = await guest.send(`SET ${w} ${h}`);
    if (reply !== `SET 0 ${w}x${h}`) throw new Error(`SET ${w} ${h}: ${reply}`);
    const pattern = await guest.send("PATTERN");
    if (pattern !== `PATTERN ${w}x${h}`) throw new Error(`PATTERN at ${w}x${h}: ${pattern}`);
    const frame = await save(`${w}x${h}-pattern`);
    const problems = checkPattern(frame, w, h);
    if (problems.length) throw new Error(`${w}x${h}: ${problems.join("; ")}`);
    await guest.send("REDRAW");
    await sleep(REDRAW_DELAY_MS);
    await save(`${w}x${h}`);
  }
}
