export interface LaunchDeps {
	isLocked: () => boolean;
	setLaunching: (value: boolean) => void;
	saveIni: () => Promise<void>;
	setStatus: (msg: string) => void;
	fetchFn: typeof fetch;
	gameExe: string;
	autoLoadSave?: string;
	launchingStatus: string;
	launchedStatus: string;
	launchFailedStatus: string;
	onLaunchSuccess: () => Promise<void>;
}

/**
 * Returns true when launching is in flight or the game process is running.
 */
export function isLaunchLocked(launching: boolean, diagRunning?: boolean): boolean {
	return launching || Boolean(diagRunning);
}

/**
 * Returns true when background polling should query live process status.
 */
export function shouldPollProcess(
	launchWatchUntil: number,
	diagRunning?: boolean,
	now: number = Date.now()
): boolean {
	return now < launchWatchUntil || Boolean(diagRunning);
}

/**
 * Executes a game/save launch only if not already locked, ensuring the lock
 * stays active across config save, /api/launch, and initial process poll.
 */
export async function runGuardedLaunch(deps: LaunchDeps): Promise<boolean> {
	if (deps.isLocked()) return false;
	deps.setLaunching(true);
	try {
		await deps.saveIni();
		deps.setStatus(deps.launchingStatus);
		const resp = await deps.fetchFn('/api/launch', {
			method: 'POST',
			headers: { 'content-type': 'application/json' },
			body: JSON.stringify({
				gameExe: deps.gameExe.trim(),
				autoLoadSave: deps.autoLoadSave || ''
			})
		});
		const data = (await resp.json()) as { ok?: boolean; message?: string };
		deps.setStatus(data.message || (data.ok ? deps.launchedStatus : deps.launchFailedStatus));
		if (data.ok) {
			await deps.onLaunchSuccess();
			return true;
		}
		return false;
	} catch {
		deps.setStatus(deps.launchFailedStatus);
		return false;
	} finally {
		deps.setLaunching(false);
	}
}
