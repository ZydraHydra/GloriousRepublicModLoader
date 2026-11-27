<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { t } from '$lib/i18n/index.svelte';
	import { SectionHeader } from '$lib/components/ui';

	const exampleYaml = `tabTitle: My Mod Settings
iniFile: MyMod.ini
mainMenuAvailable: true
controls:
  - type: header
    label: General Settings
  - type: bool
    section: settings
    key: enabled
    label: Enable Mod
    default: true
    tooltip: Master toggle saved to MyMod.ini
  - type: float
    section: settings
    key: speed_multiplier
    label: Speed Multiplier
    min: 0.25
    max: 10.0
    step: 0.25
    format: "%.2fx"
    default: 2.0
  - type: select
    section: settings
    key: mode
    label: Mode
    options: ["Balanced", "Aggressive"]
    default: Balanced`;
</script>

<div class="panel">
	<h2>{t('schemaHelp.title')}</h2>
	<p>
		{@html t('schemaHelp.p1')}
	</p>
	<p>
		{@html t('schemaHelp.p2', { hotkey: app.overlayHotkey })}
	</p>
	<SectionHeader title={t('schemaHelp.exampleHeader')} />
	<pre class="code-block">{exampleYaml}</pre>
</div>
