<script lang="ts">
	import { onMount } from 'svelte';
	import { app } from '$lib/stores/app.svelte';
	import { i18n, t } from '$lib/i18n/index.svelte';
	import { openExternalUrl } from '$lib/utils';
	import { Button } from '$lib/components/ui';
	import PlaylistsView from '$lib/views/PlaylistsView.svelte';
	import ModsView from '$lib/views/ModsView.svelte';
	import OverridesView from '$lib/views/OverridesView.svelte';
	import SaveInspectorView from '$lib/views/SaveInspectorView.svelte';
	import OffsetsView from '$lib/views/OffsetsView.svelte';
	import DiagnosticsView from '$lib/views/DiagnosticsView.svelte';
	import SchemaHelpView from '$lib/views/SchemaHelpView.svelte';
	import SettingsView from '$lib/views/SettingsView.svelte';

	const enabledCount = $derived(
		(app.discovery?.mods || []).filter((m) => m.enabled && !m.blocked).length
	);
	const totalCount = $derived((app.discovery?.mods || []).length);
	const totalOffsets = $derived((app.discovery?.offsets || []).length);
	const okOffsets = $derived(
		(app.discovery?.offsets || []).filter((o) => o.status === 'OK' || o.status === 'CACHED').length
	);
	const verLabel = $derived(
		app.discovery && app.discovery.gameVersion !== 'unknown'
			? t('header.wrsrVersion', { version: app.discovery.gameVersion })
			: t('header.wrsrDetecting')
	);
	const incompatibleEnabledMods = $derived(
		(app.discovery?.mods || []).filter(
			(m) => m.enabled && !m.blocked && m.offsetCompatibility === 'needs_update'
		)
	);
	const backupPercent = $derived.by(() => {
		const bp = app.backupProgress;
		if (!bp) return 0;
		if (bp.phase === 'pruning' || bp.phase === 'done') return 100;
		if (bp.totalBytes > 0) return Math.min(100, Math.round((bp.copiedBytes / bp.totalBytes) * 100));
		if (bp.totalFiles > 0) return Math.min(100, Math.round((bp.completedFiles / bp.totalFiles) * 100));
		return 0;
	});

	function formatBytes(bytes: number): string {
		if (bytes < 1024) return `${bytes} B`;
		if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
		if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
		return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
	}

	let offsetBannerDismissed = $state(false);

	let playingInternationale = $state(false);
	let audioEl: HTMLAudioElement | null = null;

	function toggleInternationale() {
		if (!audioEl) {
			audioEl = new Audio('/internationale.ogg');
			audioEl.loop = true;
		}
		if (playingInternationale) {
			audioEl.pause();
			audioEl.currentTime = 0;
			playingInternationale = false;
		} else {
			audioEl.play().catch(() => {});
			playingInternationale = true;
		}
	}

	onMount(() => {
		i18n.init().then(() => {
			if (!app.statusToast) app.setStatus(t('common.ready'));
		});
		app.fetchState().then(() => {
			app.checkForUpdates(false);
			app.pollDiagnostics(false, true);
			app.startDiagnosticsPolling();
		});
		const latestSaveTimer = setInterval(() => {
			app.refreshLatestSave();
		}, 5000);
		return () => {
			clearInterval(latestSaveTimer);
			if (audioEl) {
				audioEl.pause();
				audioEl = null;
			}
		};
	});

	function inspectCrashSave() {
		app.switchView('saves');
		app.fetchSaves(app.lastCrashSaveName || null);
	}
</script>

