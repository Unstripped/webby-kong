import fs from 'node:fs';
import path from 'node:path';
import type { Plugin } from 'vite';
import { defineConfig } from 'vitest/config';

const root = import.meta.dirname;
const web = path.join(root, 'web');

// build/web also holds dk64.js, dk64.wasm and the CMake tree, so only the hashed bundles are cleared per build.
function cleanAssets(): Plugin {
  let outDir = '';
  return {
    name: 'clean-assets',
    configResolved(config) { outDir = path.resolve(config.root, config.build.outDir); },
    buildStart() { fs.rmSync(path.join(outDir, 'assets'), { recursive: true, force: true }); },
  };
}

export default defineConfig({
  root: web,
  base: '/',
  cacheDir: path.join(root, 'node_modules', '.vite'),
  publicDir: path.join(web, 'public'),
  plugins: [cleanAssets()],
  oxc: { jsx: { runtime: 'automatic', importSource: 'preact' } },
  build: {
    outDir: path.join(root, 'build', 'web'),
    emptyOutDir: false,
    assetsDir: 'assets',
    sourcemap: true,
    target: 'es2022',
    rollupOptions: {
      input: {
        game: path.join(web, 'index.html'),
        dashboard: path.join(web, 'dashboard', 'index.html'),
        auth: path.join(web, 'auth', 'index.html'),
      },
    },
  },
  test: {
    environment: 'happy-dom',
    environmentOptions: { happyDOM: { url: 'http://localhost/' } },
    include: ['src/**/*.test.{ts,tsx}'],
    restoreMocks: true,
  },
});
