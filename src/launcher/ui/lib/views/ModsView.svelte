<script lang="ts">
	import { app, type ModItem } from '$lib/stores/app.svelte';
	import { abiBadgeVariant, compareVersionsDesc } from '$lib/utils';
	import { Badge, Button, Select, SplitLayout, TextInput, ToggleSwitch } from '$lib/components/ui';
	import ModInspector from './ModInspector.svelte';

	let searchQuery = $state('');
	let abiFilter = $state('all');
	let versionFilter = $state('all');

	const abiOptions = [
		{ value: 'all', label: 'All ABIs' },
		{ value: 'GRML', label: 'GRML' },
		{ value: 'RML v1', label: 'RML v1' },
		{ value: 'Tesmio v4', label: 'Tesmio v4' },
		{ value: 'Raw DLL', label: 'Raw DLL' }
	];

	const versionOptions = $derived.by(() => {
		const versions = new Set<string>();
		let hasUnspecified = false;
		for (const m of app.discovery?.mods || []) {
			const list = (m.supportedGameVersions || []).filter(Boolean);
			if (list.length === 0) hasUnspecified = true;
			else list.forEach((v) => versions.add(v));
		}
		const opts = [{ value: 'all', label: 'All Versions' }];
		for (const v of [...versions].sort(compareVersionsDesc)) {
			opts.push({ value: v, label: v });
		}
		if (hasUnspecified) opts.push({ value: '__unspecified__', label: 'Unspecified' });
		return opts;
	});

	const filteredMods = $derived.by(() => {
		const q = searchQuery.trim().toLowerCase();
		return (app.discovery?.mods || []).filter((m) => {
			if (abiFilter !== 'all' && m.abi !== abiFilter) return false;
			if (versionFilter !== 'all') {
				const supported = (m.supportedGameVersions || []).filter(Boolean);
				if (versionFilter === '__unspecified__') {
					if (supported.length > 0) return false;
				} else if (!supported.includes(versionFilter)) {
					return false;
				}
			}
			if (!q) return true;
			return (
				m.name.toLowerCase().includes(q) ||
				m.id.toLowerCase().includes(q) ||
				(m.author || '').toLowerCase().includes(q) ||
				(m.workshopId || '').includes(q)
			);
		});
	});

	const selectedMod = $derived(
		(app.discovery?.mods || []).find((m) => m.id === app.selectedModId) ?? null
	);

	function toggleMod(mod: ModItem, checked: boolean) {
		if (mod.blocked) return;
		mod.enabled = checked;
		app.saveGrmlIni();
	}

	async function enableAll() {
		if (!app.discovery) return;
		filteredMods.forEach((m) => {
			if (!m.blocked) m.enabled = true;
		});
		await app.saveGrmlIni();
		await app.fetchState();
	}

	async function disableAll() {
		if (!app.discovery) return;
		filteredMods.forEach((m) => {
			m.enabled = false;
		});
		await app.saveGrmlIni();
		await app.fetchState();
	}

	async function reloadMods() {
		await app.fetchState();
		app.setStatus('Reloaded Mods & Workshop list.');
	}
</script>

<SplitLayout>
	{#snippet toolbar()}
		<TextInput
			bind:value={searchQuery}
			placeholder="Search mods by name, ID, author, Workshop ID..."
		/>
		<Select bind:value={abiFilter} options={abiOptions} />
		<Select bind:value={versionFilter} options={versionOptions} />
	{/snippet}

	{#snippet banner()}
		{#if (app.discovery?.warnings || []).length > 0}
			<div class="warnings-banner">{app.discovery?.warnings.join(' | ')}</div>
		{/if}
	{/snippet}

	{#snippet list()}
		{#each filteredMods as mod (mod.id)}
			{@const subParts = [
				mod.author && mod.author !== 'Unknown' ? mod.author : '',
				mod.version && mod.version !== 'unversioned' ? `v${mod.version}` : '',
				mod.workshopId ? `Workshop ${mod.workshopId}` : 'Local'
			].filter(Boolean)}

			<!-- svelte-ignore a11y_click_events_have_key_events -->
			<!-- svelte-ignore a11y_no_static_element_interactions -->
			<div
				class="mod-row"
				class:selected={mod.id === app.selectedModId}
				class:disabled={!mod.enabled}
				onclick={() => (app.selectedModId = mod.id)}
			>
				<ToggleSwitch
					checked={!mod.blocked && mod.enabled}
					disabled={mod.blocked}
					title={mod.blocked ? mod.warning || 'Blocked' : ''}
					onchange={(c) => toggleMod(mod, c)}
				/>

				{#if mod.imagePath}
					<img
						class="mod-thumb"
						src="/api/mod-image?path={encodeURIComponent(mod.imagePath)}"
						alt=""
					/>
				{:else}
					<div class="mod-thumb-fallback">{(mod.name || 'M').slice(0, 2).toUpperCase()}</div>
				{/if}

				<div style="min-width:0;">
					<div class="mod-title">{mod.name}</div>
					<div class="mod-sub">{subParts.join(' · ')}</div>
				</div>

				<div class="mod-badges">
					{#if mod.blocked || mod.warning}
						<Badge variant="warn" title={mod.warning || 'Dependency/conflict issue'}>
							{mod.blocked ? '! Built into GRML' : '! Warning'}
						</Badge>
					{/if}
					{#if mod.gameVersionMismatch}
						<Badge
							variant="warn"
							title={`Mod declares support for [${(mod.supportedGameVersions || []).join(', ')}], but detected game build is ${app.discovery?.gameVersion}. It can still be loaded.`}
						>
							! Version
						</Badge>
					{/if}
					<Badge variant={abiBadgeVariant(mod.abi)}>{mod.abi}</Badge>
				</div>
			</div>
		{/each}
	{/snippet}

	{#snippet footer()}
		<Button size="sm" onclick={enableAll}>Enable All</Button>
		<Button size="sm" onclick={disableAll}>Disable All</Button>
		<div style="margin-left:auto;">
			<Button size="sm" onclick={reloadMods}>↻ Reload</Button>
		</div>
	{/snippet}

	{#snippet inspector()}
		<ModInspector mod={selectedMod} />
	{/snippet}
</SplitLayout>
