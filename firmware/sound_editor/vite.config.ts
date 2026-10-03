import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';

export default defineConfig({
  base: '/sound-editor/',
  plugins: [
    svelte(),
    {
      name: 'bundled-package-notices',
      generateBundle(_options, bundle) {
        const packages = new Set<string>();
        for (const output of Object.values(bundle)) {
          if (output.type !== 'chunk') continue;
          for (const id of Object.keys(output.modules)) {
            const path = id.replaceAll('\\', '/').split('/node_modules/').at(-1);
            if (!id.includes('node_modules') || !path) continue;
            const parts = path.split('/');
            packages.add(parts.slice(0, path.startsWith('@') ? 2 : 1).join('/'));
          }
        }
        this.emitFile({ type: 'asset', fileName: 'bundled-packages.json', source: JSON.stringify([...packages].sort()) });
      },
    },
  ],
  build: { target: 'es2022', sourcemap: false },
});
