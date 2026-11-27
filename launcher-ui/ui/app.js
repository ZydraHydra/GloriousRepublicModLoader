let state = null;
let selectedModId = null;
let inspectorTab = "schema";
let modIniCache = new Map();

const $ = (id) => document.getElementById(id);

function setStatus(msg) {
  $("statusToast").textContent = msg;
}

function abiBadgeClass(abi) {
  if (abi === "GRML") return "badge badge-grml";
  if (abi === "RML v1") return "badge badge-rml";
  if (abi === "Tesmio v4") return "badge badge-tesmio";
  return "badge";
}

async function fetchState() {
  setStatus("Discovering mods & WRSR binary...");
  const game = $("gameExeInput").value.trim();
  const qs = game ? `?game=${encodeURIComponent(game)}` : "";
  const resp = await fetch(`/api/state${qs}`);
  const data = await resp.json();
  if (data.error) {
    setStatus(`Error: ${data.error}`);
    return;
  }
  state = data;
  modIniCache.clear();
  renderAll();
  setStatus(`Loaded ${data.discovery.mods.length} mod(s).`);
}

function renderAll() {
  if (!state) return;
  const d = state.discovery;
  $("versionSubtitle").textContent = `GRML v${d.loaderVersion} · Unified F9 Mod Settings Hub`;
  $("gameVersionPill").textContent = d.gameVersion !== "unknown"
    ? `WRSR ${d.gameVersion}`
    : "WRSR (unrecognized build)";
  if (!$("gameExeInput").value && d.gameExe) {
    $("gameExeInput").value = d.gameExe;
  }

  const gen = state.grmlIni?.general || {};
  $("chkMenuStatus").checked = gen.show_menu_status !== "0";
  $("chkWorkshop").checked = gen.scan_workshop !== "0";
  $("chkWip").checked = gen.scan_workshop_wip !== "0";

  const enabledCount = d.mods.filter((m) => m.enabled && !m.blocked).length;
  $("modCountBadge").textContent = `${enabledCount}/${d.mods.length}`;

  renderWarnings(d.warnings);
  renderModsTable();
  renderOverrides();
  renderInspector();
  renderPlaylists();
  renderDiagnostics();
}

function renderWarnings(warnings) {
  const banner = $("warningsBanner");
  if (!warnings || warnings.length === 0) {
    banner.classList.add("hidden");
    banner.textContent = "";
    return;
  }
  banner.classList.remove("hidden");
  banner.textContent = warnings.join(" | ");
}

