import adapter from '@sveltejs/adapter-static';
import { vitePreprocess } from '@sveltejs/vite-plugin-svelte';

/** @type {import('@sveltejs/kit').Config} */
const config = {
	preprocess: vitePreprocess(),
	kit: {
		adapter: adapter({
			pages: '../../../build/dist',
			assets: '../../../build/dist',
			fallback: 'index.html',
			precompress: false,
			strict: true
		})
	}
};

export default config;
