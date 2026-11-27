<script lang="ts">
	import type { Snippet } from 'svelte';

	interface Props {
		title: string;
		tone?: 'gold' | 'danger';
		actions?: Snippet;
	}

	let { title, tone = 'gold', actions }: Props = $props();
</script>

<div class="ui-sec-hdr">
	<span class="ui-sec-title" class:danger={tone === 'danger'}>{title}</span>
	{#if actions}
		<div class="ui-sec-actions">{@render actions()}</div>
	{/if}
</div>

<style>
	.ui-sec-hdr {
		display: flex;
		align-items: center;
		justify-content: space-between;
		gap: 8px;
		margin-top: 10px;
		margin-bottom: 8px;
		padding-bottom: 4px;
		border-bottom: 1px solid var(--border);
	}
	.ui-sec-title {
		color: var(--gold);
		font-size: 11px;
		font-weight: 700;
		text-transform: uppercase;
		letter-spacing: 0.6px;
	}
	.ui-sec-title.danger {
		color: var(--danger);
	}
	.ui-sec-actions {
		display: flex;
		align-items: center;
		gap: 6px;
	}
</style>
