<script lang="ts">
	import { onMount } from 'svelte';
	import { app } from '$lib/stores/app.svelte';
	import { Badge, Button, PanelCard, SectionHeader, Select, TextInput, ToggleSwitch } from '$lib/components/ui';

	let autoRefresh = $state(false);
	let logSearch = $state('');
	let logLevel = $state('all');
	let logViewerEl = $state<HTMLDivElement | null>(null);

	const levelOptions = [
		{ value: 'all', label: 'All Levels' },
		{ value: 'warn', label: 'Warnings & Errors' },
		{ value: 'error', label: 'Errors Only' }
	];

	const logLines = $derived(app.diagData?.logTail ?? app.logTail ?? []);

	const filteredLines = $derived.by(() => {
		const q = logSearch.trim().toLowerCase();
		return logLines.filter((line) => {
			const up = line.toUpperCase();
			const isErr = up.includes('[ERROR]') || up.includes('[FATAL]') || up.includes('[CRASHGUARD]');
			const isWarn = up.includes('[WARN]');
			if (logLevel === 'error' && !isErr) return false;
			if (logLevel === 'warn' && !isErr && !isWarn) return false;
			if (q && !line.toLowerCase().includes(q)) return false;
			return true;
		});
	});

	const shortSha = $derived.by(() => {
		const sha = app.discovery?.gameSha256 || 'N/A';
		return sha.length > 20 ? `${sha.slice(0, 10)}...${sha.slice(-8)}` : sha;
	});

	const statusBadgeInfo = $derived.by(() => {
		const dd = app.diagData;
		if (!dd) return { text: 'SOVIET64.exe: Idle', variant: 'default' as const };
		if (dd.running) {
			return {
				text: `SOVIET64.exe: Running (PID ${dd.pids.join(', ')}) · ${dd.checkedAt}`,
				variant: 'ready' as const
			};
		}
		if (dd.exitedUnexpectedly || (dd.suspects || []).some((s) => s.severity === 'crash')) {
			return {
				text: `SOVIET64.exe: Exited / Crashed · ${dd.checkedAt}`,
				variant: 'warn' as const
			};
		}
		return {
			text: `SOVIET64.exe: Idle · ${dd.checkedAt}`,
			variant: 'default' as const
		};
	});

	onMount(() => {
		app.startDiagnosticsPolling(() => autoRefresh);
	});

	async function copySha() {
		if (!app.discovery?.gameSha256) return;
		await navigator.clipboard.writeText(app.discovery.gameSha256);
		app.setStatus('Copied SOVIET64.exe SHA-256 to clipboard.');
	}

	async function copyLog() {
		await navigator.clipboard.writeText(logLines.join('\n'));
		app.setStatus(`Copied ${logLines.length} log line(s) to clipboard.`);
	}

	async function disableSuspectMod(modId: string, modName: string) {
		const modObj = app.discovery?.mods.find((m) => m.id === modId);
		if (!modObj) return;
		modObj.enabled = false;
		await app.saveGrmlIni();
		await app.fetchState();
		await app.pollDiagnostics(false, false);
		app.setStatus(`Disabled offending mod '${modName}' in grml.ini.`);
	}

	function lineClass(line: string): string {
		const up = line.toUpperCase();
		if (up.includes('[ERROR]') || up.includes('[FATAL]') || up.includes('[CRASHGUARD]')) {
			return 'log-line log-line-error';
		}
		if (up.includes('[WARN]')) return 'log-line log-line-warn';
		return 'log-line log-line-info';
	}
</script>