function renderModsTable() {
  const container = $("modsTable");
  container.innerHTML = "";
  const q = $("searchInput").value.trim().toLowerCase();
  const abiF = $("abiFilter").value;

  const mods = state.discovery.mods.filter((m) => {
    if (abiF !== "all" && m.abi !== abiF) return false;
    if (!q) return true;
    return (
      m.name.toLowerCase().includes(q) ||
      m.id.toLowerCase().includes(q) ||
      (m.author || "").toLowerCase().includes(q) ||
      (m.workshopId || "").includes(q)
    );
  });

  for (const mod of mods) {
    const row = document.createElement("div");
    row.className = "mod-row" +
      (mod.id === selectedModId ? " selected" : "") +
      (!mod.enabled ? " disabled" : "");

    const orderEl = document.createElement("div");
    orderEl.className = "mod-order";
    orderEl.textContent = mod.loadOrder > 0 ? `#${mod.loadOrder}` : "-";

    const chk = document.createElement("input");
    chk.type = "checkbox";
    chk.checked = !mod.blocked && mod.enabled;
    chk.disabled = !!mod.blocked;
    if (mod.blocked) {
      chk.title = mod.warning || "Blocked";
    }
    chk.addEventListener("click", (ev) => ev.stopPropagation());
    chk.addEventListener("change", () => {
      if (mod.blocked) { chk.checked = false; return; }
      mod.enabled = chk.checked;
      row.classList.toggle("disabled", !mod.enabled);
      saveGrmlIni();
    });

    const info = document.createElement("div");
    const title = document.createElement("div");
    title.className = "mod-title";
    title.textContent = mod.name;
    const sub = document.createElement("div");
    sub.className = "mod-sub";
    sub.textContent = `${mod.id} · v${mod.version}${mod.workshopId ? ` · Workshop ${mod.workshopId}` : " · Local"}`;
    info.append(title, sub);

    const badges = document.createElement("div");
    badges.className = "mod-badges";
    if (mod.uiSchema) {
      const uiB = document.createElement("span");
      uiB.className = "badge";
      uiB.textContent = "UI Schema";
      badges.appendChild(uiB);
    }
    if (mod.blocked || mod.warning) {
      const warnB = document.createElement("span");
      warnB.className = "badge badge-warn";
      warnB.textContent = mod.blocked ? "! Built into GRML" : "! Warning";
      warnB.title = mod.warning || "Dependency/conflict issue";
      badges.appendChild(warnB);
    }
    const abiB = document.createElement("span");
    abiB.className = abiBadgeClass(mod.abi);
    abiB.textContent = mod.abi;
    badges.appendChild(abiB);

    row.append(orderEl, chk, info, badges);
    row.addEventListener("click", () => {
      selectedModId = mod.id;
      inspectorTab = mod.uiSchema ? "schema" : "raw";
      renderModsTable();
      renderInspector();
    });
    container.appendChild(row);
  }
}

async function loadModIni(iniPath) {
  if (!iniPath) return { exists: false, sections: [], rawText: "" };
  if (modIniCache.has(iniPath)) return modIniCache.get(iniPath);
  const resp = await fetch(`/api/mod-ini?iniPath=${encodeURIComponent(iniPath)}`);
  const data = await resp.json();
  modIniCache.set(iniPath, data);
  return data;
}

function getIniValue(sections, sectionName, keyName, fallback) {
  const sec = sections.find((s) => s.name.toLowerCase() === (sectionName || "").toLowerCase());
  const ent = sec?.entries.find((e) => e.key.toLowerCase() === (keyName || "").toLowerCase());
  return ent ? ent.value : fallback;
}

