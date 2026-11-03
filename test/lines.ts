// Line-oriented reading from a byte stream, with a deadline per line.

export class Lines {
  private partial = "";
  private queue: string[] = [];
  private waiter: ((line: string) => void) | null = null;

  constructor(private readonly label: string, private readonly log: (line: string) => void) {}

  push(bytes: Uint8Array | string): void {
    const text = typeof bytes === "string" ? bytes : Buffer.from(bytes).toString("latin1");
    for (const c of text) {
      if (c === "\r") continue;
      if (c !== "\n") {
        this.partial += c;
        continue;
      }
      const line = this.partial;
      this.partial = "";
      this.log(`${this.label}: ${line}`);
      if (this.waiter) {
        const w = this.waiter;
        this.waiter = null;
        w(line);
      } else this.queue.push(line);
    }
  }

  next(timeoutMs: number): Promise<string> {
    const queued = this.queue.shift();
    if (queued !== undefined) return Promise.resolve(queued);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiter = null;
        reject(new Error(`${this.label}: no line within ${timeoutMs} ms`));
      }, timeoutMs);
      this.waiter = (line) => {
        clearTimeout(timer);
        resolve(line);
      };
    });
  }
}
