<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { i18n, t } from '$lib/i18n/index.svelte';
	import { openExternalUrl } from '$lib/utils';
	import { Badge, Button, PanelCard, SectionHeader, Select, TextInput, ToggleSwitch } from '$lib/components/ui';

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

	async function onDevPathsChange() {
		app.savesData = null;
		await app.saveGrmlIni();
		await app.fetchState();
		await app.fetchSaves();
	}

	async function onDevModeToggle() {
		if (app.chkDevMode) {
			const sd = app.discovery?.steamDefaults;
			if (sd) {
				if (!app.devSavePath.trim() && sd.saveRoot) app.devSavePath = sd.saveRoot;
				if (!app.devWorkshopPath.trim() && sd.workshopRoot) app.devWorkshopPath = sd.workshopRoot;
				if (!app.devWorkshopWipPath.trim() && sd.workshopWipRoot) app.devWorkshopWipPath = sd.workshopWipRoot;
				if (!app.devOverridesPath.trim() && sd.overridesRoot) app.devOverridesPath = sd.overridesRoot;
			}
		}
		await onDevPathsChange();
	}

	async function fillSteamDefaults() {
		const sd = app.discovery?.steamDefaults;
		if (!sd) return;
		if (sd.saveRoot) app.devSavePath = sd.saveRoot;
		if (sd.workshopRoot) app.devWorkshopPath = sd.workshopRoot;
		if (sd.workshopWipRoot) app.devWorkshopWipPath = sd.workshopWipRoot;
		if (sd.overridesRoot) app.devOverridesPath = sd.overridesRoot;
		await onDevPathsChange();
	}

	async function browseBackupFolder() {
		const resp = await fetch(`/api/browse-folder?current=${encodeURIComponent(app.saveBackupPath.trim())}`);
		const data = await resp.json();
		if (data.ok && data.path) {
			app.saveBackupPath = data.path;
			await app.saveGrmlIni();
		}
	}

	async function onMaxBackupsChange() {
		const n = Math.round(Number(app.maxSaveBackups) || 3);
		app.maxSaveBackups = Math.max(1, Math.min(50, n));
		await app.saveGrmlIni();
	}

	async function browseDevFolder(field: 'save' | 'workshop' | 'wip' | 'overrides') {
		const current =
			field === 'save'
				? app.devSavePath
				: field === 'workshop'
					? app.devWorkshopPath
					: field === 'wip'
						? app.devWorkshopWipPath
						: app.devOverridesPath;
		const resp = await fetch(`/api/browse-folder?current=${encodeURIComponent(current.trim())}`);
		const data = await resp.json();
		if (data.ok && data.path) {
			if (field === 'save') app.devSavePath = data.path;
			else if (field === 'workshop') app.devWorkshopPath = data.path;
			else if (field === 'wip') app.devWorkshopWipPath = data.path;
			else app.devOverridesPath = data.path;
			await onDevPathsChange();
		}
	}
</script>

