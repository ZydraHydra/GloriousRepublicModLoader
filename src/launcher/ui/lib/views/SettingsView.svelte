<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { i18n, t } from '$lib/i18n/index.svelte';
	import { openExternalUrl } from '$lib/utils';
	import { Badge, Button, PanelCard, Select, TextInput, ToggleSwitch } from '$lib/components/ui';

	let listeningHotkey = $state(false);

	const languageOptions = $derived.by(() => {
		const detected = i18n.detectedGameLocale;
		const autoLabel = detected
			? t('settings.languageAutoDetected', { code: detected })
			: t('settings.languageAuto');
		return [
			{ value: 'auto', label: autoLabel },
			...i18n.available.map((l) => ({
				value: l.code,
				label: l.nativeName && l.nativeName !== l.name ? `${l.nativeName} (${l.name})` : l.name
			}))
		];
	});

	async function onLanguageChange(code: string) {
		await i18n.setLocale(code);
		await app.saveGrmlIni();
		await app.fetchState();
		if (i18n.setting === 'auto') {
			app.setStatus(
				t('settings.languageChangedStatus', {
					language: i18n.detectedGameLocale
						? t('settings.languageAutoDetected', { code: i18n.detectedGameLocale })
						: t('settings.languageAuto')
				})
			);
			return;
		}
		const chosen = i18n.available.find((l) => l.code === i18n.locale);
		app.setStatus(
			t('settings.languageChangedStatus', {
				language: chosen?.nativeName || chosen?.name || i18n.locale
			})
		);
	}

	const hotkeyPresets = [
		'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12',
		'Insert', 'Home', 'End', 'PageUp', 'PageDown', 'Pause', 'ScrollLock'
	];

	const hotkeyOptions = $derived.by(() => {
		const opts = hotkeyPresets.map((k) => ({ value: k, label: k }));
		if (app.overlayHotkey && !hotkeyPresets.includes(app.overlayHotkey)) {
			opts.push({ value: app.overlayHotkey, label: app.overlayHotkey });
		}
		return opts;
	});

	function mapEventToHotkey(ev: KeyboardEvent): string | null {
		if (ev.key === 'Escape') return null;
		if (/^F([1-9]|1[0-2])$/.test(ev.key)) return ev.key;
		if (ev.key === 'Insert') return 'Insert';
		if (ev.key === 'Home') return 'Home';
		if (ev.key === 'End') return 'End';
		if (ev.key === 'PageUp') return 'PageUp';
		if (ev.key === 'PageDown') return 'PageDown';
		if (ev.key === 'Pause') return 'Pause';
		if (ev.key === 'ScrollLock') return 'ScrollLock';
		if (ev.key === '`' || ev.key === '~') return 'Tilde';
		if (/^[a-zA-Z0-9]$/.test(ev.key)) return ev.key.toUpperCase();
		return null;
	}

	function startRebind() {
		listeningHotkey = true;
		const onKeyDown = async (ev: KeyboardEvent) => {
			if (['Shift', 'Control', 'Alt', 'Meta'].includes(ev.key)) return;
			ev.preventDefault();
			ev.stopPropagation();
			window.removeEventListener('keydown', onKeyDown, true);
			listeningHotkey = false;
			const mapped = mapEventToHotkey(ev);
			if (mapped) {
				app.overlayHotkey = mapped;
				await app.saveGrmlIni();
				app.setStatus(t('settings.hotkeySetStatus', { key: mapped }));
			}
		};
		window.addEventListener('keydown', onKeyDown, true);
	}

	const verLabel = $derived(
		app.discovery && app.discovery.gameVersion !== 'unknown'
			? t('settings.wrsrVersion', { version: app.discovery.gameVersion })
			: t('settings.wrsrUnrecognized')
	);

	async function browseGame() {
		const resp = await fetch(`/api/browse-game?current=${encodeURIComponent(app.gameExeInput.trim())}`);
		const data = await resp.json();
		if (data.ok && data.path) {
			await app.reloadForGameExe(data.path);
			app.setStatus(t('settings.gameExeSetStatus', { path: data.path }));
		}
	}

	async function onPathChange() {
		await app.reloadForGameExe();
	}

	async function onDiscoveryToggle() {
		await app.saveGrmlIni();
		await app.fetchState();
	}
</script>

