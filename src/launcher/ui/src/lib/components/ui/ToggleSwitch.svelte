<script lang="ts">
	interface Props {
		checked?: boolean;
		disabled?: boolean;
		label?: string;
		title?: string;
		onchange?: (checked: boolean) => void;
	}

	let {
		checked = $bindable(false),
		disabled = false,
		label = '',
		title = '',
		onchange
	}: Props = $props();

	function handleChange(ev: Event) {
		const target = ev.currentTarget as HTMLInputElement;
		checked = target.checked;
		onchange?.(checked);
	}
</script>

<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<label
	class="ui-toggle-wrap"
	class:disabled
	{title}
	onclick={(e) => e.stopPropagation()}
>
	<span class="switch">
		<input type="checkbox" {checked} {disabled} onchange={handleChange} />
		<span class="switch-slider"></span>
	</span>
	{#if label}
		<span class="ui-toggle-label">{label}</span>
	{/if}
</label>

<style>
	.ui-toggle-wrap {
		display: inline-flex;
		align-items: center;
		gap: 8px;
		cursor: pointer;
		font-size: 12px;
		color: var(--text);
		user-select: none;
		white-space: nowrap;
	}
	.ui-toggle-wrap.disabled {
		opacity: 0.5;
		cursor: not-allowed;
	}
	.ui-toggle-label {
		color: var(--text);
	}
</style>
