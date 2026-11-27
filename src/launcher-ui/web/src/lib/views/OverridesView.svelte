<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { openExternalUrl } from '$lib/utils';
	import { Badge, Button, PanelCard, SectionHeader, ToggleSwitch } from '$lib/components/ui';

	function movePack(idx: number, delta: number) {
		const target = idx + delta;
		if (target < 0 || target >= app.vfs.packs.length) return;
		const [moved] = app.vfs.packs.splice(idx, 1);
		app.vfs.packs.splice(target, 0, moved);
		app.saveVfsConfig();
	}

	async function reloadPacks() {
		await app.fetchState();
		app.setStatus('Reloaded Asset Override packs.');
	}
</script>

<div class="panel">
	<div class="ov-hdr">
		<h2 style="margin:0;">Asset Overrides (Media VFS)</h2>
		<div class="ov-hdr-actions">
			<Badge>
				{app.vfs.packs.length} pack(s) · {app.vfs.indexedFiles} file(s) · {app.vfs.conflictCount} conflict(s)
			</Badge>
			<Button size="sm" onclick={reloadPacks}>↻ Reload Packs</Button>
		</div>
	</div>
	<p class="muted" style="margin:6px 0 14px 0;">
		Non-destructive virtual filesystem redirection for <code>media_soviet\</code> and <code>override\</code> folders or <code>.zip</code> packs. Higher packs (#1) win on file collisions.
	</p>

	<SectionHeader title="Override Packs & Priority Order" />
	{#if app.vfs.packs.length === 0}
		<PanelCard>
			<span class="muted">
				No asset override packs found in <code>&lt;GameDir&gt;\media_overrides\</code> or Steam Workshop <code>media_soviet/override</code> folders.
			</span>
		</PanelCard>
	{:else}
		<div class="playlists-list" style="margin-bottom:16px;">
			{#each app.vfs.packs as pk, idx (pk.id)}
				{@const wsId = pk.id.startsWith('ws:') ? pk.id.slice(3) : ''}
				<PanelCard>
					<div class="ov-pack-row">
						<div class="ov-pack-left">
							<ToggleSwitch
								bind:checked={pk.enabled}
								onchange={() => app.saveVfsConfig()}
							/>
							<div>
								<div class="ov-pack-title">
									<strong>#{idx + 1} {pk.displayName}</strong>
									<Badge variant={pk.source === 'Workshop' ? 'rml' : 'grml'}>{pk.source}</Badge>
								</div>
								<div class="muted" style="font-size:11px;margin-top:2px;">
									<code>{pk.id}</code> · {pk.indexedFiles}/{pk.totalFiles} file(s)
									{#if pk.conflictCount > 0}
										· <span style="color:var(--warn)">{pk.conflictCount} conflict(s)</span>
									{/if}
									{#if wsId}
										· <button
											type="button"
											class="insp-link-url"
											style="background:none;border-top:none;border-left:none;border-right:none;padding:0;"
											onclick={() => openExternalUrl(`https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(wsId)}`)}
										>
											Workshop ↗
										</button>
									{/if}
								</div>
							</div>
						</div>
						<div class="ov-pack-actions">
							<Button size="sm" disabled={idx === 0} onclick={() => movePack(idx, -1)}>↑</Button>
							<Button size="sm" disabled={idx + 1 >= app.vfs.packs.length} onclick={() => movePack(idx, 1)}>↓</Button>
						</div>
					</div>
				</PanelCard>
			{/each}
		</div>
	{/if}

	<SectionHeader title="File Conflict Inspector" />
	{#if (app.vfs.conflicts || []).length === 0}
		<PanelCard>
			<span class="muted">No asset override conflicts detected across enabled packs.</span>
		</PanelCard>
	{:else}
		<div class="schema-form">
			{#each app.vfs.conflicts as c (c.relativePath)}
				<PanelCard>
					<div><code>{c.relativePath}</code></div>
					<div class="control-tip">
						<strong>Winner:</strong> {c.winningPack} · <strong>Overridden:</strong> {(c.overriddenPacks || []).join(', ')}
					</div>
				</PanelCard>
			{/each}
		</div>
	{/if}
</div>

<style>
	.ov-hdr {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 12px;
		flex-wrap: wrap;
	}
	.ov-hdr-actions {
		display: flex;
		align-items: center;
		gap: 8px;
	}
	.ov-pack-row {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 10px;
	}
	.ov-pack-left {
		display: flex;
		align-items: center;
		gap: 10px;
		min-width: 0;
	}
	.ov-pack-title {
		display: flex;
		align-items: center;
		gap: 7px;
		flex-wrap: wrap;
	}
	.ov-pack-actions {
		display: flex;
		gap: 6px;
		flex-shrink: 0;
	}
</style>
