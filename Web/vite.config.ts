import { defineConfig, loadEnv } from 'vite';
import preact from '@preact/preset-vite';
import tailwindcss from '@tailwindcss/vite';
import { lingui } from '@lingui/vite-plugin';
import path from 'path';
import createPlugins from './vitePlugins';
import { readFileSync, existsSync } from 'fs';

/**
 * Read WEB version from version.mk (single source of truth)
 * Reads WEB_VERSION_OVERRIDE and WEB_SUFFIX, falls back to main version
 * Falls back to package.json version if version.mk is not available
 */
function getProjectVersion(): string {
  const versionMkPath = path.resolve(__dirname, '../version.mk');
  
  if (existsSync(versionMkPath)) {
    try {
      const content = readFileSync(versionMkPath, 'utf-8');
      
      // Try to read WEB_VERSION_OVERRIDE first
      const webVersionOverride = content.match(/WEB_VERSION_OVERRIDE\s*:?=\s*(\S+)/)?.[1]?.trim();
      
      let version: string;
      if (webVersionOverride && webVersionOverride !== '') {
        // Use WEB component override version
        version = webVersionOverride;
      } else {
        // Use main version
        const major = content.match(/VERSION_MAJOR\s*:?=\s*(\d+)/)?.[1] || '0';
        const minor = content.match(/VERSION_MINOR\s*:?=\s*(\d+)/)?.[1] || '0';
        const patch = content.match(/VERSION_PATCH\s*:?=\s*(\d+)/)?.[1] || '0';
        const build = content.match(/VERSION_BUILD\s*\??=\s*(\d+)/)?.[1] || '0';
        version = `${major}.${minor}.${patch}.${build}`;
      }
      
      // Handle suffix: check WEB_SUFFIX first, then VERSION_SUFFIX
      const webSuffix = content.match(/WEB_SUFFIX\s*:?=\s*(\S+)/)?.[1]?.trim();
      const mainSuffix = content.match(/VERSION_SUFFIX\s*:?=\s*(\S+)/)?.[1]?.trim();
      
      let suffix = '';
      if (webSuffix !== undefined) {
        // WEB_SUFFIX is explicitly set
        suffix = (webSuffix === 'NONE' || webSuffix === '') ? '' : webSuffix;
      } else if (mainSuffix && mainSuffix !== '') {
        // Use main suffix if WEB_SUFFIX not set
        suffix = mainSuffix;
      }
      
      // Append suffix if present
      return suffix ? `${version}_${suffix}` : version;
    } catch {
      console.warn('Failed to read version.mk, falling back to package.json');
    }
  }
  
  // Fallback to package.json
  const packageJson = JSON.parse(readFileSync(path.resolve(__dirname, 'package.json'), 'utf-8'));
  return packageJson.version;
}

// https://vite.dev/config/
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '')
  const appVersion = getProjectVersion();
  console.log(`Building with version: ${appVersion}`);
  
  return {
    base: env.VITE_APP_BASE || '/',
    define: {
      __APP_VERSION__: JSON.stringify(appVersion),
    },
    plugins: [
      preact(),
      tailwindcss(),
      lingui(),
      ...createPlugins
    ],
    server: {
      host: '0.0.0.0', // Listen on all network interfaces
      port: 5173,
      proxy: {
        '/api': {
          target: 'http://192.168.10.10',
          changeOrigin: true
        },
      },
    },
    resolve: {
      alias: {
        '@': path.resolve(__dirname, './src'),
        'react': path.resolve(__dirname, 'node_modules/preact/compat'),
        'react-dom': path.resolve(__dirname, 'node_modules/preact/compat'),
        "react-dom/test-utils": "preact/test-utils",
      },
    },
    esbuild: {
      jsx: 'automatic',
    },
    optimizeDeps: {
      include: ['@lingui/macro', 'zustand'],
    },
  };
});
