<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
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
		app.setStatus(`Copied ${ids.length} missing Workshop ID(s) to clipboard.`);
	}
</script>

<SplitLayout>
	{#snippet toolbar()}
		<TextInput bind:value={searchQuery} placeholder="Search saves by name..." />
		<ToggleSwitch bind:checked={missingOnly} label="Missing Mods Only" />
		<Button size="sm" onclick={() => app.fetchSaves(app.selectedSaveName)}>↻ Refresh Saves</Button>
	{/snippet}

	{#snippet list()}
		{#if app.savesLoading}
			<EmptyState message="Scanning saves & Workshop items..." />
		{:else if !app.savesData}
			<EmptyState>
				Open this tab or click <strong>↻ Refresh Saves</strong> to scan <code>media_soviet\save</code>.
			</EmptyState>
		{:else if !app.savesData.ok}
			<EmptyState message={app.savesData.error || 'Failed to scan saves.'} />
		{:else if filteredSaves.length === 0}
			<EmptyState message="No matching saves found." />
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
							{save.lastModifiedDisplay || 'Unknown time'} · {(save.referencedWorkshopIds || []).length} Workshop item(s)
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
			<EmptyState message="Select a save to inspect referenced Workshop IDs and missing items." />
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
					? `${selectedSave.inGameYear} (Day ${selectedSave.inGameDay || 0})`
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
						onclick={() => app.launchWrsr(selectedSave.name)}
						title={`Launch WRSR directly into '${selectedSave.name}'`}
					>
						▶ Launch Save
					</Button>
				</div>
				<div class="insp-subline">
					<span>Folder: {selectedSave.name}</span>
					<span>·</span>
					<span>Last Played: {selectedSave.lastModifiedDisplay || 'Unknown'}</span>
					<span class="insp-link-chip" title={selectedSave.path || ''}>Path</span>
				</div>
			</div>

			{#if errors.length > 0}
				<div style="margin-bottom:12px;">
					<PanelCard tone="danger">
						<div class="control-label" style="color:var(--danger);">Save Integrity Issues</div>
						<div class="insp-meta" style="color:var(--text);">{errors.join(' · ')}</div>
					</PanelCard>
				</div>
			{/if}

			{#if missing.length > 0}
				<SectionHeader title={`Missing Workshop Items (${missing.length})`} tone="danger">
					{#snippet actions()}
						<Button variant="primary" size="sm" onclick={() => copyMissing(missing)}>
							Copy Missing IDs
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
									Steam Client ↗
								</Button>
								<Button
									size="sm"
									onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(id)}`)}
								>
									Web Page ↗
								</Button>
							</div>
						</div>
					{/each}
				</div>
			{/if}

			<div class="save-details-grid">
				{#if inGameDate}
					<div class="save-detail-card">
						<div class="save-detail-label">In-Game Date</div>
						<div class="save-detail-val">{inGameDate}</div>
					</div>
				{/if}
				<div class="save-detail-card">
					<div class="save-detail-label">Workshop Mods</div>
					<div class="save-detail-val">{installedCount} / {refs.length} installed</div>
				</div>
				<div class="save-detail-card">
					<div class="save-detail-label">Save Size</div>
					<div class="save-detail-val">{sizeMb} ({selectedSave.fileCount || 0} files)</div>
				</div>
				{#if selectedSave.worldSize}
					<div class="save-detail-card">
						<div class="save-detail-label">Map Size</div>
						<div class="save-detail-val">{selectedSave.worldSize}</div>
					</div>
				{/if}
				{#if selectedSave.originalMapName}
					<div class="save-detail-card">
						<div class="save-detail-label">Map Template</div>
						<div class="save-detail-val">{selectedSave.originalMapName}</div>
					</div>
				{/if}
				{#if selectedSave.headerVersion}
					<div class="save-detail-card">
						<div class="save-detail-label">Header Format</div>
						<div class="save-detail-val">v{selectedSave.headerVersion}</div>
					</div>
				{/if}
			</div>

			<SectionHeader title={`Installed Workshop Items in Save (${installed.length})`} />
			{#if installed.length === 0}
				<div class="insp-meta">
					{refs.length === 0
						? "No Workshop items are referenced in this save's header.bin."
						: 'None of the referenced Workshop items are currently installed.'}
				</div>
			{:else}
				<div class="schema-form" style="margin-top:8px;">
					{#each installed as im (im.id)}
						{@const knownName = modNameByWsId.get(String(im.id))}
						{@const assetsText = (im.assets || []).length > 0 ? `Assets: ${im.assets.join(', ')}` : ''}
						{@const metaParts = [im.itemType || 'Workshop Item', assetsText].filter(Boolean)}
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
									Steam ↗
								</Button>
								<Button
									size="sm"
									onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(im.id)}`)}
								>
									Web ↗
								</Button>
							</div>
						</div>
					{/each}
				</div>
			{/if}
		{/if}
	{/snippet}
</SplitLayout>
