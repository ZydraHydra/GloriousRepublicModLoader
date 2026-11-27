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
	import DiagnosticsView from '$lib/views/DiagnosticsView.svelte';
	import SchemaHelpView from '$lib/views/SchemaHelpView.svelte';
	import SettingsView from '$lib/views/SettingsView.svelte';

	const enabledCount = $derived(
		(app.discovery?.mods || []).filter((m) => m.enabled && !m.blocked).length
	);
	const totalCount = $derived((app.discovery?.mods || []).length);
	const verLabel = $derived(
		app.discovery && app.discovery.gameVersion !== 'unknown'
			? t('header.wrsrVersion', { version: app.discovery.gameVersion })
			: t('header.wrsrDetecting')
	);

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
				class:active={app.activeView === 'playlists'}
				onclick={() => app.switchView('playlists')}
			>
				{t('nav.playlists')}
			</button>
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
</style>
