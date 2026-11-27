// Captures clean 1240x780 screenshots of all 8 GRML Launcher pages into docs/screenshots/
// using GloriousRepublicModLoader.exe --serve-only + headless Microsoft Edge CDP.
import { dirname, fromFileUrl, join, resolve } from "jsr:@std/path@1";
import { decodeBase64 } from "jsr:@std/encoding@1/base64";

const scriptDir = dirname(fromFileUrl(import.meta.url));
const rootDir = resolve(scriptDir, "..");
const exePath = join(rootDir, "build", "release", "GloriousRepublicModLoader.exe");
const outDir = join(rootDir, "docs", "screenshots");

function existsSync(p: string): boolean {
  try {
    Deno.statSync(p);
    return true;
  } catch {
    return false;
  }
}

if (!existsSync(exePath)) {
  console.error(`Launcher executable not found: ${exePath}\nRun .\\build.ps1 first.`);
  Deno.exit(1);
}

const edgeCandidates = [
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
  "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
];
const edgePath = edgeCandidates.find((p) => existsSync(p));
if (!edgePath) {
  console.error("Microsoft Edge (msedge.exe) not found.");
  Deno.exit(1);
}

Deno.mkdirSync(outDir, { recursive: true });
const edgeProfile = Deno.makeTempDirSync({ prefix: "grml-screenshots-" });

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function readUntilMatch(
  stream: ReadableStream<Uint8Array>,
  pattern: RegExp,
  timeoutMs = 10000,
): Promise<string> {
  const reader = stream.getReader();
  const decoder = new TextDecoder();
  let buf = "";
  const timer = setTimeout(() => {
    reader.cancel().catch(() => {});
  }, timeoutMs);
  try {
    while (true) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += decoder.decode(value, { stream: true });
      const m = buf.match(pattern);
      if (m) return m[1].trim();
    }
    throw new Error(`Pattern ${pattern} not found in stream`);
  } finally {
    clearTimeout(timer);
    reader.releaseLock();
  }
}

class CdpClient {
  private ws: WebSocket;
  private id = 0;
  private pending = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();

  constructor(wsUrl: string) {
    this.ws = new WebSocket(wsUrl);
    this.ws.addEventListener("message", (ev) => {
      const msg = JSON.parse(String(ev.data)) as {
        id?: number;
        result?: unknown;
        error?: { message?: string };
      };
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id)!;
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error(msg.error.message ?? "CDP error"));
        else resolve(msg.result);
      }
    });
  }

  async connect() {
    await new Promise<void>((res, rej) => {
      this.ws.addEventListener("open", () => res(), { once: true });
      this.ws.addEventListener("error", () => rej(new Error("WebSocket error")), { once: true });
    });
    await this.call("Page.enable");
    await this.call("Emulation.setDeviceMetricsOverride", {
      width: 1240,
      height: 780,
      deviceScaleFactor: 1,
      mobile: false,
    });
  }

  call(method: string, params: Record<string, unknown> = {}): Promise<Record<string, unknown>> {
    const id = ++this.id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, {
        resolve: (v) => resolve((v ?? {}) as Record<string, unknown>),
        reject,
      });
      this.ws.send(JSON.stringify({ id, method, params }));
    });
  }

  async eval(expression: string): Promise<unknown> {
    const res = await this.call("Runtime.evaluate", {
      expression,
      awaitPromise: true,
      returnByValue: true,
    });
    return (res.result as { value?: unknown } | undefined)?.value;
  }

  async screenshot(filePath: string) {
    const res = await this.call("Page.captureScreenshot", { format: "png" });
    Deno.writeFileSync(filePath, decodeBase64(String(res.data ?? "")));
    console.log(`Captured ${filePath}`);
  }

  close() {
    this.ws.close();
  }
}

let serverProc: Deno.ChildProcess | null = null;
let edgeProc: Deno.ChildProcess | null = null;
let cdp: CdpClient | null = null;

