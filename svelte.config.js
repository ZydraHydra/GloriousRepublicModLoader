import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),
	kit: {
		outDir: 'build/.svelte-kit',
		files: {
			assets: 'src/launcher/ui/static',
			lib: 'src/launcher/ui/lib',
			routes: 'src/launcher/ui/routes',
			appTemplate: 'src/launcher/ui/app.html'
		},
		adapter: adapter({
			pages: 'build/dist',
			assets: 'build/dist',
			fallback: 'index.html',
			precompress: false,
			strict: true
		})
	}
};

export default config;
