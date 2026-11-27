<script lang="ts">
	import { app, type ModItem, type PlaylistEntry } from '$lib/stores/app.svelte';
	import { t } from '$lib/i18n/index.svelte';
	import { openExternalUrl } from '$lib/utils';
	import {
		Badge,
		Button,
		EmptyState,
		PanelCard,
		TextInput,
		ToggleSwitch
	} from '$lib/components/ui';

	let playlistName = $state('');
	let steamInput = $state('');
	let importingSteam = $state(false);
	let syncingName = $state<string | null>(null);
	let syncingAllSteam = $state(false);
	let steamError = $state('');
	let selectedMap = $state<Record<string, boolean>>({});
	let expandedMap = $state<Record<string, boolean>>({});

	const installedWorkshopIds = $derived.by(() => {
		const s = new Set<string>();
		for (const m of app.discovery?.mods ?? []) {
			if (m.workshopId) s.add(m.workshopId);
		}
		return s;
	});

	const steamPlaylists = $derived(
		app.playlists.filter((pl) => Boolean(pl.collectionId))
	);

	function getPlaylistMods(pl: PlaylistEntry): ModItem[] {
		const idSet = new Set(pl.enabledIds ?? []);
		const wsSet = new Set(pl.workshopIds ?? []);
		return (app.discovery?.mods ?? []).filter(
			(m) => idSet.has(m.id) || (wsSet.size > 0 && m.workshopId && wsSet.has(m.workshopId))
		);
	}

	function getMissingWorkshopIds(pl: PlaylistEntry): string[] {
		return (pl.workshopIds ?? []).filter((wsId) => !installedWorkshopIds.has(wsId));
	}

	const selectedPlaylists = $derived(
		app.playlists.filter((pl) => Boolean(selectedMap[pl.name]))
	);

	function toggleSelect(name: string, checked: boolean) {
		selectedMap = { ...selectedMap, [name]: checked };
	}

	function selectAll() {
		const next: Record<string, boolean> = {};
		for (const pl of app.playlists) next[pl.name] = true;
		selectedMap = next;
	}

	function clearSelection() {
		selectedMap = {};
	}

	function toggleExpanded(name: string) {
		expandedMap = { ...expandedMap, [name]: !expandedMap[name] };
	}

	async function savePlaylist() {
		const name = playlistName.trim();
		if (!name || !app.discovery) return;
		const enabledIds = app.discovery.mods.filter((m) => m.enabled).map((m) => m.id);
		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ action: 'save', name, enabledIds, source: 'local' })
		});
		playlistName = '';
		await app.fetchState();
		app.setStatus(t('playlists.savedStatus', { name, count: enabledIds.length }));
	}

	async function importSteamCollection() {
		const raw = steamInput.trim();
		if (!raw || importingSteam) return;
		steamError = '';
		importingSteam = true;
		app.setStatus(t('playlists.fetchingSteamStatus'));
		try {
			const resp = await fetch('/api/playlists/import-steam', {
				method: 'POST',
				headers: { 'content-type': 'application/json' },
				body: JSON.stringify({ collection: raw })
			});
			const data = await resp.json();
			if (!data.ok) {
				steamError = data.error || t('playlists.importFailed');
				app.setStatus(steamError);
				return;
			}

			const workshopIds: string[] = data.workshopIds ?? [];
			const wsSet = new Set(workshopIds);
			const matchedIds = (app.discovery?.mods ?? [])
				.filter((m) => m.workshopId && wsSet.has(m.workshopId))
				.map((m) => m.id);

			const name = data.title || t('playlists.steamDefaultTitle', { id: data.collectionId });
			await fetch('/api/playlists', {
				method: 'POST',
				headers: { 'content-type': 'application/json' },
				body: JSON.stringify({
					action: 'save',
					name,
					enabledIds: matchedIds,
					workshopIds,
					source: 'steam',
					collectionId: data.collectionId,
					collectionUrl: data.collectionUrl
				})
			});

			steamInput = '';
			await app.fetchState();
			app.setStatus(
				t('playlists.importedStatus', { name, matched: matchedIds.length, total: workshopIds.length })
			);
		} catch {
			steamError = t('playlists.steamNetworkError');
			app.setStatus(steamError);
		} finally {
			importingSteam = false;
		}
	}

	async function syncSingleSteamCollection(pl: PlaylistEntry): Promise<boolean> {
		if (!pl.collectionId) return false;
		const resp = await fetch('/api/playlists/import-steam', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ collection: pl.collectionId })
		});
		const data = await resp.json();
		if (!data.ok) {
			throw new Error(data.error || `Failed to sync '${pl.name}'.`);
		}
		const workshopIds: string[] = data.workshopIds ?? [];
		const wsSet = new Set(workshopIds);
		const matchedIds = (app.discovery?.mods ?? [])
			.filter((m) => m.workshopId && wsSet.has(m.workshopId))
			.map((m) => m.id);

		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({
				action: 'save',
				name: pl.name,
				enabledIds: matchedIds,
				workshopIds,
				source: 'steam',
				collectionId: data.collectionId,
				collectionUrl: data.collectionUrl
			})
		});
		return true;
	}

	async function syncSteamPlaylist(pl: PlaylistEntry) {
		if (!pl.collectionId || syncingName || syncingAllSteam) return;
		syncingName = pl.name;
		steamError = '';
		app.setStatus(t('playlists.updatingPlaylistStatus', { name: pl.name }));
		try {
			await syncSingleSteamCollection(pl);
			await app.fetchState();
			app.setStatus(t('playlists.updatedPlaylistStatus', { name: pl.name }));
		} catch (e) {
			steamError = e instanceof Error ? e.message : t('playlists.updateFailed');
			app.setStatus(steamError);
		} finally {
			syncingName = null;
		}
	}

	async function updateAllSteamCollections() {
		if (steamPlaylists.length === 0 || syncingAllSteam || syncingName) return;
		syncingAllSteam = true;
		steamError = '';
		app.setStatus(t('playlists.updatingAllStatus', { count: steamPlaylists.length }));
		let updated = 0;
		const errors: string[] = [];
		try {
			for (const pl of steamPlaylists) {
				try {
					if (await syncSingleSteamCollection(pl)) updated++;
				} catch (e) {
					errors.push(e instanceof Error ? e.message : pl.name);
				}
			}
			await app.fetchState();
			if (errors.length > 0) {
				steamError = t('playlists.updatedAllPartialError', {
					updated,
					total: steamPlaylists.length,
					errors: errors.join('; ')
				});
				app.setStatus(steamError);
			} else {
				app.setStatus(t('playlists.updatedAllStatus', { count: updated }));
			}
		} finally {
			syncingAllSteam = false;
		}
	}

	async function togglePlaylist(pl: PlaylistEntry, enable: boolean) {
		if (!app.discovery) return;
		const mods = getPlaylistMods(pl);
		let changed = 0;
		for (const m of mods) {
			if (enable && m.blocked) continue;
			if (m.enabled !== enable) {
				m.enabled = enable;
				changed++;
			}
		}
		await app.saveGrmlIni();
		await app.fetchState();
		app.setStatus(
			t(enable ? 'playlists.enabledStatus' : 'playlists.disabledStatus', {
				name: pl.name,
				total: mods.length,
				changed
			})
		);
	}

	async function soloPlaylist(pl: PlaylistEntry) {
		if (!app.discovery) return;
		const targetSet = new Set(getPlaylistMods(pl).map((m) => m.id));
		for (const m of app.discovery.mods) {
			m.enabled = targetSet.has(m.id) && !m.blocked;
		}
		await app.saveGrmlIni();
		await app.fetchState();
		app.setStatus(t('playlists.soloStatus', { name: pl.name }));
	}

	async function applySelected(enable: boolean) {
		if (!app.discovery || selectedPlaylists.length === 0) return;
		const targetIds = new Set<string>();
		for (const pl of selectedPlaylists) {
			for (const m of getPlaylistMods(pl)) {
				targetIds.add(m.id);
			}
		}
		let changed = 0;
		for (const m of app.discovery.mods) {
			if (!targetIds.has(m.id)) continue;
			if (enable && m.blocked) continue;
			if (m.enabled !== enable) {
				m.enabled = enable;
				changed++;
			}
		}
		await app.saveGrmlIni();
		await app.fetchState();
		app.setStatus(
			t(enable ? 'playlists.bulkEnabledStatus' : 'playlists.bulkDisabledStatus', {
				count: selectedPlaylists.length,
				changed
			})
		);
	}

	async function deletePlaylist(name: string) {
		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ action: 'delete', name })
		});
		const next = { ...selectedMap };
		delete next[name];
		selectedMap = next;
		await app.fetchState();
	}

	async function deleteSelected() {
		if (selectedPlaylists.length === 0) return;
		const names = selectedPlaylists.map((p) => p.name);
		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ action: 'delete', names })
		});
		selectedMap = {};
		await app.fetchState();
		app.setStatus(t('playlists.deletedSelectedStatus', { count: names.length }));
	}
