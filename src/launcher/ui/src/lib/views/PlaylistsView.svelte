<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { Button, EmptyState, PanelCard, TextInput } from '$lib/components/ui';

	let playlistName = $state('');

	async function savePlaylist() {
		const name = playlistName.trim();
		if (!name || !app.discovery) return;
		const enabledIds = app.discovery.mods.filter((m) => m.enabled).map((m) => m.id);
		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ action: 'save', name, enabledIds })
		});
		playlistName = '';
		await app.fetchState();
		app.setStatus(`Saved playlist '${name}'.`);
	}

	async function applyPlaylist(name: string, enabledIds: string[]) {
		if (!app.discovery) return;
		const set = new Set(enabledIds);
		for (const m of app.discovery.mods) {
			m.enabled = set.has(m.id);
		}
		await app.saveGrmlIni();
		await app.fetchState();
		app.setStatus(`Applied playlist '${name}'.`);
	}

	async function deletePlaylist(name: string) {
		await fetch('/api/playlists', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ action: 'delete', name })
		});
		await app.fetchState();
	}
</script>

<div class="panel">
	<h2>Saved Mod Playlists</h2>
	<p class="muted">Snapshot your currently enabled mods into reusable profiles.</p>
	<div class="toolbar" style="border:1px solid var(--border);border-radius:6px;margin-bottom:12px;">
		<TextInput
			bind:value={playlistName}
			placeholder="New playlist name (e.g. Realism + Speed)"
		/>
		<Button variant="primary" onclick={savePlaylist}>Save Current Selection</Button>
	</div>

	{#if app.playlists.length === 0}
		<EmptyState message="No saved playlists yet." />
	{:else}
		<div class="playlists-list">
			{#each app.playlists as pl (pl.name)}
				<PanelCard>
					<div class="pl-row">
						<div>
							<strong>{pl.name}</strong>
							<div class="muted" style="font-size:11px;">{pl.enabledIds.length} enabled mod(s)</div>
						</div>
						<div class="pl-actions">
							<Button variant="primary" size="sm" onclick={() => applyPlaylist(pl.name, pl.enabledIds)}>
								Apply
							</Button>
							<Button size="sm" onclick={() => deletePlaylist(pl.name)}>Delete</Button>
						</div>
					</div>
				</PanelCard>
			{/each}
		</div>
	{/if}
</div>

<style>
	.pl-row {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 10px;
	}
	.pl-actions {
		display: flex;
		gap: 6px;
	}
</style>
