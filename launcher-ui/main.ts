// ============================================================================
// main.ts — Lean Deno Desktop HTTP/Static Bridge to grml_injector.exe
// ============================================================================
import { dirname, join, resolve } from "jsr:@std/path@1";

interface DiscoveredMod {
  id: string;
  name: string;
  dllPath: string;
  enabled: boolean;
}

interface PlaylistEntry {
  name: string;
  createdAt: string;
  enabledIds: string[];
}

interface CrashSuspect {
  severity: "crash" | "error" | "warn";
  modId: string | null;
  modName: string;
  reason: string;
  source: string;
  faultingModule: string;
}

const uiRoot = import.meta.dirname ? join(import.meta.dirname, "ui") : resolve("ui");
let lastDiscoveryMods: DiscoveredMod[] = [];
let lastLaunch: { pid: number; startedAtMs: number } | null = null;

if ("BrowserWindow" in Deno && typeof (Deno as unknown as { BrowserWindow?: unknown }).BrowserWindow === "function") {
  const BrowserWindowCtor = (Deno as unknown as {
    BrowserWindow: new (opts: { title?: string; width?: number; height?: number; resizable?: boolean }) => unknown;
  }).BrowserWindow;
  new BrowserWindowCtor({
    title: "Glorious Republic Mod Loader (GRML)",
    width: 1240,
    height: 780,
    resizable: true,
  });
}

async function existsFile(path: string): Promise<boolean> {
  try {
    return (await Deno.stat(path)).isFile;
  } catch {
    return false;
  }
}

async function resolveRuntimeBinDir(): Promise<string> {
  const candidates = [
    dirname(Deno.execPath()),
    resolve(dirname(Deno.execPath()), ".."),
    Deno.cwd(),
    resolve(Deno.cwd(), "build/release/bin"),
    resolve(Deno.cwd(), "../build/release/bin"),
  ];
  if (import.meta.dirname) {
    candidates.unshift(resolve(import.meta.dirname, "../build/release/bin"));
    candidates.unshift(import.meta.dirname);
  }
  for (const dir of candidates) {
    if (await existsFile(join(dir, "grml_injector.exe"))) return dir;
  }
  throw new Error("Could not locate grml_injector.exe");
}

async function runInjector(
  binDir: string,
  args: string[],
  stdinJson?: string,
): Promise<{ code: number; stdout: string; stderr: string }> {
  const exe = join(binDir, "grml_injector.exe");
  const cmd = new Deno.Command(exe, {
    args,
    cwd: binDir,
    stdin: stdinJson !== undefined ? "piped" : "null",
    stdout: "piped",
    stderr: "piped",
  });
  if (stdinJson === undefined) {
    const out = await cmd.output();
    return {
      code: out.code,
      stdout: new TextDecoder().decode(out.stdout),
      stderr: new TextDecoder().decode(out.stderr),
    };
  }
  const child = cmd.spawn();
  const writer = child.stdin.getWriter();
  await writer.write(new TextEncoder().encode(stdinJson));
  await writer.close();
  const out = await child.output();
  return {
    code: out.code,
    stdout: new TextDecoder().decode(out.stdout),
    stderr: new TextDecoder().decode(out.stderr),
  };
}

async function readLogTail(binDir: string, maxLines = 250): Promise<string[]> {
  try {
    const raw = await Deno.readTextFile(join(binDir, "grml.log"));
    return raw.split(/\r?\n/).filter((l) => l.length > 0).slice(-maxLines);
  } catch {
    return [];
  }
}

async function readPlaylists(binDir: string): Promise<PlaylistEntry[]> {
  try {
    const raw = await Deno.readTextFile(join(binDir, "grml_playlists.json"));
    const data = JSON.parse(raw.replace(/^\uFEFF/, ""));
    return Array.isArray(data) ? data : [];
  } catch {
    return [];
  }
}

function matchModByModulePath(modPathOrName: string, mods: DiscoveredMod[]): DiscoveredMod | undefined {
  if (!modPathOrName) return undefined;
  const norm = modPathOrName.toLowerCase().replaceAll("/", "\\");
  const leaf = norm.split("\\").pop() || norm;
  return mods.find((m) => {
    const dll = m.dllPath.toLowerCase().replaceAll("/", "\\");
    if (dll === norm || (dll.split("\\").pop() || "") === leaf) return true;
    const parent = dll.slice(0, dll.lastIndexOf("\\"));
    return parent.length > 0 && norm.startsWith(parent + "\\");
  });
}

