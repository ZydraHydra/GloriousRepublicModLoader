<script lang="ts">
	import { app, type ModItem, type UiControl } from '$lib/stores/app.svelte';
	import { t } from '$lib/i18n/index.svelte';
	import { abiBadgeVariant, compareVersionsDesc, openExternalUrl } from '$lib/utils';
	import { Badge, Button, EmptyState, PanelCard, SectionHeader, Select, TextInput, ToggleSwitch } from '$lib/components/ui';

	interface IniEntry {
		key: string;
		value: string;
	}
	interface IniSection {
		name: string;
		entries: IniEntry[];
	}
	interface IniData {
		exists: boolean;
		sections: IniSection[];
		rawText: string;
	}

	interface Props {
		mod: ModItem | null;
	}

	let { mod }: Props = $props();

	let inspectorTab = $state<'schema' | 'raw'>('schema');
	let iniData = $state<IniData>({ exists: false, sections: [], rawText: '' });
	let rawText = $state('');
	let fieldValues = $state<Record<string, string>>({});
	let descExpanded = $state(false);
	let descOverflow = $state(false);
	let descEl = $state<HTMLDivElement | null>(null);
	let lastLoadedKey = '';

	const verInfo = $derived.by(() => {
		if (!mod) return null;
		const list = (mod.supportedGameVersions || []).filter(Boolean);
		if (list.length === 0) return null;
		const sorted = [...list].sort(compareVersionsDesc);
		const latest = sorted[0];
		const allText = list.join(', ');
		const detected = app.discovery?.gameVersion || '';
		if (mod.gameVersionMismatch) {
			return {
				label: `! WRSR ${latest}`,
				mismatch: true,
				tooltip: t('inspector.detectedWrsrMismatch', {
					versions: allText,
					detected: detected || t('common.unknown')
				})
			};
		}
		const match =
			list.find((v) => v === detected) ||
			list.find((v) => detected && (detected.startsWith(v) || v.startsWith(detected))) ||
			latest;
		return {
			label: `WRSR ${match}`,
			mismatch: false,
			tooltip: t('inspector.supportedWrsr', { versions: allText })
		};
	});

	function getIniVal(sections: IniSection[], secName: string, keyName: string, fallback: string): string {
		const sec = sections.find((s) => s.name.toLowerCase() === (secName || '').toLowerCase());
		const ent = sec?.entries.find((e) => e.key.toLowerCase() === (keyName || '').toLowerCase());
		return ent ? ent.value : fallback;
	}

	function ctrlKey(c: UiControl, idx: number): string {
		return `${c.section || 'settings'}::${c.key || idx}`;
	}

	$effect(() => {
		const curMod = mod;
		if (!curMod) {
			lastLoadedKey = '';
			return;
		}
		const key = `${curMod.id}|${curMod.iniPath}`;
		if (key === lastLoadedKey) return;
		lastLoadedKey = key;
		inspectorTab = curMod.uiSchema ? 'schema' : 'raw';
		descExpanded = false;

		if (!curMod.iniPath) {
			iniData = { exists: false, sections: [], rawText: '' };
			rawText = '';
			fieldValues = {};
			return;
		}
		fetch(`/api/mod-ini?iniPath=${encodeURIComponent(curMod.iniPath)}`)
			.then((r) => r.json())
			.then((data: IniData) => {
				iniData = data;
				rawText = data.rawText || `; ${curMod.iniPath}\n[settings]\n`;
				const nextVals: Record<string, string> = {};
				const controls = curMod.uiSchema?.controls || [];
				controls.forEach((ctrl, idx) => {
					if (ctrl.type === 'header') return;
					const k = ctrlKey(ctrl, idx);
					nextVals[k] = getIniVal(
						data.sections || [],
						ctrl.section || 'settings',
						ctrl.key || '',
						ctrl.defaultValue ?? ''
					);
				});
				fieldValues = nextVals;
			});
	});

	$effect(() => {
		if (!descEl || !mod?.description) {
			descOverflow = false;
			return;
		}
		// Measure collapsed overflow on next frame
		requestAnimationFrame(() => {
			if (descEl && !descExpanded) {
				descOverflow = descEl.scrollHeight > descEl.clientHeight + 2;
			}
		});
	});

	function isBoolTrue(v: string): boolean {
		const lo = (v || '').toLowerCase();
		return lo === '1' || lo === 'true' || lo === 'yes' || lo === 'on';
	}

	async function saveSchemaSettings() {
		if (!mod?.iniPath || !mod.uiSchema) return;
		const updates = (mod.uiSchema.controls || [])
			.map((ctrl, idx) => ({ ctrl, idx }))
			.filter(({ ctrl }) => ctrl.type !== 'header' && Boolean(ctrl.key))
			.map(({ ctrl, idx }) => ({
				section: ctrl.section || 'settings',
				key: ctrl.key || '',
				value: fieldValues[ctrlKey(ctrl, idx)] ?? ctrl.defaultValue ?? ''
			}));
		const resp = await fetch('/api/mod-ini', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ iniPath: mod.iniPath, updates })
		});
		if ((await resp.json()).ok) {
			app.setStatus(t('inspector.savedFileStatus', { path: mod.iniPath }));
		}
	}

	async function saveRawIni() {
		if (!mod?.iniPath) return;
		const resp = await fetch('/api/mod-ini', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({ iniPath: mod.iniPath, rawText })
		});
		if ((await resp.json()).ok) {
			app.setStatus(t('inspector.savedFileStatus', { path: mod.iniPath }));
		}
	}

	async function exportSchema() {
		if (!mod) return;
		const resp = await fetch('/api/generate-schema', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({
				dllPath: mod.dllPath,
				modName: mod.name,
				iniPath: mod.iniPath
			})
		});
		const res = await resp.json();
		if (res.ok) {
			app.setStatus(t('inspector.generatedSchemaStatus', { path: res.outPath }));
			await app.fetchState();
		} else {
			app.setStatus(t('inspector.exportFailedStatus', { error: res.error || t('common.unknownError') }));
		}
	}
