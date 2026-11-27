<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { t } from '$lib/i18n/index.svelte';
	import { openExternalUrl } from '$lib/utils';
	import {
		Badge,
		Button,
		EmptyState,
		PanelCard,
		SectionHeader,
		SplitLayout,
		TextInput,
		ToggleSwitch
	} from '$lib/components/ui';

	let searchQuery = $state('');
	let missingOnly = $state(false);

	const filteredSaves = $derived.by(() => {
		const q = searchQuery.trim().toLowerCase();
		return (app.savesData?.saves || []).filter((s) => {
			if (missingOnly && (s.missingWorkshopIds || []).length === 0) return false;
			if (q && !s.name.toLowerCase().includes(q)) return false;
			return true;
		});
	});

	const selectedSave = $derived(
		(app.savesData?.saves || []).find((s) => s.name === app.selectedSaveName) ?? null
	);

	const modNameByWsId = $derived.by(() => {
		const map = new Map<string, string>();
		for (const m of app.discovery?.mods || []) {
			if (m.workshopId) map.set(String(m.workshopId), m.name);
		}
		return map;
	});

	async function copyMissing(ids: string[]) {
		await navigator.clipboard.writeText(ids.join('\n'));
		app.setStatus(t('saves.copiedMissingStatus', { count: ids.length }));
	}
</script>