<div class="app-shell">
	<header class="topbar">
		<div class="brand">
			<button
				type="button"
				class="logo-flip-btn"
				class:flipped={playingInternationale}
				title={playingInternationale ? t('header.stopInternationale') : t('header.title')}
				onclick={toggleInternationale}
			>
				<span class="logo-flip-inner">
					<img class="logo-face logo-front" src="/logo.png" alt="GRML" />
					<span class="logo-face logo-back" aria-hidden="true">
						<svg viewBox="0 0 36 36" width="36" height="36" xmlns="http://www.w3.org/2000/svg">
							<defs>
								<linearGradient id="grmlFlagBg" x1="0%" y1="0%" x2="100%" y2="100%">
									<stop offset="0%" stop-color="#c93b32" />
									<stop offset="55%" stop-color="#9e1f19" />
									<stop offset="100%" stop-color="#7a1511" />
								</linearGradient>
							</defs>
							<rect width="36" height="36" fill="url(#grmlFlagBg)" />
							<!-- Playful gold waving ribbon -->
							<path
								d="M0 28 Q9 24 18 27 T36 25 L36 36 L0 36 Z"
								fill="#d4a853"
								opacity="0.24"
							/>
							<!-- Golden beamed eighth-note (cracktro music joke) -->
							<path
								d="M14 23 V11 L24 9 V21"
								fill="none"
								stroke="#f0c850"
								stroke-width="2.4"
								stroke-linecap="round"
								stroke-linejoin="round"
							/>
							<circle cx="11.8" cy="23.5" r="3" fill="#f0c850" />
							<circle cx="21.8" cy="21.5" r="3" fill="#f0c850" />
						</svg>
					</span>
				</span>
			</button>
			<div class="brand-text">
				<div class="brand-title">{t('header.title')}</div>
			</div>
		</div>

		<div class="game-bar">
			{#if app.updateInfo?.updateAvailable && app.updateInfo.releaseUrl}
				<Button
					size="sm"
					title={t('header.updateAvailableTitle', { version: app.updateInfo.latestVersion ?? '' })}
					onclick={() => openExternalUrl(app.updateInfo!.releaseUrl!)}
				>
					{t('header.updateAvailableBadge', { version: app.updateInfo.latestVersion ?? '' })}
				</Button>
			{/if}
			<span class="pill" title={app.discovery?.gameExe || t('header.gameExeNotDetected')}>{verLabel}</span>
			{#if app.latestSave}
				<Button
					disabled={app.launchLocked}
					onclick={() => app.launchWrsr(app.latestSave)}
					title={t('header.continueSaveTitle', { save: app.latestSave })}
				>
					{t('header.continueSave', { save: app.latestSave })}
				</Button>
			{/if}
			<Button variant="primary" disabled={app.launchLocked} onclick={() => app.launchWrsr()}>{t('header.launchWrsr')}</Button>
		</div>
	</header>

	{#if incompatibleEnabledMods.length > 0 && !offsetBannerDismissed}
		<div class="post-crash-banner">
			<span>
				Offset update required for {incompatibleEnabledMods.length} enabled mod(s):
				{incompatibleEnabledMods
					.map((m) => `${m.name} (${(m.missingRequiredOffsets || []).join(', ')})`)
					.join(' · ')}
			</span>
			<div class="post-crash-actions">
				<Button
					size="sm"
					disabled={app.updatingOffsets}
					onclick={() => app.updateOffsetsOta()}
				>
					{app.updatingOffsets ? 'Checking...' : 'Check GitHub Offset Updates'}
				</Button>
				<Button
					variant="primary"
					size="sm"
					disabled={app.launchLocked}
					onclick={() =>
						app.launchWrsr(
							null,
							incompatibleEnabledMods.map((m) => m.id)
						)}
				>
					Temporarily Disable Affected Mods &amp; Play
				</Button>
				<Button size="sm" onclick={() => (offsetBannerDismissed = true)}>Dismiss</Button>
			</div>
		</div>
	{/if}

	{#if app.crashBannerVisible && app.lastCrashSaveName}
		<div class="post-crash-banner">
			<span>{t('header.crashBannerText', { save: app.lastCrashSaveName })}</span>
			<div class="post-crash-actions">
				<Button variant="primary" size="sm" onclick={inspectCrashSave}>{t('header.inspectSave')}</Button>
				<Button size="sm" onclick={() => (app.crashBannerVisible = false)}>{t('common.dismiss')}</Button>
			</div>
		</div>
	{/if}

	<div class="main-grid">
		<aside class="sidebar">
			<button
				class="nav-btn"
				class:active={app.activeView === 'mods'}
				onclick={() => app.switchView('mods')}
			>
				<span>{t('nav.mods')}</span>
				<span class="count-pill">{enabledCount}/{totalCount}</span>
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'playlists'}
				onclick={() => app.switchView('playlists')}
			>
				{t('nav.playlists')}
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'overrides'}
				onclick={() => app.switchView('overrides')}
			>
				{t('nav.overrides')}
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'saves'}
				onclick={() => app.switchView('saves')}
			>
				{t('nav.saves')}
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'offsets'}
				onclick={() => app.switchView('offsets')}
			>
				<span>{t('nav.offsets')}</span>
				{#if totalOffsets > 0}
					<span class="count-pill">{okOffsets}/{totalOffsets}</span>
				{/if}
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'diagnostics'}
				onclick={() => app.switchView('diagnostics')}
			>
				{t('nav.diagnostics')}
			</button>
			<button
				class="nav-btn"
				class:active={app.activeView === 'schema-help'}
				onclick={() => app.switchView('schema-help')}
			>
				{t('nav.schemaHelp')}
			</button>

			<div class="sidebar-footer">
				<button
					class="nav-btn"
					class:active={app.activeView === 'settings'}
					onclick={() => app.switchView('settings')}
				>
					{t('nav.settings')}
				</button>
				<div class="status-box">{app.statusToast || t('common.ready')}</div>
			</div>
		</aside>

		<main class="content">
			<section class="view active">
				{#if app.activeView === 'playlists'}
					<PlaylistsView />
				{:else if app.activeView === 'mods'}
					<ModsView />
				{:else if app.activeView === 'overrides'}
					<OverridesView />
				{:else if app.activeView === 'saves'}
					<SaveInspectorView />
				{:else if app.activeView === 'offsets'}
					<OffsetsView />
				{:else if app.activeView === 'diagnostics'}
					<DiagnosticsView />
				{:else if app.activeView === 'schema-help'}
					<SchemaHelpView />
				{:else if app.activeView === 'settings'}
					<SettingsView />
				{/if}
			</section>
		</main>
	</div>

	{#if app.launching && app.backupProgress}
		{@const bp = app.backupProgress}
		<div class="backup-modal-backdrop" role="dialog" aria-modal="true" aria-label={t('backupModal.title')}>
			<div class="backup-modal-card">
				<div class="backup-modal-header">
					<span class="backup-modal-title">{t('backupModal.title')}</span>
					{#if bp.phase === 'copying' || bp.phase === 'pruning' || bp.phase === 'done'}
						<span class="backup-modal-pct">{backupPercent}%</span>
					{/if}
				</div>
				<div class="backup-modal-status">
					{#if bp.phase === 'scanning'}
						{t('backupModal.scanning')}
					{:else if bp.phase === 'copying'}
						{t('backupModal.copying')}
					{:else if bp.phase === 'pruning'}
						{t('backupModal.pruning')}
					{:else}
						{t('backupModal.preparing')}
					{/if}
				</div>
				<div class="backup-progress-track">
					{#if bp.phase === 'scanning'}
						<div class="backup-progress-fill indeterminate"></div>
					{:else}
						<div class="backup-progress-fill" style="width: {backupPercent}%"></div>
					{/if}
				</div>
				{#if bp.totalFiles > 0}
					<div class="backup-modal-meta">
						<span>
							{t('backupModal.filesProgress', {
								done: bp.completedFiles,
								total: bp.totalFiles
							})}
						</span>
						<span>{formatBytes(bp.copiedBytes)} / {formatBytes(bp.totalBytes)}</span>
					</div>
				{/if}
				{#if bp.currentItem}
					<div class="backup-modal-file" title={bp.currentItem}>{bp.currentItem}</div>
				{/if}
			</div>
		</div>
	{/if}
</div>

<style>
	.logo-flip-btn {
		width: 36px;
		height: 36px;
		padding: 0;
		border: none;
		background: transparent;
		cursor: pointer;
		perspective: 420px;
		flex-shrink: 0;
	}
	.logo-flip-inner {
		position: relative;
		display: block;
		width: 100%;
		height: 100%;
		transform-style: preserve-3d;
		transition: transform 0.55s cubic-bezier(0.22, 1, 0.36, 1);
	}
	.logo-flip-btn.flipped .logo-flip-inner {
		transform: rotateY(180deg);
	}
	.logo-face {
		position: absolute;
		inset: 0;
		width: 36px;
		height: 36px;
		border-radius: 6px;
		border: 1px solid var(--gold);
		overflow: hidden;
		backface-visibility: hidden;
		box-shadow: 0 2px 8px rgba(0, 0, 0, 0.45);
	}
	.logo-front {
		object-fit: cover;
		background: #141518;
	}
	.logo-back {
		transform: rotateY(180deg);
		display: flex;
		align-items: center;
		justify-content: center;
		background: #a81c18;
	}
	.backup-modal-backdrop {
		position: fixed;
		inset: 0;
		z-index: 9999;
		background: rgba(10, 11, 14, 0.76);
		backdrop-filter: blur(4px);
		display: flex;
		align-items: center;
		justify-content: center;
		padding: 20px;
	}
	.backup-modal-card {
		width: min(460px, 100%);
		background: #17191e;
		border: 1px solid rgba(214, 166, 69, 0.45);
		border-radius: 10px;
		padding: 18px 20px;
		box-shadow: 0 18px 48px rgba(0, 0, 0, 0.6);
		display: flex;
		flex-direction: column;
		gap: 10px;
	}
	.backup-modal-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 12px;
	}
	.backup-modal-title {
		font-size: 15px;
		font-weight: 700;
		color: #f3eee2;
	}
	.backup-modal-pct {
		font-size: 13px;
		font-weight: 700;
		color: var(--gold);
		font-variant-numeric: tabular-nums;
	}
	.backup-modal-status {
		font-size: 13px;
		color: rgba(243, 238, 226, 0.82);
	}
	.backup-progress-track {
		width: 100%;
		height: 8px;
		border-radius: 999px;
		background: rgba(255, 255, 255, 0.08);
		overflow: hidden;
	}
	.backup-progress-fill {
		height: 100%;
		background: linear-gradient(90deg, #b8882d, #e4b85c);
		border-radius: 999px;
		transition: width 0.12s ease-out;
	}
	.backup-progress-fill.indeterminate {
		width: 40%;
		animation: backup-indeterminate 1.1s infinite ease-in-out;
	}
	@keyframes backup-indeterminate {
		0% { transform: translateX(-110%); }
		100% { transform: translateX(260%); }
	}
	.backup-modal-meta {
		display: flex;
		align-items: center;
		justify-content: space-between;
		font-size: 12px;
		color: rgba(243, 238, 226, 0.65);
		font-variant-numeric: tabular-nums;
	}
	.backup-modal-file {
		font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
		font-size: 11px;
		color: rgba(243, 238, 226, 0.55);
		white-space: nowrap;
		overflow: hidden;
		text-overflow: ellipsis;
	}
</style>