function analyzeDiagnostics(
  logTail: string[],
  werCrash: { time: string; faultingModuleName: string; faultingModulePath: string; exceptionCode: string; faultOffset: string } | null,
  mods: DiscoveredMod[],
): CrashSuspect[] {
  const suspects: CrashSuspect[] = [];
  const seen = new Set<string>();
  const add = (s: CrashSuspect) => {
    const k = `${s.severity}:${s.modId || s.faultingModule}:${s.reason}`;
    if (!seen.has(k)) {
      seen.add(k);
      suspects.push(s);
    }
  };

  for (const line of logTail) {
    const cgMod = line.match(
      /\[CrashGuard\]\s+FATAL\s+(0x[0-9A-Fa-f]+)\s+in\s+(\S+)\s+\([^)]+\)\s+->\s+Suspect mod:\s+(\S+)\s+\(([^)]+)\)\s+\[([^\]]+)\]/,
    );
    if (cgMod) {
      add({
        severity: "crash",
        modId: cgMod[3],
        modName: cgMod[4],
        reason: `Unhandled ${cgMod[1]} in ${cgMod[2]}`,
        source: "CrashGuard",
        faultingModule: cgMod[5],
      });
      continue;
    }
    const supFail = line.match(/\[Supervisor\]\s+([^:\s]+):\s+(LoadLibraryW.+|GrmlPluginInit exception.+)$/);
    if (supFail) {
      const m = mods.find((x) => x.id === supFail[1]);
      add({
        severity: "error",
        modId: supFail[1],
        modName: m?.name ?? supFail[1],
        reason: supFail[2],
        source: "Supervisor",
        faultingModule: m?.dllPath ?? "",
      });
      continue;
    }
    const tsmRefused = line.match(/\[TesmioCompat\]\s+(Inline hook '[^']+' refused:.+)$/);
    if (tsmRefused) {
      add({
        severity: "warn",
        modId: null,
        modName: tsmRefused[1].match(/'([^']+)'/)?.[1] ?? "TesmioHook",
        reason: tsmRefused[1],
        source: "TesmioCompat",
        faultingModule: "",
      });
    }
  }

  if (werCrash && werCrash.faultingModuleName) {
    const matched = matchModByModulePath(werCrash.faultingModulePath || werCrash.faultingModuleName, mods);
    add({
      severity: "crash",
      modId: matched?.id ?? null,
      modName: matched?.name ?? werCrash.faultingModuleName,
      reason: `Windows WER (${werCrash.time}): ${werCrash.exceptionCode} at ${werCrash.faultingModuleName} +${werCrash.faultOffset}`,
      source: "Windows Event Log",
      faultingModule: werCrash.faultingModulePath || werCrash.faultingModuleName,
    });
  }
  return suspects;
}

async function serveStatic(relPath: string, contentType: string): Promise<Response> {
  const bytes = await Deno.readFile(join(uiRoot, relPath));
  return new Response(bytes, { headers: { "content-type": contentType } });
}

function forwardInjectorJson(res: { code: number; stdout: string; stderr: string }): Response {
  try {
    return Response.json(JSON.parse(res.stdout), { status: res.code === 0 ? 200 : 500 });
  } catch {
    return Response.json({ ok: false, error: res.stderr || res.stdout || "Injector error" }, { status: 500 });
  }
}