async function renderInspector() {
  const pane = $("inspectorPane");
  const mod = state?.discovery.mods.find((m) => m.id === selectedModId);
  if (!mod) {
    pane.innerHTML = `<div class="empty-state">Select a mod to inspect metadata, declarative UI settings, or edit its .ini file.</div>`;
    return;
  }

  const iniData = await loadModIni(mod.iniPath);

  pane.innerHTML = "";
  const hdr = document.createElement("div");
  hdr.className = "insp-header";
  hdr.innerHTML = `
    <h2>${escapeHtml(mod.name)} <span class="${abiBadgeClass(mod.abi)}">${escapeHtml(mod.abi)}</span></h2>
    <div class="insp-meta">
      <div><strong>ID:</strong> <code>${escapeHtml(mod.id)}</code> · <strong>Version:</strong> ${escapeHtml(mod.version)} · <strong>Author:</strong> ${escapeHtml(mod.author || "Unknown")}</div>
      <div><strong>DLL:</strong> <code>${escapeHtml(mod.dllPath)}</code></div>
      <div><strong>INI:</strong> <code>${escapeHtml(mod.iniPath || "None")}</code> ${iniData.exists ? "(found)" : "(will be created on save)"}</div>
      ${mod.description ? `<div style="margin-top:4px;color:var(--text)">${escapeHtml(mod.description)}</div>` : ""}
      ${mod.warning ? `<div style="margin-top:4px;color:var(--danger)"><strong>Warning:</strong> ${escapeHtml(mod.warning)}</div>` : ""}
    </div>
  `;
  pane.appendChild(hdr);

  const tabs = document.createElement("div");
  tabs.className = "insp-tabs";
  const btnSchema = document.createElement("button");
  btnSchema.className = `btn btn-sm ${inspectorTab === "schema" ? "btn-primary" : ""}`;
  btnSchema.textContent = mod.uiSchema
    ? (mod.uiSchema.autoGenerated ? "Auto-Built INI UI" : "Declarative UI Settings")
    : "Settings (No Schema)";
  btnSchema.onclick = () => { inspectorTab = "schema"; renderInspector(); };

  const btnRaw = document.createElement("button");
  btnRaw.className = `btn btn-sm ${inspectorTab === "raw" ? "btn-primary" : ""}`;
  btnRaw.textContent = "Raw INI Editor";
  btnRaw.onclick = () => { inspectorTab = "raw"; renderInspector(); };

  tabs.append(btnSchema, btnRaw);
  pane.appendChild(tabs);

  if (inspectorTab === "schema") {
    if (!mod.uiSchema) {
      const box = document.createElement("div");
      box.className = "control-card";
      box.innerHTML = `
        <div class="control-label">No <code>.ui.yaml</code> / <code>.ui.json</code> schema found for this mod.</div>
        <div class="control-tip" style="margin:6px 0 10px 0;">
          You can still edit its settings in <strong>Raw INI Editor</strong>, or generate a declarative <code>.ui.yaml</code> file right next to the DLL so both the Launcher and in-game <code>F9</code> render friendly controls automatically.
        </div>
      `;
      const genBtn = document.createElement("button");
      genBtn.className = "btn btn-sm btn-primary";
      genBtn.textContent = "Generate .ui.yaml from INI";
      genBtn.onclick = async () => {
        const resp = await fetch("/api/generate-schema", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ dllPath: mod.dllPath, iniPath: mod.iniPath, format: "yaml" }),
        });
        const out = await resp.json();
        if (out.ok) {
          setStatus(`Generated ${out.outPath}`);
          await fetchState();
        }
      };
      box.appendChild(genBtn);
      pane.appendChild(box);
      return;
    }

    const form = document.createElement("div");
    form.className = "schema-form";
    const pendingUpdates = [];

    if (mod.uiSchema.autoGenerated) {
      const autoBanner = document.createElement("div");
      autoBanner.className = "control-card";
      autoBanner.style.display = "flex";
      autoBanner.style.justifyContent = "space-between";
      autoBanner.style.alignItems = "center";
      autoBanner.style.gap = "10px";
      const msg = document.createElement("div");
      msg.className = "control-tip";
      msg.style.margin = "0";
      msg.innerHTML = `<strong>Auto-Built from INI:</strong> Scanned controls &amp; comment tooltips from <code>${escapeHtml(mod.uiSchema.iniFile || "mod.ini")}</code>.`;
      const exportBtn = document.createElement("button");
      exportBtn.className = "btn btn-sm";
      exportBtn.textContent = "Export .ui.yaml";
      exportBtn.onclick = async () => {
        const resp = await fetch("/api/generate-schema", {
          method: "POST",
          headers: { "content-type": "application/json" },
          body: JSON.stringify({ dllPath: mod.dllPath, iniPath: mod.iniPath, format: "yaml" }),
        });
        const out = await resp.json();
        if (out.ok) {
          setStatus(`Exported ${out.outPath}`);
          await fetchState();
        }
      };
      autoBanner.append(msg, exportBtn);
      form.appendChild(autoBanner);
    }

    for (const ctrl of mod.uiSchema.controls) {
      if (ctrl.type === "header") {
        const h = document.createElement("div");
        h.className = "schema-header";
        h.textContent = ctrl.label;
        form.appendChild(h);
        continue;
      }

      const card = document.createElement("div");
      card.className = "control-card";
      const curVal = getIniValue(iniData.sections, ctrl.section, ctrl.key, ctrl.default);

      const top = document.createElement("div");
      top.className = "control-top";
      const lbl = document.createElement("div");
      lbl.className = "control-label";
      lbl.textContent = ctrl.label;
      top.appendChild(lbl);

      const valState = { section: ctrl.section, key: ctrl.key, value: String(curVal) };
      pendingUpdates.push(valState);

      if (ctrl.type === "bool") {
        const chk = document.createElement("input");
        chk.type = "checkbox";
        chk.checked = curVal === "1" || curVal.toLowerCase() === "true";
        valState.value = chk.checked ? "1" : "0";
        chk.onchange = () => { valState.value = chk.checked ? "1" : "0"; };
        top.appendChild(chk);
        card.appendChild(top);
      } else {
        card.appendChild(top);
        const row = document.createElement("div");
        row.className = "control-input-row";

        if (ctrl.type === "int" || ctrl.type === "float") {
          const parsed = Number.parseFloat(curVal);
          const initNum = Number.isFinite(parsed) ? parsed : Number.parseFloat(ctrl.default || "0");
          let min = Number.isFinite(ctrl.min) ? ctrl.min : 0;
          let max = Number.isFinite(ctrl.max) && ctrl.max > min ? ctrl.max : min + 100;
          if (ctrl.hasExplicitRange !== true) {
            if (initNum < min) min = initNum;
            if (initNum > max) max = initNum;
          }
          const step = ctrl.step > 0 ? ctrl.step : (ctrl.type === "int" ? 1 : 0.1);

          const range = document.createElement("input");
          range.type = "range";
          range.min = String(min);
          range.max = String(max);
          range.step = String(step);
          range.value = String(initNum);

          const num = document.createElement("input");
          num.type = "number";
          if (ctrl.hasExplicitRange === true) {
            num.min = String(min);
            num.max = String(max);
          }
          num.step = ctrl.hasExplicitRange === true ? String(step) : "any";
          num.value = String(initNum);
          num.style.width = "88px";

          valState.value = String(initNum);
          range.oninput = () => { num.value = range.value; valState.value = range.value; };
          num.oninput = () => {
            const v = Number.parseFloat(num.value);
            if (Number.isFinite(v)) {
              if (v > Number.parseFloat(range.max)) range.max = String(v);
              if (v < Number.parseFloat(range.min)) range.min = String(v);
              range.value = String(v);
            }
            valState.value = num.value;
          };
          row.append(range, num);
        } else if (ctrl.type === "select") {
          const sel = document.createElement("select");
          sel.style.flex = "1";
          for (const opt of ctrl.options) {
            const o = document.createElement("option");
            o.value = opt;
            o.textContent = opt;
            if (opt === curVal) o.selected = true;
            sel.appendChild(o);
          }
          valState.value = sel.value;
          sel.onchange = () => { valState.value = sel.value; };
          row.appendChild(sel);
        } else {
          const inp = document.createElement("input");
          inp.type = "text";
          inp.style.flex = "1";
          inp.value = curVal;
          inp.oninput = () => { valState.value = inp.value; };
          row.appendChild(inp);
        }
        card.appendChild(row);
      }

      if (ctrl.tooltip) {
        const tip = document.createElement("div");
        tip.className = "control-tip";
        tip.textContent = ctrl.tooltip;
        card.appendChild(tip);
      }
      form.appendChild(card);
    }

    const saveBtn = document.createElement("button");
    saveBtn.className = "btn btn-primary";
    saveBtn.textContent = `Save Settings to ${mod.iniPath.split(/[\\/]/).pop()}`;
    saveBtn.onclick = async () => {
      const resp = await fetch("/api/mod-ini", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ iniPath: mod.iniPath, updates: pendingUpdates }),
      });
      if ((await resp.json()).ok) {
        modIniCache.delete(mod.iniPath);
        setStatus(`Saved ${mod.iniPath}`);
      }
    };
    form.appendChild(saveBtn);
    pane.appendChild(form);
  } else {
    const area = document.createElement("textarea");
    area.className = "raw-ini-area";
    area.value = iniData.rawText || `; ${mod.iniPath}\n[settings]\n`;
    const saveRaw = document.createElement("button");
    saveRaw.className = "btn btn-primary";
    saveRaw.style.marginTop = "10px";
    saveRaw.textContent = "Save Raw INI";
    saveRaw.onclick = async () => {
      const resp = await fetch("/api/mod-ini", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ iniPath: mod.iniPath, rawText: area.value }),
      });
      if ((await resp.json()).ok) {
        modIniCache.delete(mod.iniPath);
        setStatus(`Saved ${mod.iniPath}`);
      }
    };
    pane.append(area, saveRaw);
  }
}

