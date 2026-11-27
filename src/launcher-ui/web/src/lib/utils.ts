export function compareVersionsDesc(a: string, b: string): number {
	const pa = String(a).split(/[^0-9]+/).map((n) => parseInt(n, 10) || 0);
	const pb = String(b).split(/[^0-9]+/).map((n) => parseInt(n, 10) || 0);
	const len = Math.max(pa.length, pb.length);
	for (let i = 0; i < len; i++) {
		const diff = (pb[i] || 0) - (pa[i] || 0);
		if (diff !== 0) return diff;
	}
	return String(b).localeCompare(String(a));
}

export function abiBadgeVariant(abi: string): 'grml' | 'rml' | 'tesmio' | 'default' {
	if (abi === 'GRML') return 'grml';
	if (abi === 'RML v1') return 'rml';
	if (abi === 'Tesmio v4') return 'tesmio';
	return 'default';
}

export async function openExternalUrl(targetUrl: string): Promise<void> {
	await fetch('/api/open-url', {
		method: 'POST',
		headers: { 'content-type': 'application/json' },
		body: JSON.stringify({ url: targetUrl })
	});
}
