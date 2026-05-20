import { readFileSync, existsSync } from 'fs';
import path from 'path';
import type { Plugin } from 'vite';

/**
 * Read WEB version from version.mk (single source of truth)
 * Reads WEB_VERSION_OVERRIDE and WEB_SUFFIX, falls back to main version
 * Falls back to package.json version if version.mk is not available
 */
function getWebVersion(): string {
  const versionMkPath = path.resolve(process.cwd(), '../version.mk');
  
  if (existsSync(versionMkPath)) {
    try {
      const content = readFileSync(versionMkPath, 'utf-8');
      
      // Try to read WEB_VERSION_OVERRIDE first
      const webVersionOverride = content.match(/WEB_VERSION_OVERRIDE\s*:?=\s*(\S+)/)?.[1]?.trim();
      
      let resolvedVersion: string;
      if (webVersionOverride && webVersionOverride !== '' && webVersionOverride !== '$(VERSION)') {
        // Use WEB component override version
        resolvedVersion = webVersionOverride;
      } else {
        // Use main version
        const major = content.match(/VERSION_MAJOR\s*:?=\s*(\d+)/)?.[1] || '0';
        const minor = content.match(/VERSION_MINOR\s*:?=\s*(\d+)/)?.[1] || '0';
        const patch = content.match(/VERSION_PATCH\s*:?=\s*(\d+)/)?.[1] || '0';
        const build = content.match(/VERSION_BUILD\s*\??=\s*(\d+)/)?.[1] || '0';
        resolvedVersion = `${major}.${minor}.${patch}.${build}`;
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
      
      // Append suffix if present (use underscore for display)
      return suffix ? `${resolvedVersion}_${suffix}` : resolvedVersion;
    } catch {
      console.warn('Failed to read version.mk, falling back to package.json');
    }
  }
  
  // Fallback to package.json
  const packageJsonPath = path.resolve(process.cwd(), 'package.json');
  const packageJson = JSON.parse(readFileSync(packageJsonPath, 'utf-8'));
  return packageJson.version;
}

export default function version(): Plugin {
  return {
    name: 'version-log',
    transformIndexHtml(html: string) {
      // Read version from version.mk (single source of truth)
      const appVersion = getWebVersion();

      // Inject version output script before </body> tag
      const versionScript = `
    <script>
      (function() {
        var v = '${appVersion}';
        if (typeof console !== 'undefined' && console.log) {
          console.log(
            '%c🚀 %cVersion:%c ' + v + ' %c',
            'font-size: 16px;',
            'color: #666; font-weight: bold; font-size: 14px; padding: 4px 8px; background: #f0f0f0; border-radius: 4px 0 0 4px;',
            'color: #4CAF50; font-weight: bold; font-size: 14px; padding: 4px 8px; background: #f0f0f0; border-radius: 0 4px 4px 0;',
            ''
          );
        }
      })();
    </script>`;
      
      return html.replace('</body>', `${versionScript}\n  </body>`);
    },
  };
}