<SplitLayout>
	{#snippet toolbar()}
		<TextInput bind:value={searchQuery} placeholder={t('saves.searchPlaceholder')} />
		<ToggleSwitch bind:checked={missingOnly} label={t('saves.missingOnly')} />
		<Button size="sm" onclick={() => app.fetchSaves(app.selectedSaveName)}>{t('saves.refreshBtn')}</Button>
	{/snippet}

	{#snippet list()}
		{#if app.savesLoading}
			<EmptyState message={t('saves.scanning')} />
		{:else if !app.savesData}
			<EmptyState>
				{@html t('saves.emptyPrompt')}
			</EmptyState>
		{:else if !app.savesData.ok}
			<EmptyState message={app.savesData.error || t('saves.scanFailed')} />
		{:else if filteredSaves.length === 0}
			<EmptyState message={t('saves.noMatching')} />
		{:else}
			{#each filteredSaves as save (save.name)}
				{@const missingCount = (save.missingWorkshopIds || []).length}
				{@const errCount = (save.errors || []).length}
				<!-- svelte-ignore a11y_click_events_have_key_events -->
				<!-- svelte-ignore a11y_no_static_element_interactions -->
				<div
					class="save-row"
					class:selected={save.name === app.selectedSaveName}
					onclick={() => (app.selectedSaveName = save.name)}
				>
					<div class="save-row-main">
						<div class="mod-title">{save.name}</div>
						<div class="mod-sub">
							{save.lastModifiedDisplay || t('saves.unknownTime')} · {t('saves.workshopItemsCount', { count: (save.referencedWorkshopIds || []).length })}
						</div>
					</div>
					<Badge variant={missingCount > 0 || errCount > 0 ? 'warn' : 'ready'}>
						{save.status}
					</Badge>
				</div>
			{/each}
		{/if}
	{/snippet}

	{#snippet inspector()}
		{#if !selectedSave}
			<EmptyState message={t('saves.emptySelection')} />
		{:else}
			{@const refs = selectedSave.referencedWorkshopIds || []}
			{@const missing = selectedSave.missingWorkshopIds || []}
			{@const installed = selectedSave.installedMods || []}
			{@const errors = selectedSave.errors || []}
			{@const installedCount = refs.length - missing.length}
			{@const hasIssue = errors.length > 0 || missing.length > 0}
			{@const sizeMb = selectedSave.totalBytes
				? `${(selectedSave.totalBytes / (1024 * 1024)).toFixed(1)} MB`
				: '—'}
			{@const inGameDate =
				selectedSave.inGameYear && selectedSave.inGameYear > 0
					? t('saves.inGameDateValue', { year: selectedSave.inGameYear, day: selectedSave.inGameDay || 0 })
					: ''}

			<div class="insp-header">
				<div style="display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;">
					<h2 style="margin:0;">
						{selectedSave.displayName || selectedSave.name}
						<Badge variant={hasIssue ? 'warn' : 'ready'}>{selectedSave.status}</Badge>
					</h2>
					<Button
						variant="primary"
						size="sm"
						disabled={app.launchLocked}
						onclick={() => app.launchWrsr(selectedSave.name)}
						title={t('saves.launchSaveTip', { save: selectedSave.name })}
					>
						{t('saves.launchSaveBtn')}
					</Button>
				</div>
				<div class="insp-subline">
					<span>{t('saves.folderLabel', { name: selectedSave.name })}</span>
					<span>·</span>
					<span>{t('saves.lastPlayedLabel', { time: selectedSave.lastModifiedDisplay || t('common.unknown') })}</span>
					<span class="insp-link-chip" title={selectedSave.path || ''}>{t('saves.pathChip')}</span>
				</div>
			</div>

			{#if errors.length > 0}
				<div style="margin-bottom:12px;">
					<PanelCard tone="danger">
						<div class="control-label" style="color:var(--danger);">{t('saves.integrityIssues')}</div>
						<div class="insp-meta" style="color:var(--text);">{errors.join(' · ')}</div>
					</PanelCard>
				</div>
			{/if}

			{#if missing.length > 0}
				<SectionHeader title={t('saves.missingItemsHeader', { count: missing.length })} tone="danger">
					{#snippet actions()}
						<Button variant="primary" size="sm" onclick={() => copyMissing(missing)}>
							{t('saves.copyMissingIds')}
						</Button>
					{/snippet}
				</SectionHeader>
				<div class="schema-form" style="margin-bottom:14px;">
					{#each missing as id (id)}
						<div class="missing-ws-row">
							<strong style="font-family:var(--mono);font-size:12px;">{id}</strong>
							<div class="missing-ws-actions">
								<Button
									size="sm"
									onclick={() => openExternalUrl(`steam://url/CommunityFilePage/${encodeURIComponent(id)}`)}
								>
									{t('saves.steamClient')}
								</Button>
								<Button
									size="sm"
									onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(id)}`)}
								>
									{t('saves.webPage')}
								</Button>
							</div>
						</div>
					{/each}
				</div>
			{/if}

			<div class="save-details-grid">
				{#if inGameDate}
					<div class="save-detail-card">
						<div class="save-detail-label">{t('saves.inGameDateLabel')}</div>
						<div class="save-detail-val">{inGameDate}</div>
					</div>
				{/if}
				<div class="save-detail-card">
					<div class="save-detail-label">{t('saves.workshopModsLabel')}</div>
					<div class="save-detail-val">{t('saves.installedOfTotal', { installed: installedCount, total: refs.length })}</div>
				</div>
				<div class="save-detail-card">
					<div class="save-detail-label">{t('saves.saveSizeLabel')}</div>
					<div class="save-detail-val">{t('saves.saveSizeValue', { size: sizeMb, files: selectedSave.fileCount || 0 })}</div>
				</div>
				{#if selectedSave.worldSize}
					<div class="save-detail-card">
						<div class="save-detail-label">{t('saves.mapSizeLabel')}</div>
						<div class="save-detail-val">{selectedSave.worldSize}</div>
					</div>
				{/if}
				{#if selectedSave.originalMapName}
					<div class="save-detail-card">
						<div class="save-detail-label">{t('saves.mapTemplateLabel')}</div>
						<div class="save-detail-val">{selectedSave.originalMapName}</div>
					</div>
				{/if}
				{#if selectedSave.headerVersion}
					<div class="save-detail-card">
						<div class="save-detail-label">{t('saves.headerFormatLabel')}</div>
						<div class="save-detail-val">v{selectedSave.headerVersion}</div>
					</div>
				{/if}
			</div>

			<SectionHeader title={t('saves.installedItemsHeader', { count: installed.length })} />
			{#if installed.length === 0}
				<div class="insp-meta">
					{refs.length === 0
						? t('saves.noRefsInHeader')
						: t('saves.noRefsInstalled')}
				</div>
			{:else}
				<div class="schema-form" style="margin-top:8px;">
					{#each installed as im (im.id)}
						{@const knownName = modNameByWsId.get(String(im.id))}
						{@const assetsText = (im.assets || []).length > 0 ? t('saves.assetsPrefix', { assets: im.assets.join(', ') }) : ''}
						{@const metaParts = [im.itemType || t('saves.workshopItemFallback'), assetsText].filter(Boolean)}
						<div class="missing-ws-row">
							<div style="min-width:0;">
								<div style="font-size:12px;font-weight:600;color:var(--text);">
									{#if knownName}{knownName} {/if}
									<span style="font-family:var(--mono);color:var(--muted);">{im.id}</span>
								</div>
								<div class="insp-meta" style="margin-top:2px;">{metaParts.join(' · ')}</div>
							</div>
							<div class="missing-ws-actions">
								<Button
									size="sm"
									onclick={() => openExternalUrl(`steam://url/CommunityFilePage/${encodeURIComponent(im.id)}`)}
								>
									{t('saves.steamShort')}
								</Button>
								<Button
									size="sm"
									onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(im.id)}`)}
								>
									{t('saves.webShort')}
								</Button>
							</div>
						</div>
					{/each}
				</div>
			{/if}
		{/if}
	{/snippet}
</SplitLayout>
