<script lang="ts">
	import { app } from '$lib/stores/app.svelte';
	import { Button, PanelCard, Select, TextInput, ToggleSwitch } from '$lib/components/ui';

	let listeningHotkey = $state(false);

	const hotkeyPresets = [
		'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12',
		'Insert', 'Home', 'End', 'PageUp', 'PageDown', 'Pause', 'ScrollLock'
	];

	const hotkeyOptions = $derived.by(() => {
		const opts = hotkeyPresets.map((k) => ({ value: k, label: k }));
		if (app.overlayHotkey && !hotkeyPresets.includes(app.overlayHotkey)) {
			opts.push({ value: app.overlayHotkey, label: app.overlayHotkey });
		}
		return opts;
	});

	function mapEventToHotkey(ev: KeyboardEvent): string | null {
		if (ev.key === 'Escape') return null;
		if (/^F([1-9]|1[0-2])$/.test(ev.key)) return ev.key;
		if (ev.key === 'Insert') return 'Insert';
		if (ev.key === 'Home') return 'Home';
		if (ev.key === 'End') return 'End';
		if (ev.key === 'PageUp') return 'PageUp';
		if (ev.key === 'PageDown') return 'PageDown';
		if (ev.key === 'Pause') return 'Pause';
		if (ev.key === 'ScrollLock') return 'ScrollLock';
		if (ev.key === '`' || ev.key === '~') return 'Tilde';
		if (/^[a-zA-Z0-9]$/.test(ev.key)) return ev.key.toUpperCase();
		return null;
	}

	function startRebind() {
		listeningHotkey = true;
		const onKeyDown = async (ev: KeyboardEvent) => {
			if (['Shift', 'Control', 'Alt', 'Meta'].includes(ev.key)) return;
			ev.preventDefault();
			ev.stopPropagation();
			window.removeEventListener('keydown', onKeyDown, true);
			listeningHotkey = false;
			const mapped = mapEventToHotkey(ev);
			if (mapped) {
				app.overlayHotkey = mapped;
				await app.saveGrmlIni();
				app.setStatus(`In-game menu hotkey set to ${mapped}.`);
			}
		};
		window.addEventListener('keydown', onKeyDown, true);
	}

	const verLabel = $derived(
		app.discovery && app.discovery.gameVersion !== 'unknown'
			? `WRSR ${app.discovery.gameVersion}`
			: 'WRSR (unrecognized build)'
	);

	async function browseGame() {
		const resp = await fetch(`/api/browse-game?current=${encodeURIComponent(app.gameExeInput.trim())}`);
		const data = await resp.json();
		if (data.ok && data.path) {
			app.gameExeInput = data.path;
			await app.saveGrmlIni();
			await app.fetchState();
			app.setStatus(`Game executable set to ${data.path}`);
		}
	}

	async function onPathChange() {
		await app.saveGrmlIni();
		await app.fetchState();
	}

	async function onDiscoveryToggle() {
		await app.saveGrmlIni();
		await app.fetchState();
	}
</script>

<div class="panel">
	<h2>Loader Settings</h2>
	<p class="muted" style="margin:0 0 14px 0;">
		Changes are saved automatically to <code>grml.ini</code> in <code>%LOCALAPPDATA%\GRML</code>.
	</p>

	<div class="schema-form">
		<PanelCard>
			<div class="control-top">
				<div class="control-label">Game Executable (<code>SOVIET64.exe</code>)</div>
				<span class="pill">{verLabel}</span>
			</div>
			<div class="control-tip">
				Path to your <code>SOVIET64.exe</code> installation used for launching, version detection, and Workshop discovery.
			</div>
			<div class="control-input-row">
				<TextInput
					bind:value={app.gameExeInput}
					mono={true}
					placeholder="Path to SOVIET64.exe"
					onchange={onPathChange}
				/>
				<Button onclick={browseGame}>Set Path</Button>
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">Scan Steam Workshop (784150)</div>
				<ToggleSwitch bind:checked={app.chkWorkshop} onchange={onDiscoveryToggle} />
			</div>
			<div class="control-tip">
				Automatically discover DLL plugins inside subscribed Steam Workshop mod folders (enabled by default).
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">Scan Local Development Folder (<code>workshop_wip</code>)</div>
				<ToggleSwitch bind:checked={app.chkWip} onchange={onDiscoveryToggle} />
			</div>
			<div class="control-tip">
				Discover local in-development DLL mods inside <code>media_soviet\workshop_wip\</code> (enabled by default).
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">Enable Runtime Asset Redirection</div>
				<ToggleSwitch bind:checked={app.chkVfsEnabled} onchange={() => app.saveVfsConfig()} />
			</div>
			<div class="control-tip">
				Non-destructive virtual filesystem redirection for <code>media_soviet\</code> and <code>override\</code> packs (saved to <code>grml_vfs.ini</code>).
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">In-Game Mod Settings Hotkey</div>
				<div style="display:flex;align-items:center;gap:8px;">
					<Select
						bind:value={app.overlayHotkey}
						options={hotkeyOptions}
						onchange={() => app.saveGrmlIni()}
					/>
					<Button
						variant={listeningHotkey ? 'primary' : 'default'}
						size="sm"
						onclick={startRebind}
					>
						{listeningHotkey ? 'Press any key... (Esc to cancel)' : 'Rebind Key'}
					</Button>
				</div>
			</div>
			<div class="control-tip">
				Key used in-game to toggle the GRML Mod Settings overlay (saved as <code>overlay_hotkey</code> in <code>grml.ini</code>; takes effect on next game launch).
			</div>
		</PanelCard>

		<PanelCard>
			<div class="control-top">
				<div class="control-label">Show GRML Tag on Main Menu</div>
				<ToggleSwitch bind:checked={app.chkMenuStatus} onchange={() => app.saveGrmlIni()} />
			</div>
			<div class="control-tip">
				Appends <code>| GRML 2.0.0</code> to the in-game main menu version string so you can confirm GRML is active.
			</div>
		</PanelCard>
	</div>
</div>
