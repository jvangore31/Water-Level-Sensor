import { spawn } from 'node:child_process'
import process from 'node:process'

const npm = process.platform === 'win32' ? 'npm.cmd' : 'npm'
const children = [
  spawn(npm, ['start'], { cwd: 'bridge', stdio: 'inherit', shell: process.platform === 'win32' }),
  spawn(npm, ['run', 'dev'], { cwd: 'dashboard', stdio: 'inherit', shell: process.platform === 'win32' }),
]

let stopping = false
const stop = () => {
  if (stopping) return
  stopping = true
  for (const child of children) child.kill('SIGTERM')
  setTimeout(() => process.exit(), 500).unref()
}

process.on('SIGINT', stop)
process.on('SIGTERM', stop)

for (const child of children) {
  child.on('exit', (code) => {
    if (!stopping && code && code !== 0) {
      console.error(`A development service exited with code ${code}.`)
      stop()
      process.exitCode = code
    }
  })
}
