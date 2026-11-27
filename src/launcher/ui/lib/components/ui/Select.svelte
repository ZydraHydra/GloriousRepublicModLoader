<script lang="ts">
	export interface SelectOption {
		value: string;
		label: string;
	}

	interface Props {
		value?: string;
		options: SelectOption[];
		disabled?: boolean;
		onchange?: (value: string) => void;
	}

	let {
		value = $bindable(''),
		options,
		disabled = false,
		onchange
	}: Props = $props();
</script>

<select
	class="ui-select"
	{disabled}
	bind:value
	onchange={() => onchange?.(value)}
>
	{#each options as opt, idx (`${idx}::${opt.value}`)}
		<option value={opt.value}>{opt.label}</option>
	{/each}
</select>

<style>
	.ui-select {
		background: #0f1114;
		color: var(--text);
		border: 1px solid var(--border);
		border-radius: 4px;
		padding: 5px 9px;
		font-size: 12px;
		min-width: 0;
	}
	.ui-select:focus {
		outline: none;
		border-color: var(--gold);
	}
</style>
