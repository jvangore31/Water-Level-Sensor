import fs from 'node:fs'
import path from 'node:path'
import http from 'node:http'
import { fileURLToPath } from 'node:url'
import express from 'express'
import cors from 'cors'
import { WebSocketServer } from 'ws'
import { SerialPort } from 'serialport'
import { z } from 'zod'

type Mode = 'wifi' | 'usb'
type DeviceStatus = 'connected' | 'disconnected' | 'reading' | 'error'
type ReadingState = 'ok' | 'out_of_range' | 'invalid' | 'no_data'
type ReadingSource = 'json' | 'text' | 'mock'

type AppConfig = {
  containerDepthCm: number
  containerName: string
  warningThresholdPercent: number
  criticalThresholdPercent: number
  preferredMode: Mode
}

type ReadingPayload = {
  distanceCm: number | null
  waterDepthCm: number | null
  waterPercent: number | null
  rawDurationUs: number | null
  readingState: ReadingState
  timestamp: string | null
  source: ReadingSource
}

type StatusPayload = {
  mode: Mode
  deviceConnected: boolean
  sensorHealthy: boolean
  firmwareVersion: string
  serialPort: string | null
  ipAddress: string | null
  status: DeviceStatus
  message: string
}

const rootDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const dataDir = path.join(rootDir, '.data')
const configFile = path.join(dataDir, 'config.json')
const portSchema = z.object({
  port: z.string().min(1),
  baudRate: z.number().int().positive().default(115200),
})

const configSchema = z.object({
  containerDepthCm: z.number().positive(),
  containerName: z.string().default('Main Tank'),
  warningThresholdPercent: z.number().min(0).max(100),
  criticalThresholdPercent: z.number().min(0).max(100),
  preferredMode: z.enum(['wifi', 'usb']),
})

const defaultConfig: AppConfig = {
  containerDepthCm: 120,
  containerName: 'Main Tank',
  warningThresholdPercent: 35,
  criticalThresholdPercent: 15,
  preferredMode: 'usb',
}

let config = loadConfig()
let latestReading: ReadingPayload = {
  distanceCm: null,
  waterDepthCm: null,
  waterPercent: null,
  rawDurationUs: null,
  readingState: 'no_data',
  timestamp: null,
  source: 'mock',
}

let status: StatusPayload = {
  mode: config.preferredMode,
  deviceConnected: false,
  sensorHealthy: false,
  firmwareVersion: 'bridge-local',
  serialPort: null,
  ipAddress: null,
  status: 'disconnected',
  message: 'Bridge is running. Connect a serial port.',
}

let serialPort: SerialPort | null = null
let serialBuffer = ''
let lastRawLine = ''

const app = express()
const server = http.createServer(app)
const wss = new WebSocketServer({ server, path: '/ws' })

app.use(cors())
app.use(express.json())

app.get('/api/status', (_req, res) => {
  res.json(status)
})

app.get('/api/config', (_req, res) => {
  res.json(config)
})

app.put('/api/config', (req, res) => {
  const parsed = configSchema.safeParse(req.body)

  if (!parsed.success) {
    res.status(400).json({ error: parsed.error.flatten() })
    return
  }

  config = parsed.data
  persistConfig(config)
  latestReading = recalculateReading(latestReading)
  status.mode = config.preferredMode
  broadcast('config', config)
  broadcast('reading', latestReading)
  broadcast('status', status)
  res.json(config)
})

app.get('/api/reading', (_req, res) => {
  res.json(latestReading)
})

app.get('/api/ports', async (_req, res) => {
  const ports = await SerialPort.list()
  res.json({
    ports: ports.map((port) => ({
      path: port.path,
      manufacturer: port.manufacturer ?? undefined,
      friendlyName: `${port.path}${port.manufacturer ? ` (${port.manufacturer})` : ''}`,
    })),
  })
})

app.post('/api/connection', async (req, res) => {
  const parsed = portSchema.safeParse(req.body)

  if (!parsed.success) {
    res.status(400).json({ error: parsed.error.flatten() })
    return
  }

  try {
    await connectSerial(parsed.data.port, parsed.data.baudRate)
    res.json(status)
  } catch (error) {
    status = {
      ...status,
      deviceConnected: false,
      sensorHealthy: false,
      status: 'error',
      message: error instanceof Error ? error.message : 'Failed to connect to serial port.',
    }
    broadcast('status', status)
    res.status(500).json(status)
  }
})

app.delete('/api/connection', async (_req, res) => {
  await disconnectSerial()
  res.json(status)
})

wss.on('connection', (socket) => {
  socket.send(JSON.stringify({ type: 'config', data: config }))
  socket.send(JSON.stringify({ type: 'status', data: status }))
  socket.send(JSON.stringify({ type: 'reading', data: latestReading }))
})

server.listen(8787, () => {
  console.log('Bridge listening on http://127.0.0.1:8787')
})

