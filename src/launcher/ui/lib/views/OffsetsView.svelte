<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { t } from '$lib/i18n/index.svelte';
	import { Badge, Button, PanelCard } from '$lib/components/ui';

	const offsetEntries = $derived(app.discovery?.offsets ?? []);
	const missingOffsetCount = $derived(
		offsetEntries.filter((o) => o.status === 'MISSING').length
	);
	const resolvedOffsetCount = $derived(
		offsetEntries.filter((o) => o.status === 'OK' || o.status === 'CACHED').length
	);
	const missingCoreOffsets = $derived(
		offsetEntries.filter((o) => o.required && o.status === 'MISSING')
	);
	const groupedOffsets = $derived.by(() => {
		const order = [
			'Core',
			'TimeAndSim',
			'Economy',
			'EntityManagers',
			'Templates',
			'SaveLoad',
			'CameraAndUI',
			'General'
		];
		const map = new Map<string, typeof offsetEntries>();
		for (const e of offsetEntries) {
			const cat = e.category || 'General';
			if (!map.has(cat)) map.set(cat, []);
			map.get(cat)!.push(e);
		}
		const out: Array<{ category: string; items: typeof offsetEntries }> = [];
		for (const c of order) {
			if (map.has(c)) {
				out.push({ category: c, items: map.get(c)! });
				map.delete(c);
			}
		}
		for (const [c, items] of map.entries()) {
			out.push({ category: c, items });
		}
		return out;
	});

	function offsetBadgeVariant(status: string): 'ready' | 'grml' | 'warn' | 'default' {
		if (status === 'OK') return 'ready';
		if (status === 'CACHED') return 'grml';
		if (status === 'MISSING') return 'warn';
		return 'default';
	}

	function parseVerifiedBuilds(raw: string): { latest: string; others: string[]; all: string[] } {
		const list = (raw || '')
			.split(',')
			.map((s) => s.trim())
			.filter(Boolean);
		if (list.length === 0) return { latest: '—', others: [], all: [] };
		const cmp = (a: string, b: string) => {
			const pa = a.split('.').map((n) => parseInt(n, 10) || 0);
			const pb = b.split('.').map((n) => parseInt(n, 10) || 0);
			const len = Math.max(pa.length, pb.length);
			for (let i = 0; i < len; i++) {
				const diff = (pb[i] ?? 0) - (pa[i] ?? 0);
				if (diff !== 0) return diff;
			}
			return b.localeCompare(a);
		};
		const sorted = [...list].sort(cmp);
		return { latest: sorted[0], others: sorted.slice(1), all: sorted };
	}
</script>