<div class="panel">
	<h2>{t('settings.title')}</h2>
	<p class="muted" style="margin:0 0 14px 0;">
		{@html t('settings.subtitle')}
	</p>

	<div class="schema-form">
		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.languageLabel')}</div>
				<Select
					value={i18n.setting}
					options={languageOptions}
					onchange={onLanguageChange}
				/>
			</div>
			<div class="control-tip">
				{@html t('settings.languageTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{@html t('settings.gameExeLabel')}</div>
				<span class="pill">{verLabel}</span>
			</div>
			<div class="control-tip">
				{@html t('settings.gameExeTip')}
			</div>
			<div class="control-input-row">
				<TextInput
					bind:value={app.gameExeInput}
					mono={true}
					placeholder={t('settings.gameExePlaceholder')}
					onchange={onPathChange}
				/>
				<Button onclick={browseGame}>{t('settings.setPathBtn')}</Button>
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.scanWorkshopLabel')}</div>
				<ToggleSwitch bind:checked={app.chkWorkshop} onchange={onDiscoveryToggle} />
			</div>
			<div class="control-tip">
				{t('settings.scanWorkshopTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{@html t('settings.scanWipLabel')}</div>
				<ToggleSwitch bind:checked={app.chkWip} onchange={onDiscoveryToggle} />
			</div>
			<div class="control-tip">
				{@html t('settings.scanWipTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.vfsLabel')}</div>
				<ToggleSwitch bind:checked={app.chkVfsEnabled} onchange={() => app.saveVfsConfig()} />
			</div>
			<div class="control-tip">
				{@html t('settings.vfsTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.hotkeyLabel')}</div>
				<div style="display:flex;align-items:center;gap:8px;">
					<Select
						bind:value={app.overlayHotkey}
						options={hotkeyOptions}
						onchange={() => app.saveGrmlIni()}
					/>
					<Button
						variant={listeningHotkey ? 'primary' : 'default'}
						size="sm"
						onclick={startRebind}
					>
						{listeningHotkey ? t('settings.pressAnyKey') : t('settings.rebindKey')}
					</Button>
				</div>
			</div>
			<div class="control-tip">
				{@html t('settings.hotkeyTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.menuTagLabel')}</div>
				<ToggleSwitch bind:checked={app.chkMenuStatus} onchange={() => app.saveGrmlIni()} />
			</div>
			<div class="control-tip">
				{@html t('settings.menuTagTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.skipIntroLabel')}</div>
				<ToggleSwitch bind:checked={app.chkSkipIntro} onchange={() => app.saveGrmlIni()} />
			</div>
			<div class="control-tip">
				{@html t('settings.skipIntroTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">
					{@html t('settings.offsetsLabel')}
					{#if app.discovery?.offsetsHash}
						<span class="muted" style="font-weight:400;margin-left:6px;">{app.discovery.offsetsHash}</span>
					{/if}
				</div>
				<div style="display:flex;align-items:center;gap:8px;">
					{#if app.offsetsOtaResult}
						{#if app.offsetsOtaResult.ok}
							<Badge variant={app.offsetsOtaResult.updated ? 'grml' : 'ready'}>
								{app.offsetsOtaResult.updated ? t('settings.offsetsUpdatedBadge') : t('settings.offsetsUpToDateBadge')}
							</Badge>
						{:else}
							<Badge variant="warn">{app.offsetsOtaResult.error || t('settings.offsetsOtaFailedBadge')}</Badge>
						{/if}
					{/if}
					<Button size="sm" disabled={app.updatingOffsets} onclick={() => app.updateOffsetsOta()}>
						{app.updatingOffsets ? t('common.updating') : t('settings.updateOffsetsBtn')}
					</Button>
				</div>
			</div>
			<div class="control-tip">
				{@html t('settings.offsetsTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">
					{t('settings.updatesLabel')}
					{#if app.discovery?.loaderVersion}
						<span class="muted" style="font-weight:400;margin-left:6px;">v{app.discovery.loaderVersion}</span>
					{/if}
				</div>
				<div style="display:flex;align-items:center;gap:8px;">
					{#if app.updateInfo?.ok}
						{#if app.updateInfo.updateAvailable}
							<Badge variant="grml">{t('settings.updateBadge', { version: app.updateInfo.latestVersion ?? '' })}</Badge>
							{#if app.updateInfo.releaseUrl}
								<Button
									variant="primary"
									size="sm"
									onclick={() => openExternalUrl(app.updateInfo!.releaseUrl!)}
								>
									{t('settings.downloadBtn')}
								</Button>
							{/if}
						{:else}
							<Badge variant="ready">{t('settings.upToDateBadge')}</Badge>
						{/if}
					{:else if app.updateInfo && !app.updateInfo.ok}
						<Badge variant="warn">{app.updateInfo.error || t('settings.checkFailedBadge')}</Badge>
					{/if}
					<Button size="sm" disabled={app.checkingUpdate} onclick={() => app.checkForUpdates(true)}>
						{app.checkingUpdate ? t('common.checking') : t('settings.checkUpdatesBtn')}
					</Button>
				</div>
			</div>
			<div class="control-tip">
				{@html t('settings.updatesTip')}
			</div>
		</PanelCard>
	</div>
</div>
