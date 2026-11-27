<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
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
	<h2>Zero-Code Declarative UI for External Mods</h2>
	<p>
		External mods (including legacy TesmioLoader, RML v1, or raw DLLs without GRML C++ integration)
		can ship a tiny <code>&lt;ModName&gt;.ui.yaml</code> or <code>&lt;ModName&gt;.ui.json</code> beside their DLL,
		or include a <code>"settingsUi"</code> block in <code>&lt;ModName&gt;.grml.json</code>.
	</p>
	<p>
		GRML automatically renders those controls <strong>both here in the Launcher</strong> and
		<strong>in-game inside the mod menu (press <code>{app.overlayHotkey}</code>)</strong>, reading &amp; saving directly to the mod's <code>.ini</code> file.
	</p>
	<SectionHeader title="Example: MyMod.ui.yaml" />
	<pre class="code-block">{exampleYaml}</pre>
</div>
