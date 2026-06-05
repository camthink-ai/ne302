// Dynamically read Web app version and execute OTA packaging (CommonJS version)
const path = require('path');
const { spawnSync } = require('child_process');

function main() {
  const pkg = require('../package.json');
  const rawVersion = pkg.version || '0.0.0';

  // Normalize version to four segments: major.minor.patch.build (pad with 0 if insufficient, truncate if excessive, treat non-numeric as 0)
  function toFourPartVersion(v) {
    const parts = String(v).split('.');
    const nums = [];
    for (let i = 0; i < 4; i++) {
      const p = parts[i];
      const n = Number.parseInt(p, 10);
      nums.push(Number.isNaN(n) ? 0 : n);
    }
    return nums.join('.');
  }

  const version = toFourPartVersion(rawVersion);

  const scriptCwd = path.resolve(__dirname, '../../Script');
  const inputBin = path.resolve(__dirname, '../firmware_assets/web-assets.bin');
  const outputBin = path.resolve(__dirname, `../firmware_assets/web-assets_v${version}_ota.bin`);

  const args = [
    'ota_packer.py',
    inputBin,
    '-o', outputBin,
    '-t', 'web',
    '-n', 'NE302_WEB',
    '-d', 'NE302 Web User Interface',
    '-v', version,
  ];

  const res = spawnSync('python', args, { stdio: 'inherit', cwd: scriptCwd, shell: false });
  if (res.error) {
    console.error('[pack_web_assets] Execution failed:', res.error.message);
    process.exit(1);
  }
  if (res.status !== 0) {
    console.error('[pack_web_assets] Packaging command exit code:', res.status);
    process.exit(res.status);
  }
}

main();


