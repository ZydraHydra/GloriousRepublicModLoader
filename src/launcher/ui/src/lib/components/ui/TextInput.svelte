<script lang="ts">
	interface Props {
		value?: string;
		placeholder?: string;
		mono?: boolean;
		readonly?: boolean;
		disabled?: boolean;
		width?: string;
		oninput?: (value: string) => void;
		onchange?: (value: string) => void;
	}

	let {
		value = $bindable(''),
		placeholder = '',
		mono = false,
		readonly = false,
		disabled = false,
		width = '',
		oninput,
		onchange
	}: Props = $props();
</script>

<input
	type="text"
	class="ui-input"
	class:mono
	style={width ? `width:${width};flex:0 0 ${width};` : ''}
	{placeholder}
	{readonly}
	{disabled}
	bind:value
	oninput={() => oninput?.(value)}
	onchange={() => onchange?.(value)}
/>

<style>
	.ui-input {
		background: #0f1114;
		color: var(--text);
		border: 1px solid var(--border);
		border-radius: 4px;
		padding: 5px 9px;
		font-size: 12px;
		min-width: 0;
		flex: 1 1 160px;
	}
	.ui-input:focus {
		outline: none;
		border-color: var(--gold);
	}
	.ui-input.mono {
		font-family: var(--mono);
	}
</style>
