#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';

const ROOT = path.resolve(import.meta.dirname, '..');
const LOCALES_DIR = path.join(ROOT, 'src', 'launcher', 'ui', 'static', 'locales');
const UI_SRC_DIR = path.join(ROOT, 'src', 'launcher', 'ui');

function flatten(obj, prefix = '', out = {}) {
	for (const [k, v] of Object.entries(obj)) {
		if (k === '_meta') continue;
		const fullKey = prefix ? `${prefix}.${k}` : k;
		if (typeof v === 'string') {
			out[fullKey] = v;
		} else if (v && typeof v === 'object' && !Array.isArray(v)) {
			flatten(v, fullKey, out);
		} else {
			throw new Error(`Invalid value type at key "${fullKey}"`);
		}
	}
	return out;
}

function walkUiFiles(dir, out = []) {
	for (const ent of fs.readdirSync(dir, { withFileTypes: true })) {
		const full = path.join(dir, ent.name);
		if (ent.isDirectory()) {
			if (ent.name === 'static' || ent.name === 'node_modules') continue;
			walkUiFiles(full, out);
		} else if (ent.isFile() && (ent.name.endsWith('.svelte') || ent.name.endsWith('.ts'))) {
			out.push(full);
		}
	}
	return out;
}

function extractUiKeys(files) {
	const usedKeys = new Set();
	// Matches t('key') or t("key") or t(cond ? 'key.a' : 'key.b', ...)
	const callRe = /\bt\s*\(([^)]+)\)/g;
	const strRe = /['"]([a-zA-Z0-9_]+\.[a-zA-Z0-9_.]+)['"]/g;

	for (const file of files) {
		const src = fs.readFileSync(file, 'utf8');
		let m;
		while ((m = callRe.exec(src)) !== null) {
			const args = m[1];
			let sm;
			while ((sm = strRe.exec(args)) !== null) {
				usedKeys.add(sm[1]);
			}
		}
	}
	return [...usedKeys].sort();
}

function extractPlaceholders(str) {
	return (str.match(/\{[a-zA-Z0-9_]+\}/g) || []).sort().join(',');
}

let errors = 0;
const fail = (msg) => {
	console.error(`ERROR: ${msg}`);
	errors++;
};

if (!fs.existsSync(LOCALES_DIR)) {
	fail(`Locales directory not found: ${LOCALES_DIR}`);
	process.exit(1);
}

const localeFiles = fs.readdirSync(LOCALES_DIR).filter((f) => f.endsWith('.json')).sort();
if (!localeFiles.includes('en.json')) {
	fail('Base locale en.json is missing.');
	process.exit(1);
}

const enRaw = JSON.parse(fs.readFileSync(path.join(LOCALES_DIR, 'en.json'), 'utf8'));
const enFlat = flatten(enRaw);
const enKeys = Object.keys(enFlat);

// 1. Verify every t('...') key referenced in the Svelte/TS UI exists in en.json
const uiFiles = walkUiFiles(UI_SRC_DIR);
const usedUiKeys = extractUiKeys(uiFiles);
for (const key of usedUiKeys) {
	if (!(key in enFlat)) {
		fail(`UI references missing key "${key}" not found in en.json`);
	}
}

// 2. Validate every locale file against en.json
for (const file of localeFiles) {
	const fullPath = path.join(LOCALES_DIR, file);
	const expectedCode = file.slice(0, -5);
	let parsed;
	try {
		parsed = JSON.parse(fs.readFileSync(fullPath, 'utf8'));
	} catch (e) {
		fail(`${file}: invalid JSON (${e.message})`);
		continue;
	}

	const meta = parsed._meta;
	if (!meta || typeof meta !== 'object') {
		fail(`${file}: missing _meta object`);
	} else {
		if (meta.code !== expectedCode) {
			fail(`${file}: _meta.code "${meta.code}" does not match filename "${expectedCode}"`);
		}
		if (!meta.name || typeof meta.name !== 'string') {
			fail(`${file}: missing _meta.name`);
		}
		if (!meta.nativeName || typeof meta.nativeName !== 'string') {
			fail(`${file}: missing _meta.nativeName`);
		}
	}

	let flat;
	try {
		flat = flatten(parsed);
	} catch (e) {
		fail(`${file}: ${e.message}`);
		continue;
	}

	for (const k of enKeys) {
		if (!(k in flat)) {
			fail(`${file}: missing key "${k}"`);
		} else if (!flat[k].trim()) {
			fail(`${file}: empty translation for key "${k}"`);
		} else {
			const phEn = extractPlaceholders(enFlat[k]);
			const phCur = extractPlaceholders(flat[k]);
			if (phEn !== phCur) {
				fail(`${file}: placeholder mismatch for "${k}" (expected [${phEn}], got [${phCur}])`);
			}
		}
	}

	for (const k of Object.keys(flat)) {
		if (!(k in enFlat)) {
			fail(`${file}: extra/unused key "${k}" not present in en.json`);
		}
	}
}

if (errors > 0) {
	console.error(`\ni18n validation failed with ${errors} error(s).`);
	process.exit(1);
}

console.log(
	`i18n validation passed: ${localeFiles.length} locale(s), ${enKeys.length} keys, ${usedUiKeys.length} UI key references verified.`
);
