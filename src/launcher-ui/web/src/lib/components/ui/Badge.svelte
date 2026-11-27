<script lang="ts">
	import type { Snippet } from 'svelte';

	export type BadgeVariant = 'default' | 'grml' | 'rml' | 'tesmio' | 'ready' | 'warn' | 'danger';

	interface Props {
		variant?: BadgeVariant;
		title?: string;
		children?: Snippet;
	}

	let { variant = 'default', title = '', children }: Props = $props();

	const variantClass = $derived(
		variant === 'grml'
			? 'badge-grml'
			: variant === 'rml'
				? 'badge-rml'
				: variant === 'tesmio'
					? 'badge-tesmio'
					: variant === 'ready'
						? 'badge-ready'
						: variant === 'warn' || variant === 'danger'
							? 'badge-warn'
							: ''
	);
</script>

<span class="badge {variantClass}" {title} class:has-tip={Boolean(title)}>
	{@render children?.()}
</span>

<style>
	.has-tip {
		cursor: help;
	}
</style>