function renderOverrides() {
  const vfs = state?.vfs;
  if (!vfs) return;
  $("chkVfsEnabled").checked = vfs.enabled !== false;
  $("vfsSummaryBadge").textContent = `${vfs.packs.length} pack(s) · ${vfs.indexedFiles} file(s) · ${vfs.conflictCount} conflict(s)`;

  const list = $("vfsPacksList");
  list.innerHTML = "";
  if (vfs.packs.length === 0) {
    list.innerHTML = `<div class="control-card muted">No asset override packs found in <code>&lt;GameDir&gt;\\media_overrides\\</code> or Steam Workshop <code>media_soviet/override</code> folders.</div>`;
  } else {
    vfs.packs.forEach((pk, idx) => {
      const card = document.createElement("div");
      card.className = "playlist-card";
      const left = document.createElement("div");
      left.style.display = "flex";
      left.style.alignItems = "center";
      left.style.gap = "10px";

      const chk = document.createElement("input");
      chk.type = "checkbox";
      chk.checked = !!pk.enabled;
      chk.onchange = () => {
        pk.enabled = chk.checked;
        saveVfsConfig();
      };

      const info = document.createElement("div");
      const badgeCls = pk.source === "Workshop" ? "badge-rml" : "badge-grml";
      info.innerHTML = `
        <div><strong>#${idx + 1} ${escapeHtml(pk.displayName)}</strong> <span class="badge ${badgeCls}">${escapeHtml(pk.source)}</span></div>
        <div class="muted" style="font-size:11px;"><code>${escapeHtml(pk.id)}</code> · ${pk.indexedFiles}/${pk.totalFiles} file(s)${pk.conflictCount > 0 ? ` · <span style="color:var(--warn)">${pk.conflictCount} conflict(s)</span>` : ""}</div>
      `;
      left.append(chk, info);

      const actions = document.createElement("div");
      actions.style.display = "flex";
      actions.style.gap = "6px";
      const upBtn = document.createElement("button");
      upBtn.className = "btn btn-sm";
      upBtn.textContent = "↑";
      upBtn.disabled = idx === 0;
      upBtn.onclick = () => {
        const [moved] = vfs.packs.splice(idx, 1);
        vfs.packs.splice(idx - 1, 0, moved);
        saveVfsConfig();
      };
      const downBtn = document.createElement("button");
      downBtn.className = "btn btn-sm";
      downBtn.textContent = "↓";
      downBtn.disabled = idx + 1 >= vfs.packs.length;
      downBtn.onclick = () => {
        const [moved] = vfs.packs.splice(idx, 1);
        vfs.packs.splice(idx + 1, 0, moved);
        saveVfsConfig();
      };
      actions.append(upBtn, downBtn);
      card.append(left, actions);
      list.appendChild(card);
    });
  }

  const cPanel = $("vfsConflictsPanel");
  const conflicts = vfs.conflicts || [];
  if (conflicts.length === 0) {
    cPanel.innerHTML = `<div class="control-card muted">No asset override conflicts detected across enabled packs.</div>`;
  } else {
    cPanel.innerHTML = "";
    for (const c of conflicts) {
      const row = document.createElement("div");
      row.className = "control-card";
      row.innerHTML = `
        <div><code>${escapeHtml(c.relativePath)}</code></div>
        <div class="control-tip"><strong>Winner:</strong> ${escapeHtml(c.winningPack)} · <strong>Overridden:</strong> ${escapeHtml((c.overriddenPacks || []).join(", "))}</div>
      `;
      cPanel.appendChild(row);
    }
  }
}