try {
  serverProc = new Deno.Command(exePath, {
    args: ["--serve-only"],
    stdin: "piped",
    stdout: "piped",
    stderr: "null",
  }).spawn();
  const url = await readUntilMatch(serverProc.stdout, /GRML_SERVE_URL=(http:\/\/[^\r\n]+)/);

  edgeProc = new Deno.Command(edgePath, {
    args: [
      "--headless=new",
      "--disable-gpu",
      "--no-first-run",
      "--no-default-browser-check",
      "--remote-debugging-port=0",
      "--window-size=1240,780",
      `--user-data-dir=${edgeProfile}`,
      url,
    ],
    stdin: "null",
    stdout: "null",
    stderr: "piped",
  }).spawn();
  const wsBrowserUrl = await readUntilMatch(edgeProc.stderr, /DevTools listening on (ws:\/\/[^\r\n]+)/);
  const baseHttp = wsBrowserUrl.replace(/^ws:/, "http:").replace(/\/devtools\/browser\/.*$/, "");

  let pageTarget: { type: string; webSocketDebuggerUrl?: string } | undefined;
  for (let i = 0; i < 20; i++) {
    const list = (await fetch(`${baseHttp}/json/list`).then((r) => r.json())) as Array<{
      type: string;
      webSocketDebuggerUrl?: string;
    }>;
    pageTarget = list.find((t) => t.type === "page" && t.webSocketDebuggerUrl);
    if (pageTarget) break;
    await sleep(150);
  }
  if (!pageTarget?.webSocketDebuggerUrl) throw new Error("No Edge page target found");

  cdp = new CdpClient(pageTarget.webSocketDebuggerUrl);
  await cdp.connect();

  for (let i = 0; i < 60; i++) {
    const ready = await cdp.eval(`
      (() => {
        const st = document.querySelector('.status-box')?.textContent || '';
        return document.querySelectorAll('.mod-row').length > 0 || (st && !st.includes('Discovering'));
      })()
    `);
    if (ready) break;
    await sleep(250);
  }
  await sleep(300);

  const clickNav = async (label: string) => {
    await cdp!.eval(`
      (() => {
        const btns = [...document.querySelectorAll('.nav-btn')];
        const b = btns.find(el => el.textContent && el.textContent.includes(${JSON.stringify(label)}));
        if (b) b.click();
      })()
    `);
    await sleep(300);
  };

  await clickNav("Mods");
  await cdp.eval(`document.querySelector('.mod-row')?.click()`);
  await sleep(400);
  await cdp.screenshot(join(outDir, "01_mods.png"));

  await clickNav("Playlists");
  await cdp.screenshot(join(outDir, "02_playlists.png"));

  await clickNav("Asset Overrides");
  await cdp.screenshot(join(outDir, "03_overrides.png"));

  await clickNav("Save Inspector");
  for (let i = 0; i < 40; i++) {
    const hasSaves = await cdp.eval(`document.querySelectorAll('.save-row').length > 0`);
    if (hasSaves) break;
    await sleep(250);
  }
  await cdp.eval(`document.querySelector('.save-row')?.click()`);
  await sleep(350);
  await cdp.screenshot(join(outDir, "04_save_inspector.png"));

  await clickNav("Offsets & Signatures");
  await sleep(400);
  await cdp.screenshot(join(outDir, "08_offsets.png"));

  await clickNav("Diagnostics & Logs");
  await sleep(500);
  await cdp.screenshot(join(outDir, "05_diagnostics.png"));

  await clickNav("External Mod UI Guide");
  await cdp.screenshot(join(outDir, "06_ui_guide.png"));

  await clickNav("Settings");
  await cdp.screenshot(join(outDir, "07_settings.png"));

  console.log(`All 8 screenshots saved to ${outDir}`);
} finally {
  cdp?.close();
  try {
    edgeProc?.kill();
  } catch {
    // ignore
  }
  try {
    await serverProc?.stdin.close();
    serverProc?.kill();
  } catch {
    // ignore
  }
  await sleep(400);
  try {
    Deno.removeSync(edgeProfile, { recursive: true });
  } catch {
    // ignore brief Windows handle lock
  }
}
