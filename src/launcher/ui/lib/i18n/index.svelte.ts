export interface LocaleInfo {
	code: string;
	name: string;
	nativeName: string;
}

type MessageTree = {
	[key: string]: string | MessageTree;
};

const STORAGE_KEY = 'grml.language';

const FALLBACK_LOCALES: LocaleInfo[] = [
	{ code: 'en', name: 'English', nativeName: 'English' },
	{ code: 'de', name: 'German', nativeName: 'Deutsch' }
];

function flattenMessages(obj: MessageTree, prefix = '', out: Record<string, string> = {}): Record<string, string> {
	for (const [k, v] of Object.entries(obj)) {
		if (k === '_meta') continue;
		const fullKey = prefix ? `${prefix}.${k}` : k;
		if (typeof v === 'string') {
			out[fullKey] = v;
		} else if (v && typeof v === 'object' && !Array.isArray(v)) {
			flattenMessages(v, fullKey, out);
		}
	}
	return out;
}

class I18nStore {
	locale = $state<string>('en');
	setting = $state<string>('auto');
	detectedGameLocale = $state<string>('');
	available = $state<LocaleInfo[]>(FALLBACK_LOCALES);
	ready = $state<boolean>(false);
	private dictionaries = $state<Record<string, Record<string, string>>>({});
	private initPromise: Promise<void> | null = null;

	private matchAvailable(candidate: string | null | undefined): string | null {
		if (!candidate) return null;
		const norm = candidate.trim().toLowerCase();
		if (!norm) return null;
		const exact = this.available.find((l) => l.code.toLowerCase() === norm);
		if (exact) return exact.code;
		const base = norm.split(/[-_]/)[0];
		const baseMatch = this.available.find((l) => l.code.toLowerCase() === base);
		return baseMatch ? baseMatch.code : null;
	}

	private detectInitialLocale(preferred?: string, gameLocale?: string): { setting: string; resolved: string } {
		const rawPref = (preferred ?? '').trim().toLowerCase();
		if (rawPref && rawPref !== 'auto') {
			const exact = this.matchAvailable(rawPref);
			if (exact) return { setting: exact, resolved: exact };
		}

		// Explicit 'auto' in grml.ini, or unset: check localStorage only if not explicitly 'auto'
		if (!rawPref) {
			try {
				const stored = localStorage.getItem(STORAGE_KEY);
				if (stored && stored !== 'auto') {
					const fromStorage = this.matchAvailable(stored);
					if (fromStorage) return { setting: fromStorage, resolved: fromStorage };
				}
			} catch {
				// Ignore storage errors
			}
		}

		const fromGame = this.matchAvailable(gameLocale);
		if (fromGame) return { setting: 'auto', resolved: fromGame };

		if (typeof navigator !== 'undefined') {
			const langs = navigator.languages?.length ? navigator.languages : [navigator.language];
			for (const lang of langs) {
				const matched = this.matchAvailable(lang);
				if (matched) return { setting: 'auto', resolved: matched };
			}
		}
		return { setting: 'auto', resolved: 'en' };
	}

	async loadDictionary(code: string, force = false): Promise<boolean> {
		if (!force && this.dictionaries[code]) return true;
		try {
			const resp = await fetch(`/locales/${encodeURIComponent(code)}.json`, { cache: 'no-store' });
			if (!resp.ok) return false;
			const json = (await resp.json()) as MessageTree;
			this.dictionaries = {
				...this.dictionaries,
				[code]: flattenMessages(json)
			};
			return true;
		} catch {
			return false;
		}
	}

	async refreshAvailableLocales(): Promise<void> {
		try {
			const resp = await fetch('/api/locales', { cache: 'no-store' });
			if (resp.ok) {
				const data = (await resp.json()) as { locales?: LocaleInfo[] };
				if (Array.isArray(data.locales) && data.locales.length > 0) {
					this.available = data.locales;
				}
			}
		} catch {
			// Keep fallback list in standalone Vite preview
		}
	}

	async init(preferredLocale?: string, gameLocale?: string): Promise<void> {
		if (gameLocale !== undefined) {
			this.detectedGameLocale = gameLocale;
		}
		if (!this.initPromise) {
			this.initPromise = (async () => {
				await this.refreshAvailableLocales();
				await this.loadDictionary('en');
			})();
		}
		await this.initPromise;
		const { setting, resolved } = this.detectInitialLocale(preferredLocale, this.detectedGameLocale);
		this.setting = setting;
		if (resolved !== 'en') {
			await this.loadDictionary(resolved);
		}
		this.applyLocaleState(resolved, setting);
		this.ready = true;
	}

	private applyLocaleState(code: string, settingCode = this.setting): void {
		this.locale = code;
		this.setting = settingCode;
		if (typeof document !== 'undefined') {
			document.documentElement.lang = code;
		}
		try {
			localStorage.setItem(STORAGE_KEY, settingCode);
		} catch {
			// Ignore storage errors
		}
	}

	async setLocale(code: string): Promise<void> {
		const isAuto = code.trim().toLowerCase() === 'auto';
		const resolved = isAuto
			? (this.detectInitialLocale('auto', this.detectedGameLocale).resolved)
			: (this.matchAvailable(code) ?? 'en');
		const settingCode = isAuto ? 'auto' : resolved;
		if (!this.dictionaries['en']) {
			await this.loadDictionary('en');
		}
		if (resolved !== 'en') {
			const ok = await this.loadDictionary(resolved, true);
			if (!ok) {
				this.applyLocaleState('en', settingCode);
				return;
			}
		}
		this.applyLocaleState(resolved, settingCode);
	}

	t(key: string, vars?: Record<string, string | number>): string {
		const activeDict = this.dictionaries[this.locale];
		const fallbackDict = this.dictionaries['en'];
		let template = activeDict?.[key] ?? fallbackDict?.[key] ?? key;
		if (vars) {
			for (const [k, v] of Object.entries(vars)) {
				template = template.replaceAll(`{${k}}`, String(v));
			}
		}
		return template;
	}
}

export const i18n = new I18nStore();

export function t(key: string, vars?: Record<string, string | number>): string {
	return i18n.t(key, vars);
}
