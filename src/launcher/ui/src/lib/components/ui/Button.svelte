<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		variant?: 'default' | 'primary' | 'danger';
		size?: 'sm' | 'md';
		disabled?: boolean;
		title?: string;
		type?: 'button' | 'submit';
		onclick?: (ev: MouseEvent) => void;
		children?: Snippet;
	}

	let {
		variant = 'default',
		size = 'md',
		disabled = false,
		title = '',
		type = 'button',
		onclick,
		children
	}: Props = $props();
</script>

<button
	{type}
	{disabled}
	{title}
	class="btn"
	class:btn-sm={size === 'sm'}
	class:btn-primary={variant === 'primary'}
	class:btn-danger={variant === 'danger'}
	{onclick}
>
	{@render children?.()}
</button>

<style>
	.btn-danger {
		border-color: var(--danger);
		color: #f7b8b3;
		background: rgba(226, 88, 77, 0.14);
	}
	.btn-danger:hover:not(:disabled) {
		background: rgba(226, 88, 77, 0.25);
		border-color: #f2756b;
		color: #fff;
	}
</style>