<div class="panel diag-panel">
	<div class="diag-header">
		<div>
			<h2 style="margin:0;">Executable &amp; Runtime Diagnostics</h2>
			<div class="muted" style="font-size:12px;margin-top:3px;">
				Binary verification, CrashGuard attribution, and live <code>grml.log</code> stream.
			</div>
		</div>
		<div class="diag-header-actions">
			<Badge variant={statusBadgeInfo.variant}>{statusBadgeInfo.text}</Badge>
			<ToggleSwitch bind:checked={autoRefresh} label="Auto-refresh (5s)" />
			<Button size="sm" onclick={() => app.pollDiagnostics(true, true)}>Scan Crash Events</Button>
			<Button size="sm" onclick={() => app.pollDiagnostics(false, true)}>↻ Refresh</Button>
		</div>
	</div>

	{#if app.discovery}
		<div class="diag-grid">
			<div class="diag-card">
				<div class="diag-label">WRSR Build</div>
				<div class="diag-val">{app.discovery.gameVersion}</div>
				<div class="diag-sub">PE {app.discovery.peTimestamp} · {app.discovery.peSizeOfImage}</div>
			</div>
			<!-- svelte-ignore a11y_click_events_have_key_events -->
			<!-- svelte-ignore a11y_no_static_element_interactions -->
			<div
				class="diag-card"
				style="cursor:pointer;"
				title={`Click to copy SHA-256: ${app.discovery.gameSha256 || 'N/A'}`}
				onclick={copySha}
			>
				<div class="diag-label">SOVIET64.exe SHA-256</div>
				<div class="diag-val">{shortSha}</div>
				<div class="diag-sub">Click to copy full hash</div>
			</div>
			<div class="diag-card" title={app.discovery.loaderDir}>
				<div class="diag-label">Runtime Core</div>
				<div class="diag-val">{app.discovery.loaderDir.split(/[\\/]/).slice(-2).join('\\')}</div>
				<div class="diag-sub">{app.discovery.loaderDir}</div>
			</div>
			{#if app.discovery.dataDir}
				<div class="diag-card" title={app.discovery.dataDir}>
					<div class="diag-label">AppData (Config &amp; Logs)</div>
					<div class="diag-val">{app.discovery.dataDir.split(/[\\/]/).slice(-2).join('\\')}</div>
					<div class="diag-sub">{app.discovery.dataDir}</div>
				</div>
			{/if}
		</div>
	{/if}

	<SectionHeader title="Launch & Crash Attribution" />
	<div style="margin-bottom:14px;">
		{#if (app.diagData?.suspects || []).length === 0}
			<PanelCard tone="ready">
				<div class="control-label" style="color:var(--green);">
					Healthy — No launch crashes or faulting mods detected.
				</div>
			</PanelCard>
		{:else}
			<div class="schema-form">
				{#each app.diagData?.suspects || [] as s, idx (idx)}
					<PanelCard tone={s.severity === 'crash' ? 'danger' : 'default'}>
						<div class="suspect-row">
							<div>
								<div>
									<Badge variant={s.severity === 'warn' ? 'grml' : 'warn'}>
										{s.severity.toUpperCase()}
									</Badge>
									<strong style="margin-left:6px;">{s.modName}</strong>
									<span class="muted">({s.source})</span>
								</div>
								<div class="control-tip" style="margin-top:4px;">
									{s.reason}
									{#if s.faultingModule}
										· <code>{s.faultingModule}</code>
									{/if}
								</div>
							</div>
							{#if s.modId}
								{@const modObj = app.discovery?.mods.find((m) => m.id === s.modId)}
								{#if modObj?.enabled}
									<Button size="sm" onclick={() => disableSuspectMod(s.modId!, s.modName)}>
										Disable Mod
									</Button>
								{/if}
							{/if}
						</div>
					</PanelCard>
				{/each}
			</div>
		{/if}
	</div>

	<div class="diag-log-toolbar">
		<div class="diag-section-hdr" style="margin:0;">Runtime Log (<code>grml.log</code>)</div>
		<div class="diag-log-controls">
			<TextInput bind:value={logSearch} width="190px" placeholder="Filter log lines..." />
			<Select bind:value={logLevel} options={levelOptions} />
			<Button size="sm" onclick={copyLog}>Copy Log</Button>
		</div>
	</div>

	<div class="log-viewer" bind:this={logViewerEl}>
		{#if filteredLines.length === 0}
			<div class="muted">
				{logLines.length === 0
					? 'grml.log is empty or has not been created yet (launch WRSR once to generate runtime logs).'
					: 'No log lines match the current filter.'}
			</div>
		{:else}
			{#each filteredLines as line, idx (idx)}
				<div class={lineClass(line)}>{line}</div>
			{/each}
		{/if}
	</div>
</div>

<style>
	.suspect-row {
		display: flex;
		justify-content: space-between;
		align-items: center;
		gap: 10px;
	}
</style>