<div class="panel">
	<h2>{t('settings.title')}</h2>
	<p class="muted" style="margin:0 0 14px 0;">
		{@html t('settings.subtitle')}
	</p>

	<div class="schema-form">
		<SectionHeader title={t('settings.sectionLauncher')} />

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

		<SectionHeader title={t('settings.sectionGameDiscovery')} />

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

		<SectionHeader title={t('settings.sectionInGame')} />

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

		<SectionHeader title={t('settings.sectionSaveBackups')} />

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.autoBackupLabel')}</div>
				<ToggleSwitch bind:checked={app.chkAutoBackupSaves} onchange={() => app.saveGrmlIni()} />
			</div>
			<div class="control-tip">
				{@html t('settings.autoBackupTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.maxBackupsLabel')}</div>
				<input
					type="number"
					min="1"
					max="50"
					class="backup-count-input"
					bind:value={app.maxSaveBackups}
					disabled={!app.chkAutoBackupSaves}
					onchange={onMaxBackupsChange}
				/>
			</div>
			<div class="control-tip">
				{@html t('settings.maxBackupsTip')}
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">{t('settings.backupPathLabel')}</div>
			</div>
			<div class="control-tip">
				{@html t('settings.backupPathTip')}
			</div>
			<div class="control-input-row">
				<TextInput
					bind:value={app.saveBackupPath}
					mono={true}
					disabled={!app.chkAutoBackupSaves}
					placeholder={t('settings.backupPathPlaceholder')}
					onchange={() => app.saveGrmlIni()}
				/>
				<Button disabled={!app.chkAutoBackupSaves} onclick={browseBackupFolder}>{t('settings.browseBtn')}</Button>
			</div>
		</PanelCard>

		<SectionHeader title={t('settings.sectionDeveloper')} />

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
					{t('settings.mcpLabel')}
					{#if app.mcpLiveEnabled && app.mcpLiveUrl}
						<Badge variant="ready">{t('settings.mcpActiveBadge')}</Badge>
					{:else}
						<Badge variant="default">{t('settings.mcpDisabledBadge')}</Badge>
					{/if}
				</div>
				<ToggleSwitch bind:checked={app.chkMcpEnabled} onchange={() => app.saveGrmlIni()} />
			</div>
			<div class="control-tip">
				{@html t('settings.mcpTip')}
			</div>
			{#if app.chkMcpEnabled}
				<div class="control-input-row" style="margin-top:8px;">
					<input
						type="number"
						class="ui-input mono"
						style="width:110px;flex:0 0 110px;"
						min="1024"
						max="65535"
						bind:value={app.mcpPort}
						onchange={() => app.saveGrmlIni()}
					/>
					<TextInput
						value={app.mcpLiveUrl || `http://127.0.0.1:${app.mcpPort}/mcp`}
						mono={true}
						readonly={true}
					/>
					<Button
						size="sm"
						onclick={async () => {
							const url = app.mcpLiveUrl || `http://127.0.0.1:${app.mcpPort}/mcp`;
							const snippet = JSON.stringify({ mcpServers: { grml: { url } } }, null, 2);
							await navigator.clipboard.writeText(snippet);
							app.setStatus(t('settings.mcpCopiedStatus'));
						}}
					>
						{t('settings.mcpCopyConfigBtn')}
					</Button>
				</div>
			{/if}
		</PanelCard>

		{#if app.discovery?.showDevSettings}
		<PanelCard>
			<div class="control-top">
				<div class="control-label">
					{t('settings.devModeLabel')}
					{#if app.chkDevMode}
						<Badge variant="grml">{t('settings.devModeActiveBadge')}</Badge>
					{/if}
				</div>
				<ToggleSwitch bind:checked={app.chkDevMode} onchange={onDevModeToggle} />
			</div>
			<div class="control-tip">
				{@html t('settings.devModeTip')}
			</div>
			{#if app.chkDevMode}
				<div class="dev-paths-grid">
					<div class="dev-path-field" class:dev-path-inactive={!app.chkDevSave}>
						<div class="dev-path-header">
							<div class="control-tip" style="margin:0;">{@html t('settings.devSavePathLabel')}</div>
							<ToggleSwitch bind:checked={app.chkDevSave} onchange={onDevPathsChange} />
						</div>
						<div class="control-input-row">
							<TextInput
								bind:value={app.devSavePath}
								mono={true}
								disabled={!app.chkDevSave}
								placeholder={app.discovery?.steamDefaults?.saveRoot || t('settings.devPathPlaceholder')}
								onchange={onDevPathsChange}
							/>
							<Button size="sm" disabled={!app.chkDevSave} onclick={() => browseDevFolder('save')}>{t('settings.browseBtn')}</Button>
						</div>
					</div>
					<div class="dev-path-field" class:dev-path-inactive={!app.chkDevWorkshop}>
						<div class="dev-path-header">
							<div class="control-tip" style="margin:0;">{@html t('settings.devWorkshopPathLabel')}</div>
							<ToggleSwitch bind:checked={app.chkDevWorkshop} onchange={onDevPathsChange} />
						</div>
						<div class="control-input-row">
							<TextInput
								bind:value={app.devWorkshopPath}
								mono={true}
								disabled={!app.chkDevWorkshop}
								placeholder={app.discovery?.steamDefaults?.workshopRoot || t('settings.devPathPlaceholder')}
								onchange={onDevPathsChange}
							/>
							<Button size="sm" disabled={!app.chkDevWorkshop} onclick={() => browseDevFolder('workshop')}>{t('settings.browseBtn')}</Button>
						</div>
					</div>
					<div class="dev-path-field" class:dev-path-inactive={!app.chkDevWip}>
						<div class="dev-path-header">
							<div class="control-tip" style="margin:0;">{@html t('settings.devWipPathLabel')}</div>
							<ToggleSwitch bind:checked={app.chkDevWip} onchange={onDevPathsChange} />
						</div>
						<div class="control-input-row">
							<TextInput
								bind:value={app.devWorkshopWipPath}
								mono={true}
								disabled={!app.chkDevWip}
								placeholder={app.discovery?.steamDefaults?.workshopWipRoot || t('settings.devPathPlaceholder')}
								onchange={onDevPathsChange}
							/>
							<Button size="sm" disabled={!app.chkDevWip} onclick={() => browseDevFolder('wip')}>{t('settings.browseBtn')}</Button>
						</div>
					</div>
					<div class="dev-path-field" class:dev-path-inactive={!app.chkDevOverrides}>
						<div class="dev-path-header">
							<div class="control-tip" style="margin:0;">{@html t('settings.devOverridesPathLabel')}</div>
							<ToggleSwitch bind:checked={app.chkDevOverrides} onchange={onDevPathsChange} />
						</div>
						<div class="control-input-row">
							<TextInput
								bind:value={app.devOverridesPath}
								mono={true}
								disabled={!app.chkDevOverrides}
								placeholder={app.discovery?.steamDefaults?.overridesRoot || t('settings.devPathPlaceholder')}
								onchange={onDevPathsChange}
							/>
							<Button size="sm" disabled={!app.chkDevOverrides} onclick={() => browseDevFolder('overrides')}>{t('settings.browseBtn')}</Button>
						</div>
					</div>
					<div style="display:flex;justify-content:flex-end;">
						<Button size="sm" onclick={fillSteamDefaults}>{t('settings.fillSteamDefaultsBtn')}</Button>
					</div>
				</div>
			{/if}
		</PanelCard>
		{/if}
	</div>
</div>

<style>
	.dev-paths-grid {
		display: flex;
		flex-direction: column;
		gap: 10px;
		margin-top: 12px;
		padding-top: 10px;
		border-top: 1px solid rgba(255, 255, 255, 0.08);
	}
	.dev-path-field {
		display: flex;
		flex-direction: column;
		gap: 4px;
	}
	.dev-path-header {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 10px;
	}
	.dev-path-inactive {
		opacity: 0.6;
	}
	.backup-count-input {
		width: 78px;
		padding: 5px 8px;
		border-radius: 6px;
		border: 1px solid rgba(255, 255, 255, 0.14);
		background: rgba(15, 16, 18, 0.85);
		color: inherit;
		font-family: inherit;
		font-size: 13px;
		text-align: center;
	}
	.backup-count-input:disabled {
		opacity: 0.5;
	}
</style>