Deno.serve(async (req) => {
  const url = new URL(req.url);
  const path = url.pathname;

  try {
    if (req.method === "GET" && (path === "/" || path === "/index.html")) {
      return await serveStatic("index.html", "text/html; charset=utf-8");
    }
    if (req.method === "GET" && path === "/styles.css") {
      return await serveStatic("styles.css", "text/css; charset=utf-8");
    }
    if (req.method === "GET" && path === "/app.js") {
      return await serveStatic("app.js", "application/javascript; charset=utf-8");
    }
    if (req.method === "GET" && path === "/logo.png") {
      return await serveStatic("logo.png", "image/png");
    }

    const binDir = await resolveRuntimeBinDir();

    if (req.method === "GET" && path === "/api/state") {
      const gameOverride = url.searchParams.get("game") ?? "";
      const res = await runInjector(binDir, ["--discover", ...(gameOverride ? ["--game", gameOverride] : [])]);
      if (res.code !== 0) return Response.json({ error: res.stderr || "Discovery failed" }, { status: 500 });
      const discovery = JSON.parse(res.stdout);
      lastDiscoveryMods = discovery.mods ?? [];
      return Response.json({
        discovery,
        grmlIni: discovery.grmlIni ?? {},
        vfs: discovery.vfs ?? { enabled: true, scanWorkshop: true, indexedFiles: 0, conflictCount: 0, packs: [], conflicts: [] },
        logTail: await readLogTail(binDir, 250),
        playlists: await readPlaylists(binDir),
      });
    }

    if (req.method === "GET" && path === "/api/diagnostics") {
      const checkEvents = url.searchParams.get("checkEvents") === "1";
      const checkProcess = checkEvents || url.searchParams.get("checkProcess") === "1";
      const logTail = await readLogTail(binDir, 250);
      let pids: number[] = [];
      if (checkProcess) {
        const sRes = await runInjector(binDir, ["--status"]);
        if (sRes.code === 0) pids = (JSON.parse(sRes.stdout).pids ?? []) as number[];
      }
      const running = pids.length > 0;
      const exitedAfterLaunch = Boolean(
        checkProcess && lastLaunch && !pids.includes(lastLaunch.pid) && (Date.now() - lastLaunch.startedAtMs < 120000),
      );
      const hasCrashGuard = logTail.some((l) => l.includes("[CrashGuard]"));
      let werCrash = null;
      if (checkEvents || (exitedAfterLaunch && !hasCrashGuard)) {
        const wRes = await runInjector(binDir, ["--crash-events"]);
        if (wRes.code === 0) {
          const parsed = JSON.parse(wRes.stdout);
          if (parsed.found) werCrash = parsed;
        }
      }
      return Response.json({
        running,
        pids,
        lastLaunchPid: lastLaunch?.pid ?? null,
        exitedUnexpectedly: exitedAfterLaunch,
        logTail,
        suspects: analyzeDiagnostics(logTail, werCrash, lastDiscoveryMods),
        werCrash,
        checkedAt: new Date().toLocaleTimeString(),
      });
    }

    if (req.method === "POST" && path === "/api/config") {
      return forwardInjectorJson(await runInjector(binDir, ["--save-config"], await req.text()));
    }
    if (req.method === "POST" && path === "/api/vfs") {
      return forwardInjectorJson(await runInjector(binDir, ["--save-vfs"], await req.text()));
    }
    if (req.method === "GET" && path === "/api/mod-ini") {
      const iniPath = url.searchParams.get("iniPath") ?? "";
      return forwardInjectorJson(await runInjector(binDir, ["--read-ini", "--ini", iniPath]));
    }
    if (req.method === "POST" && path === "/api/mod-ini") {
      return forwardInjectorJson(await runInjector(binDir, ["--save-ini"], await req.text()));
    }
    if (req.method === "POST" && path === "/api/generate-schema") {
      return forwardInjectorJson(await runInjector(binDir, ["--generate-schema"], await req.text()));
    }

    if (req.method === "POST" && path === "/api/playlists") {
      const body = await req.json() as { action: "save" | "delete"; name: string; enabledIds?: string[] };
      const list = await readPlaylists(binDir);
      const nextList = body.action === "delete"
        ? list.filter((p) => p.name !== body.name)
        : [
            ...list.filter((p) => p.name !== body.name.trim()),
            { name: body.name.trim(), createdAt: new Date().toISOString(), enabledIds: body.enabledIds ?? [] },
          ];
      await Deno.writeTextFile(join(binDir, "grml_playlists.json"), JSON.stringify(nextList, null, 2) + "\n");
      return Response.json({ ok: true, playlists: nextList });
    }

    if (req.method === "POST" && path === "/api/launch") {
      const body = await req.json().catch(() => ({})) as { gameExe?: string };
      const res = await runInjector(binDir, ["--launch", ...(body.gameExe ? ["--game", body.gameExe] : [])]);
      try {
        const parsed = JSON.parse(res.stdout);
        if (parsed.ok && parsed.processId) {
          lastLaunch = { pid: Number(parsed.processId), startedAtMs: Date.now() };
        }
        return Response.json(parsed, { status: parsed.ok ? 200 : 500 });
      } catch {
        return Response.json({ ok: false, message: res.stderr || res.stdout || "Launch failed" }, { status: 500 });
      }
    }

    return new Response("Not Found", { status: 404 });
  } catch (err) {
    return Response.json({ error: err instanceof Error ? err.message : String(err) }, { status: 500 });
  }
});