async function saveVfsConfig() {
  const vfs = state?.vfs;
  if (!vfs) return;
  const enabledMap = {};
  const orderedIds = vfs.packs.map((p) => {
    enabledMap[p.id] = !!p.enabled;
    return p.id;
  });
  const resp = await fetch("/api/vfs", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      enabled: $("chkVfsEnabled").checked,
      orderedIds,
      enabledMap,
    }),
  });
  if ((await resp.json()).ok) {
    setStatus("Saved Asset Overrides (grml_vfs.ini).");
    await fetchState();
  }
}

function renderPlaylists() {
  const container = $("playlistsList");
  container.innerHTML = "";
  const list = state?.playlists || [];
  if (list.length === 0) {
    container.innerHTML = `<div class="muted">No saved playlists yet.</div>`;
    return;
  }
  for (const pl of list) {
    const card = document.createElement("div");
    card.className = "playlist-card";
    card.innerHTML = `
      <div>
        <strong>${escapeHtml(pl.name)}</strong>
        <div class="muted" style="font-size:11px;">${pl.enabledIds.length} enabled mod(s)</div>
      </div>
    `;
    const actions = document.createElement("div");
    actions.style.display = "flex";
    actions.style.gap = "6px";

    const applyBtn = document.createElement("button");
    applyBtn.className = "btn btn-sm btn-primary";
    applyBtn.textContent = "Apply";
    applyBtn.onclick = async () => {
      const set = new Set(pl.enabledIds);
      for (const m of state.discovery.mods) {
        m.enabled = set.has(m.id);
      }
      await saveGrmlIni();
      await fetchState();
      setStatus(`Applied playlist '${pl.name}'.`);
    };

    const delBtn = document.createElement("button");
    delBtn.className = "btn btn-sm";
    delBtn.textContent = "Delete";
    delBtn.onclick = async () => {
      await fetch("/api/playlists", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ action: "delete", name: pl.name }),
      });
      await fetchState();
    };

    actions.append(applyBtn, delBtn);
    card.appendChild(actions);
    container.appendChild(card);
  }
}

