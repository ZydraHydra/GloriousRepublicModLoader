import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {
	isLaunchLocked,
	shouldPollProcess,
	runGuardedLaunch
} from '../src/launcher/ui/lib/stores/launchGuard.ts';

test('isLaunchLocked locks while launching or while game process is running', () => {
	assert.equal(isLaunchLocked(false, false), false);
	assert.equal(isLaunchLocked(false, undefined), false);
	assert.equal(isLaunchLocked(true, false), true);
	assert.equal(isLaunchLocked(false, true), true);
	assert.equal(isLaunchLocked(true, true), true);
});

test('shouldPollProcess keeps polling during watch window or while game is running', () => {
	assert.equal(shouldPollProcess(1000, false, 900), true);
	assert.equal(shouldPollProcess(1000, false, 1000), false);
	assert.equal(shouldPollProcess(1000, true, 5000), true);
	assert.equal(shouldPollProcess(0, false, 5000), false);
});

test('runGuardedLaunch blocks concurrent clicks while launch is in flight', async () => {
	let launching = false;
	let running = false;
	let fetchCalls = 0;
	let saveCalls = 0;
	let resolveFetch;

	const fetchFn = () => {
		fetchCalls += 1;
		return new Promise((resolve) => {
			resolveFetch = resolve;
		});
	};

	const makeDeps = (autoLoadSave) => ({
		isLocked: () => isLaunchLocked(launching, running),
		setLaunching: (v) => {
			launching = v;
		},
		saveIni: async () => {
			saveCalls += 1;
		},
		setStatus: () => {},
		fetchFn,
		gameExe: 'C:\\WRSR\\SOVIET64.exe',
		autoLoadSave,
		launchingStatus: 'Launching...',
		launchedStatus: 'Launched!',
		launchFailedStatus: 'Launch failed',
		onLaunchSuccess: async () => {
			running = true;
		}
	});

	const first = runGuardedLaunch(makeDeps('MyRepublic'));
	// Second click while saveIni/fetch is still in flight must be ignored.
	const second = await runGuardedLaunch(makeDeps());
	assert.equal(second, false);
	assert.equal(launching, true);

	resolveFetch(new Response(JSON.stringify({ ok: true, message: 'Launched' }), { status: 200 }));
	assert.equal(await first, true);
	assert.equal(fetchCalls, 1);
	assert.equal(saveCalls, 1);
	assert.equal(launching, false);
	assert.equal(running, true);

	// Subsequent click while game is still running must also be blocked.
	const third = await runGuardedLaunch(makeDeps('OtherSave'));
	assert.equal(third, false);
	assert.equal(fetchCalls, 1);

	// Once game exits, launch is allowed again.
	running = false;
	assert.equal(isLaunchLocked(launching, running), false);
});

test('runGuardedLaunch unlocks buttons when launch fails or throws', async () => {
	let launching = false;
	const statuses = [];

	const failResp = await runGuardedLaunch({
		isLocked: () => isLaunchLocked(launching, false),
		setLaunching: (v) => {
			launching = v;
		},
		saveIni: async () => {},
		setStatus: (m) => statuses.push(m),
		fetchFn: async () => new Response(JSON.stringify({ ok: false }), { status: 500 }),
		gameExe: '',
		launchingStatus: 'Launching...',
		launchedStatus: 'Launched!',
		launchFailedStatus: 'Launch failed',
		onLaunchSuccess: async () => {}
	});

	assert.equal(failResp, false);
	assert.equal(launching, false);
	assert.equal(statuses.at(-1), 'Launch failed');

	const threw = await runGuardedLaunch({
		isLocked: () => isLaunchLocked(launching, false),
		setLaunching: (v) => {
			launching = v;
		},
		saveIni: async () => {},
		setStatus: (m) => statuses.push(m),
		fetchFn: async () => {
			throw new Error('network error');
		},
		gameExe: '',
		launchingStatus: 'Launching...',
		launchedStatus: 'Launched!',
		launchFailedStatus: 'Launch failed',
		onLaunchSuccess: async () => {}
	});

	assert.equal(threw, false);
	assert.equal(launching, false);
	assert.equal(statuses.at(-1), 'Launch failed');
});

test('all launch and load-save buttons in Svelte views bind disabled={app.launchLocked}', () => {
	const root = process.cwd();
	const files = [
		path.join(root, 'src/launcher/ui/routes/+page.svelte'),
		path.join(root, 'src/launcher/ui/lib/views/SaveInspectorView.svelte')
	];

	for (const filePath of files) {
		const content = fs.readFileSync(filePath, 'utf8');
		const buttonBlocks = content.match(/<Button[\s\S]*?<\/Button>/g) || [];
		const launchButtons = buttonBlocks.filter((b) => b.includes('app.launchWrsr'));
		assert.ok(launchButtons.length > 0, `Expected launch buttons in ${filePath}`);
		for (const btn of launchButtons) {
			assert.ok(
				btn.includes('disabled={app.launchLocked}'),
				`Missing disabled={app.launchLocked} on button in ${filePath}:\n${btn}`
			);
		}
	}
});
