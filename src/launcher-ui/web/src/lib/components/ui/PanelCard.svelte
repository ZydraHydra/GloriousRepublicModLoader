<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		tone?: 'default' | 'danger' | 'ready' | 'gold';
		padding?: string;
		children?: Snippet;
	}

	let { tone = 'default', padding = '', children }: Props = $props();
</script>

<div
	class="control-card"
	class:tone-danger={tone === 'danger'}
	class:tone-ready={tone === 'ready'}
	class:tone-gold={tone === 'gold'}
	style={padding ? `padding:${padding};` : ''}
>
	{@render children?.()}
</div>

<style>
	.tone-danger {
		border-color: var(--danger);
	}
	.tone-ready {
		border-color: rgba(105, 184, 100, 0.38);
	}
	.tone-gold {
		border-color: var(--gold);
	}
</style>