function renderDiagnostics(diagData) {
  const d = state?.discovery;
  if (!d) return;
  $("diagGrid").innerHTML = `
    <div class="diag-card"><div class="diag-label">Detected WRSR Build</div><div class="diag-val">${escapeHtml(d.gameVersion)}</div></div>
    <div class="diag-card"><div class="diag-label">PE TimeDateStamp / Size</div><div class="diag-val">${escapeHtml(d.peTimestamp)} / ${escapeHtml(d.peSizeOfImage)}</div></div>
    <div class="diag-card"><div class="diag-label">SOVIET64.exe SHA-256</div><div class="diag-val">${escapeHtml(d.gameSha256 || "N/A")}</div></div>
    <div class="diag-card"><div class="diag-label">Loader Directory</div><div class="diag-val">${escapeHtml(d.loaderDir)}</div></div>
  `;

  const lines = diagData?.logTail ?? state.logTail ?? [];
  const viewer = $("logViewer");
  const nearBottom = (viewer.scrollHeight - viewer.scrollTop - viewer.clientHeight) < 40;
  viewer.textContent = lines.length > 0
    ? lines.join("\n")
    : "grml.log is empty or has not been created yet (launch WRSR once to generate runtime logs).";
  if (nearBottom) {
    viewer.scrollTop = viewer.scrollHeight;
  }

  const statusBadge = $("liveGameStatus");
  if (statusBadge && diagData) {
    if (diagData.running) {
      statusBadge.textContent = `SOVIET64.exe: Running (PID ${diagData.pids.join(", ")}) · ${diagData.checkedAt}`;
      statusBadge.className = "badge badge-grml";
    } else if (diagData.exitedUnexpectedly || (diagData.suspects || []).some((s) => s.severity === "crash")) {
      statusBadge.textContent = `SOVIET64.exe: Exited / Crashed · ${diagData.checkedAt}`;
      statusBadge.className = "badge badge-tesmio";
    } else {
      statusBadge.textContent = `SOVIET64.exe: Idle · ${diagData.checkedAt}`;
      statusBadge.className = "badge";
    }
  }

  const panel = $("crashSuspectsPanel");
  if (panel) {
    const suspects = diagData?.suspects ?? [];
    if (suspects.length === 0) {
      panel.innerHTML = `<div class="control-card muted">No launch crashes or faulting mods detected.</div>`;
    } else {
      panel.innerHTML = "";
      for (const s of suspects) {
        const card = document.createElement("div");
        card.className = "control-card";
        card.style.display = "flex";
        card.style.justifyContent = "space-between";
        card.style.alignItems = "center";
        card.style.gap = "10px";
        const badgeCls = s.severity === "crash" ? "badge-tesmio" : (s.severity === "error" ? "badge-rml" : "badge");
        const left = document.createElement("div");
        left.innerHTML = `
          <div><span class="badge ${badgeCls}">${escapeHtml(s.severity.toUpperCase())}</span> <strong>${escapeHtml(s.modName)}</strong> <span class="muted">(${escapeHtml(s.source)})</span></div>
          <div class="control-tip" style="margin-top:4px;">${escapeHtml(s.reason)}${s.faultingModule ? ` · <code>${escapeHtml(s.faultingModule)}</code>` : ""}</div>
        `;
        card.appendChild(left);
        if (s.modId) {
          const modObj = state?.discovery?.mods?.find((m) => m.id === s.modId);
          if (modObj && modObj.enabled) {
            const disBtn = document.createElement("button");
            disBtn.className = "btn btn-sm";
            disBtn.textContent = "Disable Mod";
            disBtn.onclick = async () => {
              modObj.enabled = false;
              await saveGrmlIni();
              await fetchState();
              await pollDiagnostics(false);
              setStatus(`Disabled offending mod '${s.modName}' in grml.ini.`);
            };
            card.appendChild(disBtn);
          }
        }
        panel.appendChild(card);
      }
    }
  }
}

