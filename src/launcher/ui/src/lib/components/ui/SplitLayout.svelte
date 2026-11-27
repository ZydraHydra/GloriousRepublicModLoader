<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		toolbar?: Snippet;
		banner?: Snippet;
		list?: Snippet;
		footer?: Snippet;
		inspector?: Snippet;
	}

	let { toolbar, banner, list, footer, inspector }: Props = $props();
</script>

<div class="mods-layout">
	<div class="mods-list-pane">
		{#if toolbar}
			<div class="toolbar">{@render toolbar()}</div>
		{/if}
		{#if banner}
			{@render banner()}
		{/if}
		<div class="mods-table">{@render list?.()}</div>
		{#if footer}
			<div class="mods-bottom-bar">{@render footer()}</div>
		{/if}
	</div>
	<div class="inspector-pane">
		{@render inspector?.()}
	</div>
</div>