</script>

<div class="panel">
	<div class="pl-header">
		<div>
			<h2>{t('playlists.title')}</h2>
			<p class="muted" style="margin-bottom:0;">
				{t('playlists.subtitle')}
			</p>
		</div>
		{#if steamPlaylists.length > 0}
			<Button
				size="sm"
				disabled={syncingAllSteam || Boolean(syncingName)}
				onclick={updateAllSteamCollections}
				title={t('playlists.updateAllSteamTip')}
			>
				{syncingAllSteam ? t('playlists.updatingSteam') : t('playlists.updateAllSteam', { count: steamPlaylists.length })}
			</Button>
		{/if}
	</div>

	<div class="pl-action-grid">
		<div class="pl-action-card">
			<div class="pl-card-label">{t('playlists.snapshotLabel')}</div>
			<div class="pl-input-row">
				<TextInput
					bind:value={playlistName}
					placeholder={t('playlists.playlistNamePlaceholder')}
				/>
				<Button variant="primary" disabled={!playlistName.trim()} onclick={savePlaylist}>
					{t('common.save')}
				</Button>
			</div>
		</div>

		<div class="pl-action-card">
			<div class="pl-card-label">{t('playlists.importSteamLabel')}</div>
			<div class="pl-input-row">
				<TextInput
					bind:value={steamInput}
					placeholder={t('playlists.steamInputPlaceholder')}
				/>
				<Button
					variant="primary"
					disabled={importingSteam || !steamInput.trim()}
					onclick={importSteamCollection}
				>
					{importingSteam ? t('playlists.importingBtn') : t('playlists.importBtn')}
				</Button>
			</div>
		</div>
	</div>

	{#if steamError}
		<div class="warnings-banner pl-error-banner">{steamError}</div>
	{/if}

	{#if app.playlists.length === 0}
		<EmptyState message={t('playlists.empty')} />
	{:else}
		<div class="pl-bulk-bar">
			<div class="pl-bulk-left">
				<Button size="sm" onclick={selectAll}>{t('playlists.selectAll')}</Button>
				{#if selectedPlaylists.length > 0}
					<Button size="sm" onclick={clearSelection}>{t('playlists.clearSelection', { count: selectedPlaylists.length })}</Button>
				{/if}
			</div>
			{#if selectedPlaylists.length > 0}
				<div class="pl-bulk-right">
					<Button variant="primary" size="sm" onclick={() => applySelected(true)}>
						{t('playlists.enableSelected', { count: selectedPlaylists.length })}
					</Button>
					<Button size="sm" onclick={() => applySelected(false)}>
						{t('playlists.disableSelected', { count: selectedPlaylists.length })}
					</Button>
					<Button size="sm" onclick={deleteSelected}>
						{t('playlists.deleteSelected')}
					</Button>
				</div>
			{/if}
		</div>

		<div class="playlists-list">
			{#each app.playlists as pl (pl.name)}
				{@const matchedMods = getPlaylistMods(pl)}
				{@const enabledCount = matchedMods.filter((m) => m.enabled).length}
				{@const allEnabled = matchedMods.length > 0 && enabledCount === matchedMods.length}
				{@const partialEnabled = enabledCount > 0 && enabledCount < matchedMods.length}
				{@const missingWs = getMissingWorkshopIds(pl)}
				{@const isSteam = pl.source === 'steam' || Boolean(pl.collectionId)}
				{@const isExpanded = Boolean(expandedMap[pl.name])}
				<PanelCard>
					<div class="pl-row">
						<div class="pl-left">
							<input
								type="checkbox"
								checked={Boolean(selectedMap[pl.name])}
								onchange={(e) => toggleSelect(pl.name, e.currentTarget.checked)}
								title={t('playlists.selectPlaylistTip')}
							/>
							<ToggleSwitch
								checked={allEnabled}
								disabled={matchedMods.length === 0}
								title={allEnabled ? t('playlists.disableAllInListTip') : t('playlists.enableAllInListTip')}
								onchange={(checked) => togglePlaylist(pl, checked)}
							/>
							<div class="pl-info">
								<div class="pl-title-row">
									<strong>{pl.name}</strong>
									{#if isSteam}
										<Badge variant="grml" title={t('playlists.steamBadgeTip')}>{t('playlists.steamBadge')}</Badge>
									{/if}
									{#if allEnabled}
										<Badge variant="ready">{t('playlists.activeCount', { enabled: enabledCount, total: matchedMods.length })}</Badge>
									{:else if partialEnabled}
										<Badge variant="warn">{t('playlists.activeCount', { enabled: enabledCount, total: matchedMods.length })}</Badge>
									{:else}
										<Badge>{t('playlists.activeCount', { enabled: enabledCount, total: matchedMods.length })}</Badge>
									{/if}
									{#if missingWs.length > 0}
										<Badge variant="warn" title={t('playlists.missingBadgeTip')}>
											{t('playlists.missingCount', { count: missingWs.length })}
										</Badge>
									{/if}
								</div>
								<div class="muted pl-sub">
									{#if isSteam && pl.workshopIds}
										{t('playlists.steamSub', { installed: matchedMods.length, workshop: pl.workshopIds.length })}
									{:else}
										{t('playlists.localSub', { count: matchedMods.length })}
									{/if}
								</div>
							</div>
						</div>
						<div class="pl-actions">
							<Button size="sm" onclick={() => toggleExpanded(pl.name)}>
								{isExpanded ? t('playlists.hideMods') : t('playlists.showMods')}
							</Button>
							<Button
								size="sm"
								disabled={matchedMods.length === 0}
								title={t('playlists.soloTip')}
								onclick={() => soloPlaylist(pl)}
							>
								{t('playlists.solo')}
							</Button>
							{#if isSteam && pl.collectionId}
								<Button
									size="sm"
									disabled={syncingName === pl.name || syncingAllSteam}
									title={t('playlists.syncTip')}
									onclick={() => syncSteamPlaylist(pl)}
								>
									{syncingName === pl.name ? t('playlists.syncing') : t('playlists.sync')}
								</Button>
							{/if}
							{#if pl.collectionUrl}
								<Button size="sm" onclick={() => openExternalUrl(pl.collectionUrl!)}>
									{t('playlists.openOnSteam')}
								</Button>
							{/if}
							<Button size="sm" onclick={() => deletePlaylist(pl.name)}>{t('common.delete')}</Button>
						</div>
					</div>

					{#if isExpanded}
						<div class="pl-details">
							{#if matchedMods.length === 0}
								<div class="muted" style="font-size:11px;">{t('playlists.noMatchingMods')}</div>
							{:else}
								<div class="pl-mod-chips">
									{#each matchedMods as m (m.id)}
										<span class="pl-chip" class:pl-chip-on={m.enabled} title={m.id}>
											{m.name}
										</span>
									{/each}
								</div>
							{/if}
							{#if missingWs.length > 0}
								<div class="pl-missing">
									<span class="muted" style="font-size:11px;">{t('playlists.missingWorkshopIds')}</span>
									{#each missingWs as wsId (wsId)}
										<button
											type="button"
											class="pl-ws-link"
											onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(wsId)}`)}
										>
											{wsId}
										</button>
									{/each}
								</div>
							{/if}
						</div>
					{/if}
				</PanelCard>
			{/each}
		</div>
	{/if}
</div>

<style>
	.pl-header {
		display: flex;
		align-items: flex-start;
		justify-content: space-between;
		gap: 12px;
		flex-wrap: wrap;
		margin-bottom: 14px;
	}
	.pl-action-grid {
		display: grid;
		grid-template-columns: repeat(auto-fit, minmax(290px, 1fr));
		gap: 10px;
		margin-bottom: 12px;
	}
	.pl-action-card {
		background: var(--panel);
		border: 1px solid var(--border);
		border-radius: 6px;
		padding: 10px 12px;
		display: flex;
		flex-direction: column;
		gap: 7px;
	}
	.pl-card-label {
		font-size: 10px;
		font-weight: 700;
		text-transform: uppercase;
		letter-spacing: 0.7px;
		color: var(--muted);
	}
	.pl-input-row {
		display: flex;
		align-items: center;
		gap: 7px;
	}
	.pl-input-row :global(input) {
		flex: 1;
		min-width: 0;
		background: #0f1114;
		color: var(--text);
		border: 1px solid var(--border);
		border-radius: 4px;
		padding: 6px 9px;
		font-size: 12px;
	}
	.pl-error-banner {
		border: 1px solid #6e4e1c;
		border-radius: 5px;
		margin-bottom: 12px;
	}
	.pl-bulk-bar {
		display: flex;
		align-items: center;
		justify-content: space-between;
		flex-wrap: wrap;
		gap: 8px;
		padding: 8px 0;
		border-top: 1px solid var(--border);
		border-bottom: 1px solid var(--border);
	}
	.pl-bulk-left,
	.pl-bulk-right {
		display: flex;
		align-items: center;
		gap: 6px;
		flex-wrap: wrap;
	}
	.pl-row {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 10px;
		flex-wrap: wrap;
	}
	.pl-left {
		display: flex;
		align-items: center;
		gap: 10px;
		min-width: 0;
		flex: 1;
	}
	.pl-info {
		min-width: 0;
	}
	.pl-title-row {
		display: flex;
		align-items: center;
		gap: 6px;
		flex-wrap: wrap;
	}
	.pl-sub {
		font-size: 11px;
		margin-top: 2px;
	}
	.pl-actions {
		display: flex;
		gap: 6px;
		flex-wrap: wrap;
	}
	.pl-details {
		margin-top: 10px;
		padding-top: 10px;
		border-top: 1px solid var(--border);
		display: flex;
		flex-direction: column;
		gap: 8px;
	}
	.pl-mod-chips {
		display: flex;
		flex-wrap: wrap;
		gap: 5px;
	}
	.pl-chip {
		font-size: 11px;
		padding: 2px 7px;
		border-radius: 4px;
		border: 1px solid var(--border);
		background: var(--bg);
		color: var(--muted);
	}
	.pl-chip-on {
		border-color: var(--gold);
		color: var(--text);
	}
	.pl-missing {
		display: flex;
		align-items: center;
		flex-wrap: wrap;
		gap: 5px;
	}
	.pl-ws-link {
		background: transparent;
		border: 1px solid var(--border);
		color: var(--gold);
		font-family: var(--mono);
		font-size: 11px;
		padding: 1px 6px;
		border-radius: 4px;
		cursor: pointer;
	}
	.pl-ws-link:hover {
		border-color: var(--gold);
	}
</style>