let diagTimer = null;
let diagPollInFlight = false;
let launchWatchUntil = 0;

async function pollDiagnostics(checkEvents = false, checkProcess = false) {
  if (diagPollInFlight) return;
  diagPollInFlight = true;
  try {
    const params = new URLSearchParams();
    if (checkEvents) params.set("checkEvents", "1");
    if (checkProcess || checkEvents) params.set("checkProcess", "1");
    const qs = params.toString() ? `?${params.toString()}` : "";
    const resp = await fetch(`/api/diagnostics${qs}`);
    if (!resp.ok) return;
    const data = await resp.json();
    renderDiagnostics(data);
    if (data.exitedUnexpectedly) {
      launchWatchUntil = 0; // Stop watching once process exit/crash is observed
      const crashItem = (data.suspects || []).find((s) => s.severity === "crash");
      if (crashItem) {
        setStatus(`Crash detected: ${crashItem.modName} — ${crashItem.reason}`);
      }
    }
  } catch {
    // ignore transient poll error
  } finally {
    diagPollInFlight = false;
  }
}

function startDiagnosticsPolling() {
  if (diagTimer) return;
  diagTimer = setInterval(() => {
    const onDiagTab = $("view-diagnostics").classList.contains("active");
    const autoOn = $("chkAutoRefreshLog")?.checked ?? false;
    const watchingLaunch = Date.now() < launchWatchUntil;
    if (watchingLaunch) {
      pollDiagnostics(false, true);
    } else if (onDiagTab && autoOn) {
      pollDiagnostics(false, false); // Pure in-process grml.log read, zero subprocesses
    }
  }, 5000);
}