<div class="panel offsets-panel">
	<div class="offsets-header">
		<div>
			<h2 style="margin:0;">{t('diagnostics.offsetsSection')}</h2>
			{#if app.discovery?.offsetsHash}
				<div class="muted" style="font-size:12px;margin-top:3px;">
					<code>grml_offsets.ini</code> FNV-1a: <code>{app.discovery.offsetsHash}</code>
					{#if app.discovery.gameVersion && app.discovery.gameVersion !== 'unknown'}
						· WRSR <code>{app.discovery.gameVersion}</code>
					{/if}
					· {resolvedOffsetCount}/{offsetEntries.length}
					{#if missingOffsetCount > 0}
						· <span style="color:var(--danger);">{t('diagnostics.offsetsMissing', { count: missingOffsetCount })}</span>
					{/if}
				</div>
			{/if}
		</div>
		<div class="offsets-actions">
			{#if app.offsetsOtaResult}
				{#if app.offsetsOtaResult.ok}
					<Badge variant={app.offsetsOtaResult.updated ? 'grml' : 'ready'}>
						{app.offsetsOtaResult.updated ? t('diagnostics.offsetsUpdated') : t('diagnostics.offsetsUpToDate')}
					</Badge>
				{:else}
					<Badge variant="warn">{app.offsetsOtaResult.error || t('diagnostics.otaFailed')}</Badge>
				{/if}
			{/if}
			<Button size="sm" onclick={() => app.fetchState()}>{t('diagnostics.refreshBtn')}</Button>
			<Button size="sm" disabled={app.updatingOffsets} onclick={() => app.updateOffsetsOta()}>
				{app.updatingOffsets ? t('common.updating') : t('diagnostics.updateOffsetsOta')}
			</Button>
		</div>
	</div>

	<div class="offsets-body">
		{#if missingCoreOffsets.length > 0}
			<PanelCard tone="danger">
				<div class="control-label" style="color:var(--danger);">
					AOB_MISSING — Required Core offset(s) missing: {missingCoreOffsets.map((o) => o.name).join(', ')}
				</div>
			</PanelCard>
		{/if}
		{#if offsetEntries.length === 0}
			<PanelCard>
				<div class="muted">{t('diagnostics.noOffsets')}</div>
			</PanelCard>
		{:else}
			{#each groupedOffsets as grp (grp.category)}
				<PanelCard>
					<div class="diag-label" style="margin-bottom:6px;">
						{grp.category} ({grp.items.length})
					</div>
					<div class="offsets-table-wrap">
						<table class="offsets-table">
							<colgroup>
								<col class="col-section" />
								<col class="col-tier" />
								<col class="col-status" />
								<col class="col-resolve" />
								<col class="col-build" />
								<col class="col-target" />
							</colgroup>
							<thead>
								<tr>
									<th>{t('diagnostics.colSection')}</th>
									<th>Tier</th>
									<th>{t('diagnostics.colStatus')}</th>
									<th>{t('diagnostics.colResolveMode')}</th>
									<th>{t('diagnostics.colVerifiedBuild')}</th>
									<th>{t('diagnostics.colRvaTarget')}</th>
								</tr>
							</thead>
							<tbody>
								{#each grp.items as entry (entry.name)}
									{@const builds = parseVerifiedBuilds(entry.verifiedBuild)}
									<tr title={entry.anchorXref ? t('diagnostics.anchorTooltip', { anchor: entry.anchorXref }) : ''}>
										<td>
											<code>[{entry.name}]</code>
											{#if entry.anchorXref}
												<div class="muted" style="font-size:10px;">{entry.anchorXref}</div>
											{/if}
										</td>
										<td>
											<Badge variant={entry.required ? 'warn' : 'default'}>
												{entry.required ? 'Required' : 'Optional'}
											</Badge>
										</td>
										<td>
											<Badge variant={offsetBadgeVariant(entry.status)}>{entry.status}</Badge>
										</td>
										<td><code>{entry.resolve}</code></td>
										<td>
											{#if builds.all.length <= 1}
												<span>{builds.latest}</span>
											{:else}
												<span class="build-cell">
													<span>{builds.latest}</span>
													<span
														class="build-more"
														title={builds.all.join(', ')}
													>
														+{builds.others.length}
														<span class="build-popover" role="tooltip">
															{builds.all.join(', ')}
														</span>
													</span>
												</span>
											{/if}
										</td>
										<td>
											{#if entry.status === 'OK' || entry.status === 'CACHED'}
												<code>{entry.rva || '—'}</code>
												{#if entry.resolved}
													<span class="muted"> → </span><code>{entry.resolved}</code>
												{/if}
											{:else}
												<span class="muted">{t('diagnostics.matchesCount', { count: entry.matchCount })}</span>
											{/if}
										</td>
									</tr>
								{/each}
							</tbody>
						</table>
					</div>
				</PanelCard>
			{/each}
		{/if}
	</div>
</div>

<style>
	.offsets-panel {
		display: flex;
		flex-direction: column;
		gap: 12px;
	}
	.offsets-header {
		display: flex;
		justify-content: space-between;
		align-items: flex-start;
		gap: 12px;
		flex-wrap: wrap;
	}
	.offsets-actions {
		display: flex;
		align-items: center;
		gap: 8px;
		flex-wrap: wrap;
	}
	.offsets-body {
		display: flex;
		flex-direction: column;
		gap: 10px;
	}
	.offsets-table-wrap {
		overflow: visible;
	}
	.offsets-table {
		width: 100%;
		min-width: 720px;
		table-layout: fixed;
		border-collapse: collapse;
		font-size: 12px;
	}
	.offsets-table col.col-section {
		width: 30%;
	}
	.offsets-table col.col-tier {
		width: 11%;
	}
	.offsets-table col.col-status {
		width: 11%;
	}
	.offsets-table col.col-resolve {
		width: 15%;
	}
	.offsets-table col.col-build {
		width: 15%;
	}
	.offsets-table col.col-target {
		width: 18%;
	}
	.offsets-table th,
	.offsets-table td {
		text-align: left;
		padding: 6px 8px;
		border-bottom: 1px solid rgba(255, 255, 255, 0.06);
		overflow-wrap: anywhere;
		vertical-align: middle;
	}
	.offsets-table th {
		font-size: 10px;
		text-transform: uppercase;
		letter-spacing: 0.5px;
		color: var(--muted);
	}
	.offsets-table tr:last-child td {
		border-bottom: none;
	}
	.build-cell {
		display: inline-flex;
		align-items: center;
		gap: 5px;
		white-space: nowrap;
	}
	.build-more {
		position: relative;
		display: inline-flex;
		align-items: center;
		padding: 1px 5px;
		border-radius: 999px;
		font-size: 10px;
		font-weight: 600;
		line-height: 1.3;
		color: var(--gold, #e5b84b);
		background: rgba(229, 184, 75, 0.14);
		border: 1px solid rgba(229, 184, 75, 0.35);
		cursor: help;
	}
	.build-popover {
		display: none;
		position: absolute;
		bottom: calc(100% + 6px);
		left: 50%;
		transform: translateX(-50%);
		z-index: 20;
		white-space: nowrap;
		padding: 4px 8px;
		border-radius: 6px;
		font-size: 11px;
		font-weight: 500;
		color: var(--text, #e8e9ec);
		background: #17191f;
		border: 1px solid rgba(255, 255, 255, 0.14);
		box-shadow: 0 6px 18px rgba(0, 0, 0, 0.45);
		pointer-events: none;
	}
	.build-more:hover .build-popover,
	.build-more:focus-visible .build-popover {
		display: block;
	}
</style>
