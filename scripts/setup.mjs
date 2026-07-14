import { spawnSync } from 'node:child_process'
import process from 'node:process'

const major = Number(process.versions.node.split('.')[0])
if (major < 20) {
  console.error(`Node.js 20 or newer is required. Found ${process.version}.`)
  process.exit(1)
}

const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm'
for (const directory of ['bridge', 'dashboard']) {
  console.log(`\nInstalling ${directory} dependencies...`)
  const result = spawnSync(npm, ['ci'], { cwd: directory, stdio: 'inherit' })
  if (result.status !== 0) process.exit(result.status ?? 1)
}

console.log('\nSetup complete. Connect the ESP32 and run: npm start')