async function saveGrmlIni() {
  if (!state) return;
  const plugins = {};
  for (const m of state.discovery.mods) {
    plugins[m.id] = m.enabled;
  }
  const general = {
    game_path: $("gameExeInput").value.trim(),
    show_menu_status: $("chkMenuStatus").checked ? "1" : "0",
    scan_workshop: $("chkWorkshop").checked ? "1" : "0",
    scan_workshop_wip: $("chkWip").checked ? "1" : "0",
  };
  const resp = await fetch("/api/config", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ general, plugins }),
  });
  if ((await resp.json()).ok) {
    setStatus("Saved grml.ini.");
  }
}

function escapeHtml(str) {
  return String(str ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

// Navigation & Events
document.querySelectorAll(".nav-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".nav-btn").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".view").forEach((v) => v.classList.remove("active"));
    btn.classList.add("active");
    $(`view-${btn.dataset.view}`).classList.add("active");
    if (btn.dataset.view === "diagnostics") {
      pollDiagnostics(false, true);
    }
  });
});

$("btnDiagRefresh").addEventListener("click", () => pollDiagnostics(false, true));
$("btnDiagEvents").addEventListener("click", () => pollDiagnostics(true, true));
$("chkVfsEnabled")?.addEventListener("change", saveVfsConfig);
$("btnReloadVfs")?.addEventListener("click", async () => {
  await fetchState();
  setStatus("Reloaded Asset Override packs.");
});
$("btnReloadMods")?.addEventListener("click", async () => {
  await fetchState();
  setStatus("Reloaded Mods & Workshop list.");
});

$("chkMenuStatus").addEventListener("change", saveGrmlIni);
$("chkWorkshop").addEventListener("change", async () => { await saveGrmlIni(); await fetchState(); });
$("chkWip").addEventListener("change", async () => { await saveGrmlIni(); await fetchState(); });
$("gameExeInput").addEventListener("change", async () => { await saveGrmlIni(); await fetchState(); });

$("searchInput").addEventListener("input", renderModsTable);
$("abiFilter").addEventListener("change", renderModsTable);
$("btnRefresh").addEventListener("click", fetchState);
$("btnEnableAll").addEventListener("click", async () => {
  state.discovery.mods.forEach((m) => { if (!m.blocked) m.enabled = true; });
  await saveGrmlIni();
  await fetchState();
});
$("btnDisableAll").addEventListener("click", async () => {
  state.discovery.mods.forEach((m) => (m.enabled = false));
  await saveGrmlIni();
  await fetchState();
});

$("btnSavePlaylist").addEventListener("click", async () => {
  const name = $("playlistNameInput").value.trim();
  if (!name || !state) return;
  const enabledIds = state.discovery.mods.filter((m) => m.enabled).map((m) => m.id);
  await fetch("/api/playlists", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ action: "save", name, enabledIds }),
  });
  $("playlistNameInput").value = "";
  await fetchState();
  setStatus(`Saved playlist '${name}'.`);
});

$("btnLaunch").addEventListener("click", async () => {
  await saveGrmlIni();
  setStatus("Launching SOVIET64.exe and injecting GRML...");
  const resp = await fetch("/api/launch", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ gameExe: $("gameExeInput").value.trim() }),
  });
  const data = await resp.json();
  setStatus(data.message || (data.ok ? "Launched!" : "Launch failed"));
  if (data.ok) {
    launchWatchUntil = Date.now() + 30000;
    pollDiagnostics(false, true);
  }
});

fetchState();
startDiagnosticsPolling();