</script>

{#if !mod}
	<EmptyState message={t('inspector.emptySelection')} />
{:else}
	{@const subParts = [
		mod.author && mod.author !== 'Unknown' ? t('inspector.byAuthor', { author: mod.author }) : '',
		mod.version && mod.version !== 'unversioned' ? `v${mod.version}` : ''
	].filter(Boolean)}
	{@const wsUrl = mod.workshopId ? `https://steamcommunity.com/sharedfiles/filedetails/?id=${encodeURIComponent(mod.workshopId)}` : ''}

	<div class="insp-header">
		<div class="insp-hero">
			{#if mod.imagePath}
				<img
					class="insp-thumb"
					src="/api/mod-image?path={encodeURIComponent(mod.imagePath)}"
					alt={mod.name}
				/>
			{/if}
			<div class="insp-hero-body">
				<h2>
					{mod.name}
					<Badge variant={abiBadgeVariant(mod.abi)}>{mod.abi}</Badge>
					{#if verInfo}
						<Badge variant={verInfo.mismatch ? 'warn' : 'default'} title={verInfo.tooltip}>
							{verInfo.label}
						</Badge>
					{/if}
				</h2>
				<div class="insp-subline">
					<span>{subParts.length > 0 ? subParts.join(' · ') : mod.id}</span>
					<span class="insp-link-chip" title={t('inspector.dllTooltip', { id: mod.id, dllPath: mod.dllPath })}>DLL</span>
					<span
						class="insp-link-chip"
						title={t('inspector.iniTooltip', {
							iniPath: mod.iniPath || t('inspector.iniNone'),
							state: iniData.exists ? t('inspector.iniFound') : t('inspector.iniCreatedOnSave')
						})}
					>
						INI
					</span>
					{#if wsUrl}
						<button
							type="button"
							class="insp-link-url"
							style="background:none;border-top:none;border-left:none;border-right:none;padding:0;"
							title={t('inspector.openWorkshopTip', { id: mod.workshopId })}
							onclick={() => openExternalUrl(wsUrl)}
						>
							{t('inspector.workshopLink')}
						</button>
					{/if}
				</div>

				{#if mod.description}
					<div class="insp-desc-wrap">
						<div class="insp-desc" class:collapsed={!descExpanded} bind:this={descEl}>
							{mod.description}
						</div>
						{#if descOverflow || descExpanded}
							<button
								type="button"
								class="insp-desc-toggle"
								onclick={() => (descExpanded = !descExpanded)}
							>
								{descExpanded ? t('inspector.showLess') : t('inspector.showMore')}
							</button>
						{/if}
					</div>
				{/if}

				{#if mod.warning}
					<div class="insp-meta" style="color:var(--danger);">
						<strong>{t('inspector.warningLabel')}</strong> {mod.warning}
					</div>
				{/if}
			</div>
		</div>
	</div>

	<div class="insp-tabs-row">
		<div class="insp-tabs" style="margin-bottom:0;">
			<Button
				size="sm"
				variant={inspectorTab === 'schema' ? 'primary' : 'default'}
				onclick={() => (inspectorTab = 'schema')}
			>
				{mod.uiSchema ? t('inspector.tabSettings') : t('inspector.tabSettingsNoIni')}
			</Button>
			<Button
				size="sm"
				variant={inspectorTab === 'raw' ? 'primary' : 'default'}
				onclick={() => (inspectorTab = 'raw')}
			>
				{t('inspector.tabRawIni')}
			</Button>
		</div>
		{#if mod.uiSchema?.autoGenerated}
			<Button size="sm" onclick={exportSchema}>{t('inspector.exportUiYaml')}</Button>
		{/if}
	</div>

	{#if !mod.iniPath}
		<EmptyState message={t('inspector.noIniAssociated')} />
	{:else if inspectorTab === 'schema' && !mod.uiSchema}
		<EmptyState message={t('inspector.noSchemaFound')} />
	{:else if inspectorTab === 'schema' && mod.uiSchema}
		<div class="schema-form">
			{#each mod.uiSchema.controls || [] as ctrl, idx (idx)}
				{#if ctrl.type === 'header'}
					<SectionHeader title={ctrl.label} />
				{:else}
					{@const k = ctrlKey(ctrl, idx)}
					<PanelCard>
						<div class="control-top">
							<div class="control-label">{ctrl.label}</div>
							{#if ctrl.type === 'bool'}
								<ToggleSwitch
									checked={isBoolTrue(fieldValues[k] ?? '')}
									onchange={(c) => {
										fieldValues[k] = c ? '1' : '0';
									}}
								/>
							{/if}
						</div>
						{#if ctrl.type !== 'bool'}
							<div class="control-input-row">
								{#if ctrl.type === 'int' || ctrl.type === 'float'}
									{@const step = ctrl.step && ctrl.step > 0 ? ctrl.step : ctrl.type === 'int' ? 1 : 0.1}
									{@const curNum = Number.parseFloat(fieldValues[k] ?? '') || 0}
									{@const min = ctrl.hasExplicitRange ? (ctrl.minValue ?? 0) : Math.min(0, curNum)}
									{@const max = ctrl.hasExplicitRange
										? (ctrl.maxValue ?? 100)
										: Math.max(curNum > 0 ? curNum * 2 : 10, ctrl.type === 'int' ? 100 : 10)}
									<input
										type="range"
										{min}
										{max}
										{step}
										value={curNum}
										oninput={(e) => {
											fieldValues[k] = (e.currentTarget as HTMLInputElement).value;
										}}
									/>
									<input
										type="number"
										style="width:88px;"
										step={ctrl.hasExplicitRange ? step : 'any'}
										value={fieldValues[k] ?? String(curNum)}
										oninput={(e) => {
											fieldValues[k] = (e.currentTarget as HTMLInputElement).value;
										}}
									/>
								{:else if ctrl.type === 'select'}
									<Select
										options={(ctrl.options || []).map((o) => ({ value: o, label: o }))}
										value={fieldValues[k] ?? ''}
										onchange={(v) => {
											fieldValues[k] = v;
										}}
									/>
								{:else}
									<TextInput
										value={fieldValues[k] ?? ''}
										oninput={(v) => {
											fieldValues[k] = v;
										}}
									/>
								{/if}
							</div>
						{/if}
						{#if ctrl.tooltip}
							<div class="control-tip">{ctrl.tooltip}</div>
						{/if}
					</PanelCard>
				{/if}
			{/each}

			<div>
				<Button variant="primary" onclick={saveSchemaSettings}>
					{t('inspector.saveSettingsTo', { file: mod.iniPath.split(/[\\/]/).pop() ?? '' })}
				</Button>
			</div>
		</div>
	{:else}
		<div>
			<textarea class="raw-ini-area" bind:value={rawText}></textarea>
			<div style="margin-top:10px;">
				<Button variant="primary" onclick={saveRawIni}>{t('inspector.saveRawIni')}</Button>
			</div>
		</div>
	{/if}
{/if}
