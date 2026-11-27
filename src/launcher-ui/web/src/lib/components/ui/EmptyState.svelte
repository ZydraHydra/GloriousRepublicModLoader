<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		message?: string;
		children?: Snippet;
	}

	let { message = '', children }: Props = $props();
</script>

<div class="empty-state">
	{#if children}
		{@render children()}
	{:else}
		{message}
	{/if}
</div>
