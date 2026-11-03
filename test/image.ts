// The Windows 95 C: image lexoliu/lexoliu boots (copy.sh windows95-v3), fetched as the
// site's pinned, hash-verified chunks, assembled and given the driver.
import { existsSync } from "node:fs";
import { mkdir } from "node:fs/promises";
import { homedir } from "node:os";
import { dirname, join } from "node:path";
import sources from "./sources.json";

const HDA_URL = "https://i.copy.sh/windows95-v3/";
const SECTOR = 512;
const FETCH_CONCURRENCY = 16;
const HERE = dirname(import.meta.path);
const ROOT = join(HERE, "..");

export const cacheDir = process.env.VBE9X_CACHE ?? join(process.env.XDG_CACHE_HOME ?? join(homedir(), ".cache"), "vbe9x");

export function sha256(bytes: Uint8Array): string {
  return new Bun.CryptoHasher("sha256").update(bytes).digest("hex");
}

/** Runs a command, failing with its output when it exits non-zero; returns stdout. */
export async function run(cmd: string[], env: Record<string, string> = {}): Promise<string> {
  const proc = Bun.spawn(cmd, { env: { ...process.env, ...env }, stdout: "pipe", stderr: "pipe" });
  const [out, err, code] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text(), proc.exited]);
  if (code !== 0) throw new Error(`${cmd.join(" ")} exited with ${code}\n${out}${err}`);
  return out;
}

async function download(url: string): Promise<Uint8Array> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
  return new Uint8Array(await res.arrayBuffer());
}

/** Returns the local path of `path` under the cache, downloading it when absent or wrong. */
async function verified(path: string, url: string, want: string): Promise<string> {
  if (existsSync(path) && sha256(await Bun.file(path).bytes()) === want) return path;
  const bytes = await download(url);
  const got = sha256(bytes);
  if (got !== want) throw new Error(`${url}: SHA-256 ${got}, expected ${want}`);
  await mkdir(dirname(path), { recursive: true });
  await Bun.write(path, bytes);
  return path;
}

export async function source(name: keyof typeof sources): Promise<string> {
  const { url, sha256: want } = sources[name];
  return verified(join(cacheDir, "sources", name), url, want);
}

/** Assembles the base C: image into `out` from the verified chunks. */
export async function assembleBase(out: string): Promise<void> {
  const manifest = (await Bun.file(await source("hdaManifest")).text())
    .split("\n")
    .filter(Boolean)
    .map((line) => {
      const [hash, name] = line.split("  ");
      return { hash, name };
    });
  const dir = join(cacheDir, "hda");
  let next = 0;
  const worker = async () => {
    while (next < manifest.length) {
      const { hash, name } = manifest[next++];
      await verified(join(dir, name), HDA_URL + name, hash);
    }
  };
  await Promise.all(Array.from({ length: FETCH_CONCURRENCY }, worker));
  const sink = Bun.file(out).writer();
  for (const { name } of manifest) sink.write(await Bun.file(join(dir, name)).bytes());
  await sink.end();
}

const MTOOLS_ENV = { MTOOLS_SKIP_CHECK: "1" };
const DISPLAY_CLASS_KEY = "HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Services\\Class\\Display\\0000";
// Run before Windows starts: replace the display class key (dropping VBEMP's minivdd and
// modes), then add the driver's key and the test program's Run entry.
const AUTOEXEC_LINES = [
  `C:\\WINDOWS\\REGEDIT.EXE /D ${DISPLAY_CLASS_KEY}`,
  "C:\\WINDOWS\\REGEDIT.EXE C:\\VBE9X\\VBE9X.REG",
  "C:\\WINDOWS\\REGEDIT.EXE C:\\VBE9X\\TEST.REG",
];

async function partition(image: string): Promise<string> {
  const mbr = await Bun.file(image).slice(0, SECTOR).bytes();
  if (mbr[510] !== 0x55 || mbr[511] !== 0xaa) throw new Error(`${image}: no MBR signature`);
  return `${image}@@${new DataView(mbr.buffer).getUint32(0x1be + 8, true) * SECTOR}`;
}

/** Installs build/vbe9x.drv and the test program into the C: image at `image`. */
export async function install(image: string, buildDir: string, workDir: string): Promise<void> {
  const part = await partition(image);
  const mcopy = (from: string, to: string) => run(["mcopy", "-o", "-i", part, from, to], MTOOLS_ENV);
  await run(["mmd", "-i", part, "::/VBE9X"], MTOOLS_ENV);
  await mcopy(join(buildDir, "vbe9x.drv"), "::/WINDOWS/SYSTEM/VBE9X.DRV");
  await mcopy(join(ROOT, "install", "vbe9x.reg"), "::/VBE9X/VBE9X.REG");
  await mcopy(join(buildDir, "modeset.exe"), "::/VBE9X/MODESET.EXE");
  await mcopy(join(HERE, "guest", "test.reg"), "::/VBE9X/TEST.REG");

  const autoexec = join(workDir, "AUTOEXEC.BAT");
  await mcopy("::/AUTOEXEC.BAT", autoexec);
  const text = Buffer.from(await Bun.file(autoexec).bytes()).toString("latin1");
  if (text.includes(AUTOEXEC_LINES[1])) throw new Error("AUTOEXEC.BAT already installs the driver");
  const sep = text.length === 0 || text.endsWith("\r\n") ? "" : "\r\n";
  await Bun.write(autoexec, Buffer.from(`${text}${sep}${AUTOEXEC_LINES.join("\r\n")}\r\n`, "latin1"));
  await mcopy(autoexec, "::/AUTOEXEC.BAT");
}