async function connectSerial(portPath: string, baudRate: number) {
  await disconnectSerial()

  serialBuffer = ''
  lastRawLine = ''
  serialPort = new SerialPort({ path: portPath, baudRate, autoOpen: true })

  serialPort.on('data', (chunk: Buffer) => {
    serialBuffer += chunk.toString('utf8')

    let newlineIndex = serialBuffer.indexOf('\n')
    while (newlineIndex >= 0) {
      const line = serialBuffer.slice(0, newlineIndex).trim()
      serialBuffer = serialBuffer.slice(newlineIndex + 1)
      if (line) {
        ingestLine(line)
      }
      newlineIndex = serialBuffer.indexOf('\n')
    }
  })

  serialPort.on('error', (error) => {
    status = {
      ...status,
      deviceConnected: false,
      sensorHealthy: false,
      serialPort: portPath,
      status: 'error',
      message: error.message,
    }
    broadcast('status', status)
  })

  status = {
    ...status,
    mode: 'usb',
    deviceConnected: true,
    sensorHealthy: true,
    serialPort: portPath,
    status: 'connected',
    message: `Connected to ${portPath} at ${baudRate} baud.`,
  }
  broadcast('status', status)
}

async function disconnectSerial() {
  if (serialPort) {
    const openPort = serialPort
    serialPort = null
    await new Promise<void>((resolve) => {
      openPort.close(() => resolve())
    })
  }

  status = {
    ...status,
    deviceConnected: false,
    sensorHealthy: false,
    serialPort: null,
    status: 'disconnected',
    message: 'Serial connection closed.',
  }
  broadcast('status', status)
}

function ingestLine(line: string) {
  lastRawLine = line
  const now = new Date().toISOString()

  let nextReading: ReadingPayload | null = null
  const jsonCandidate = parseJsonLine(line, now)
  if (jsonCandidate) {
    nextReading = jsonCandidate
  }

  if (!nextReading) {
    nextReading = parseTextLine(line, now)
  }

  if (!nextReading) {
    status = {
      ...status,
      sensorHealthy: false,
      status: 'error',
      message: `Unsupported serial payload: ${lastRawLine}`,
    }
    broadcast('status', status)
    return
  }

  latestReading = recalculateReading(nextReading)
  status = {
    ...status,
    sensorHealthy: latestReading.readingState === 'ok',
    status: latestReading.readingState === 'ok' ? 'reading' : 'error',
    message: `Received ${latestReading.source} sensor update.`,
  }
  broadcast('reading', latestReading)
  broadcast('status', status)
}

function parseJsonLine(line: string, timestamp: string): ReadingPayload | null {
  try {
    const parsed = JSON.parse(line) as {
      distanceCm?: number
      rawDurationUs?: number
      readingState?: ReadingState
      timestamp?: string
    }

    if (typeof parsed.distanceCm !== 'number') {
      return null
    }

    return {
      distanceCm: parsed.distanceCm,
      waterDepthCm: null,
      waterPercent: null,
      rawDurationUs: typeof parsed.rawDurationUs === 'number' ? parsed.rawDurationUs : null,
      readingState: parsed.readingState ?? 'ok',
      timestamp: parsed.timestamp ?? timestamp,
      source: 'json',
    }
  } catch {
    return null
  }
}

function parseTextLine(line: string, timestamp: string): ReadingPayload | null {
  const match = line.match(/Distance:\s*([0-9]+(?:\.[0-9]+)?)\s*cm/i)
  if (!match) {
    return null
  }

  return {
    distanceCm: Number(match[1]),
    waterDepthCm: null,
    waterPercent: null,
    rawDurationUs: null,
    readingState: 'ok',
    timestamp,
    source: 'text',
  }
}

function recalculateReading(reading: ReadingPayload): ReadingPayload {
  if (reading.distanceCm === null || !Number.isFinite(reading.distanceCm)) {
    return {
      ...reading,
      waterDepthCm: null,
      waterPercent: null,
      readingState: reading.readingState === 'no_data' ? 'no_data' : 'invalid',
    }
  }

  if (reading.distanceCm < 0 || config.containerDepthCm <= 0) {
    return {
      ...reading,
      waterDepthCm: null,
      waterPercent: null,
      readingState: 'invalid',
    }
  }

  if (reading.distanceCm > config.containerDepthCm) {
    return {
      ...reading,
      waterDepthCm: 0,
      waterPercent: 0,
      readingState: 'out_of_range',
    }
  }

  const waterDepthCm = Math.max(config.containerDepthCm - reading.distanceCm, 0)
  const waterPercent = clamp((waterDepthCm / config.containerDepthCm) * 100, 0, 100)

  return {
    ...reading,
    waterDepthCm,
    waterPercent,
    readingState: 'ok',
  }
}

function clamp(value: number, min: number, max: number) {
  return Math.min(Math.max(value, min), max)
}

function broadcast(type: 'config' | 'status' | 'reading', data: unknown) {
  const payload = JSON.stringify({ type, data })
  for (const client of wss.clients) {
    if (client.readyState === 1) {
      client.send(payload)
    }
  }
}

function loadConfig(): AppConfig {
  fs.mkdirSync(dataDir, { recursive: true })

  if (!fs.existsSync(configFile)) {
    fs.writeFileSync(configFile, JSON.stringify(defaultConfig, null, 2))
    return defaultConfig
  }

  try {
    const parsed = JSON.parse(fs.readFileSync(configFile, 'utf8'))
    return configSchema.parse(parsed)
  } catch {
    fs.writeFileSync(configFile, JSON.stringify(defaultConfig, null, 2))
    return defaultConfig
  }
}

function persistConfig(nextConfig: AppConfig) {
  fs.mkdirSync(dataDir, { recursive: true })
  fs.writeFileSync(configFile, JSON.stringify(nextConfig, null, 2))
}
