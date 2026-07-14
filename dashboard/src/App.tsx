import { useEffect, useMemo, useRef, useState } from 'react'
import type { FormEvent } from 'react'
import './App.css'

type Mode = 'wifi' | 'usb'
type ReadingState = 'ok' | 'out_of_range' | 'invalid' | 'no_data'
type DeviceStatus = 'connected' | 'disconnected' | 'reading' | 'error'
type LiveState = 'connecting' | 'live' | 'offline'

type AppConfig = {
  containerDepthCm: number
  containerName: string
  warningThresholdPercent: number
  criticalThresholdPercent: number
  preferredMode: Mode
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

type ReadingPayload = {
  distanceCm: number | null
  waterDepthCm: number | null
  waterPercent: number | null
  rawDurationUs: number | null
  readingState: ReadingState
  timestamp: string | null
  source: 'json' | 'text' | 'mock'
}

type PortPayload = {
  path: string
  manufacturer?: string
  friendlyName: string
}

type NetworkPayload = {
  configured: boolean
  connected: boolean
  ssid: string
  hostname: string
  ipAddress: string | null
  signalDbm: number
}

const defaultConfig: AppConfig = {
  containerDepthCm: 120,
  containerName: 'Main Tank',
  warningThresholdPercent: 35,
  criticalThresholdPercent: 15,
  preferredMode: 'usb',
}

const defaultStatus: StatusPayload = {
  mode: 'usb',
  deviceConnected: false,
  sensorHealthy: false,
  firmwareVersion: '—',
  serialPort: null,
  ipAddress: null,
  status: 'disconnected',
  message: 'Waiting for device connection.',
}

const defaultReading: ReadingPayload = {
  distanceCm: null,
  waterDepthCm: null,
  waterPercent: null,
  rawDurationUs: null,
  readingState: 'no_data',
  timestamp: null,
  source: 'mock',
}

const isViteDevelopment = window.location.port === '5173'
const defaultApiBase = isViteDevelopment
  ? `${window.location.protocol}//${window.location.hostname}:8787`
  : window.location.origin

function App() {
  const [apiBase, setApiBase] = useState(defaultApiBase)
  const [draftApiBase, setDraftApiBase] = useState(defaultApiBase)
  const [config, setConfig] = useState<AppConfig>(defaultConfig)
  const [status, setStatus] = useState<StatusPayload>(defaultStatus)
  const [reading, setReading] = useState<ReadingPayload>(defaultReading)
  const [history, setHistory] = useState<ReadingPayload[]>([])
  const [ports, setPorts] = useState<PortPayload[]>([])
  const [selectedPort, setSelectedPort] = useState('')
  const [configMessage, setConfigMessage] = useState('Changes are saved to the active device.')
  const [connectionMessage, setConnectionMessage] = useState('Contacting the bridge…')
  const [liveState, setLiveState] = useState<LiveState>('connecting')
  const [isSaving, setIsSaving] = useState(false)
  const [network, setNetwork] = useState<NetworkPayload | null>(null)
  const lastHistoryTimestamp = useRef<string | null>(null)

  const acceptReading = (nextReading: ReadingPayload) => {
    setReading(nextReading)
    if (!nextReading.timestamp || nextReading.timestamp === lastHistoryTimestamp.current) return
    lastHistoryTimestamp.current = nextReading.timestamp
    setHistory((current) => [nextReading, ...current].slice(0, 8))
  }

  const requestJson = async <T,>(path: string, init?: RequestInit): Promise<T> => {
    const response = await fetch(`${apiBase}${path}`, init)
    const data: unknown = await response.json()
    if (!response.ok) {
      const detail =
        typeof data === 'object' && data !== null && 'message' in data && typeof data.message === 'string'
          ? data.message
          : `Request failed (${response.status})`
      throw new Error(detail)
    }
    return data as T
  }

  useEffect(() => {
    let cancelled = false
    setLiveState('connecting')
    setConnectionMessage(`Contacting ${apiBase}…`)

    Promise.all([
      fetch(`${apiBase}/api/config`),
      fetch(`${apiBase}/api/status`),
      fetch(`${apiBase}/api/reading`),
    ])
      .then(async (responses) => {
        const failed = responses.find((response) => !response.ok)
        if (failed) throw new Error(`Device API returned ${failed.status}.`)
        return Promise.all(responses.map((response) => response.json()))
      })
      .then(([configData, statusData, readingData]) => {
        if (cancelled) return
        const nextStatus = statusData as StatusPayload
        setConfig(configData as AppConfig)
        setStatus(nextStatus)
        acceptReading(readingData as ReadingPayload)
        setSelectedPort(nextStatus.serialPort ?? '')
        setConnectionMessage(`API connected at ${apiBase}.`)

        if (nextStatus.mode === 'usb') {
          fetch(`${apiBase}/api/ports`)
            .then((response) => response.ok ? response.json() : { ports: [] })
            .then((portsData: { ports: PortPayload[] }) => {
              if (cancelled) return
              const nextPorts = portsData.ports
              setPorts(nextPorts)
              const usbPort = nextPorts.find((port) => /tty(USB|ACM)|COM\d+/i.test(port.path))
              setSelectedPort(nextStatus.serialPort ?? usbPort?.path ?? nextPorts[0]?.path ?? '')
            })
            .catch(() => setPorts([]))
        } else {
          setPorts([])
          fetch(`${apiBase}/api/network`)
            .then((response) => response.ok ? response.json() : null)
            .then((networkData: NetworkPayload | null) => {
              if (!cancelled) setNetwork(networkData)
            })
            .catch(() => setNetwork(null))
        }
      })
      .catch((error: unknown) => {
        if (cancelled) return
        setLiveState('offline')
        setStatus((current) => ({
          ...current,
          deviceConnected: false,
          sensorHealthy: false,
          status: 'error',
          message: error instanceof Error ? error.message : 'Unable to load device data.',
        }))
        setConnectionMessage(error instanceof Error ? error.message : 'Unable to load device data.')
      })

    return () => {
      cancelled = true
    }
  }, [apiBase])

  useEffect(() => {
    let socket: WebSocket | null = null
    let retryTimer: number | undefined
    let stopped = false

    const connect = () => {
      if (stopped) return
      setLiveState('connecting')
      socket = new WebSocket(`${apiBase.replace(/^http/, 'ws')}/ws`)

      socket.onopen = () => {
        setLiveState('live')
        setConnectionMessage(`Live updates connected at ${apiBase}.`)
      }

      socket.onmessage = (event) => {
        try {
          const payload = JSON.parse(event.data) as
            | { type: 'config'; data: AppConfig }
            | { type: 'status'; data: StatusPayload }
            | { type: 'reading'; data: ReadingPayload }
          if (payload.type === 'config') setConfig(payload.data)
          if (payload.type === 'status') setStatus(payload.data)
          if (payload.type === 'reading') acceptReading(payload.data)
        } catch {
          setConnectionMessage('A malformed live update was ignored.')
        }
      }

      socket.onclose = () => {
        if (stopped) return
        setLiveState('offline')
        setConnectionMessage('Live updates disconnected. Retrying…')
        retryTimer = window.setTimeout(connect, 2500)
      }
    }

    connect()
    return () => {
      stopped = true
      window.clearTimeout(retryTimer)
      socket?.close()
    }
  }, [apiBase])

  const alertTone = useMemo(() => {
    if (reading.readingState !== 'ok' || reading.waterPercent === null) return 'neutral'
    if (reading.waterPercent <= config.criticalThresholdPercent) return 'critical'
    if (reading.waterPercent <= config.warningThresholdPercent) return 'warning'
    return 'healthy'
  }, [config.criticalThresholdPercent, config.warningThresholdPercent, reading])

  const alert = useMemo(() => {
    if (reading.readingState === 'out_of_range') return { tone: 'critical', title: 'Sensor out of range', detail: 'No valid water percentage can be calculated.' }
    if (reading.readingState === 'invalid') return { tone: 'critical', title: 'Invalid sensor reading', detail: 'Check the sensor and container depth configuration.' }
    if (reading.readingState === 'no_data') return { tone: 'neutral', title: 'Waiting for a reading', detail: 'Connect the device to begin monitoring.' }
    if (alertTone === 'critical') return { tone: 'critical', title: 'Critical water level', detail: `Level is at or below ${config.criticalThresholdPercent}%.` }
    if (alertTone === 'warning') return { tone: 'warning', title: 'Water level warning', detail: `Level is at or below ${config.warningThresholdPercent}%.` }
    return null
  }, [alertTone, config.criticalThresholdPercent, config.warningThresholdPercent, reading.readingState])

  const handleConfigSubmit = async (event: FormEvent<HTMLFormElement>) => {
    event.preventDefault()
    if (config.containerDepthCm <= 0) {
      setConfigMessage('Container depth must be greater than zero.')
      return
    }
    if (config.criticalThresholdPercent > config.warningThresholdPercent) {
      setConfigMessage('Critical threshold must not exceed the warning threshold.')
      return
    }
    setIsSaving(true)
    try {
      const data = await requestJson<AppConfig>('/api/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(config),
      })
      setConfig(data)
      setConfigMessage(`Saved for ${data.containerName || 'this container'}.`)
    } catch (error) {
      setConfigMessage(error instanceof Error ? error.message : 'Failed to save configuration.')
    } finally {
      setIsSaving(false)
    }
  }

  const handleConnect = async () => {
    if (!selectedPort) return setConnectionMessage('Select a serial port first.')
    setConnectionMessage(`Opening ${selectedPort}…`)
    try {
      const data = await requestJson<StatusPayload>('/api/connection', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ port: selectedPort, baudRate: 115200 }),
      })
      setStatus(data)
      setConnectionMessage(`Connected to ${selectedPort}.`)
    } catch (error) {
      setConnectionMessage(error instanceof Error ? error.message : 'Failed to connect.')
    }
  }

  const handleDisconnect = async () => {
    try {
      const data = await requestJson<StatusPayload>('/api/connection', { method: 'DELETE' })
      setStatus(data)
      setConnectionMessage('Serial connection closed.')
    } catch (error) {
      setConnectionMessage(error instanceof Error ? error.message : 'Failed to disconnect.')
    }
  }

  const handleWifiReset = async () => {
    if (!window.confirm('Change Wi-Fi network? The sensor will restart in setup mode. Container settings will be kept.')) return
    try {
      await requestJson<{ message: string }>('/api/network/reset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ confirm: 'RESET_WIFI' }),
      })
      setConnectionMessage('Wi-Fi cleared. Join the WaterLevel-XXXX network to configure the sensor.')
    } catch (error) {
      setConnectionMessage(error instanceof Error ? error.message : 'Failed to reset Wi-Fi.')
    }
  }

  const percentLabel = reading.waterPercent === null || reading.readingState !== 'ok' ? '—' : `${reading.waterPercent.toFixed(1)}%`
  const fillPercent = reading.waterPercent === null || reading.readingState !== 'ok' ? 0 : reading.waterPercent
  const updatedLabel = reading.timestamp ? new Date(reading.timestamp).toLocaleString() : 'No reading yet'
  const endpoint = status.serialPort ?? status.ipAddress ?? 'No device endpoint'

  return (
    <main className="app-shell">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark" aria-hidden="true">◒</span>
          <div><strong>Waterwatch</strong><small>ESP32 monitor</small></div>
        </div>
        <div className="topbar-status">
          <span className={`live-dot ${liveState}`} />
          <span>{liveState === 'live' ? 'Live updates' : liveState === 'connecting' ? 'Connecting' : 'Offline'}</span>
          <span className={`status-badge ${status.status}`}>{status.status}</span>
        </div>
      </header>

      <section className="overview">
        <div className="overview-copy">
          <p className="eyebrow">Live container overview</p>
          <h1>{config.containerName || 'Unnamed Container'}</h1>
          <p>Current level based on a {config.containerDepthCm.toFixed(1)} cm measurable depth.</p>
          <div className="connection-line"><span>{status.mode === 'usb' ? 'USB' : 'Wi‑Fi'}</span><strong>{endpoint}</strong></div>
        </div>

        <article className={`level-card ${alertTone}`}>
          <div className="tank" aria-hidden="true">
            <div className="tank-fill" style={{ height: `${fillPercent}%` }}><span /></div>
          </div>
          <div className="level-readout">
            <span>Water remaining</span>
            <strong>{percentLabel}</strong>
            <small>{reading.readingState === 'ok' ? `${reading.waterDepthCm?.toFixed(1)} cm of water` : 'No Reading'}</small>
          </div>
        </article>
      </section>

      {alert && <section className={`alert-banner ${alert.tone}`} role="alert"><span>!</span><div><strong>{alert.title}</strong><p>{alert.detail}</p></div></section>}

      <section className="metric-grid" aria-label="Current measurements">
        <article><span>Water depth</span><strong>{reading.waterDepthCm === null ? '—' : `${reading.waterDepthCm.toFixed(1)} cm`}</strong><small>Calculated remaining depth</small></article>
        <article><span>Sensor distance</span><strong>{reading.distanceCm === null ? '—' : `${reading.distanceCm.toFixed(1)} cm`}</strong><small>Sensor to water surface</small></article>
        <article><span>Device health</span><strong>{status.sensorHealthy ? 'Healthy' : 'Needs attention'}</strong><small>{status.message}</small></article>
        <article><span>Last update</span><strong>{reading.timestamp ? new Date(reading.timestamp).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : '—'}</strong><small>{updatedLabel}</small></article>
      </section>

      <section className="workspace-grid">
        <article className="panel settings-panel">
          <div className="panel-header"><div><p className="eyebrow">Configuration</p><h2>Container settings</h2></div><p>{configMessage}</p></div>
          <form className="config-form" onSubmit={handleConfigSubmit}>
            <label className="wide">Container name<input value={config.containerName} onChange={(event) => setConfig((current) => ({ ...current, containerName: event.target.value }))} placeholder="Rain Barrel" /></label>
            <label>Container depth <span>cm</span><input type="number" min="0.1" step="0.1" value={config.containerDepthCm} onChange={(event) => setConfig((current) => ({ ...current, containerDepthCm: Number(event.target.value) }))} /></label>
            <label>Preferred connection<select value={config.preferredMode} onChange={(event) => setConfig((current) => ({ ...current, preferredMode: event.target.value as Mode }))}><option value="usb">Local USB bridge</option><option value="wifi">Wi‑Fi device</option></select></label>
            <label>Warning level <span>%</span><input type="number" min="0" max="100" value={config.warningThresholdPercent} onChange={(event) => setConfig((current) => ({ ...current, warningThresholdPercent: Number(event.target.value) }))} /></label>
            <label>Critical level <span>%</span><input type="number" min="0" max="100" value={config.criticalThresholdPercent} onChange={(event) => setConfig((current) => ({ ...current, criticalThresholdPercent: Number(event.target.value) }))} /></label>
            <button className="wide" type="submit" disabled={isSaving}>{isSaving ? 'Saving…' : 'Save configuration'}</button>
          </form>
        </article>

        <article className="panel connection-panel">
          <div className="panel-header"><div><p className="eyebrow">Connectivity</p><h2>Device connection</h2></div><span className={`status-badge ${status.status}`}>{status.status}</span></div>
          <div className="connection-summary"><span>Mode<strong>{status.mode.toUpperCase()}</strong></span><span>Firmware<strong>{status.firmwareVersion}</strong></span><span>Payload<strong>{reading.source.toUpperCase()}</strong></span></div>
          {status.mode === 'wifi' && network && <div className="wifi-summary"><span>Network<strong>{network.ssid || 'Not configured'}</strong></span><span>Signal<strong>{network.connected ? `${network.signalDbm} dBm` : 'Offline'}</strong></span><button type="button" className="secondary" onClick={handleWifiReset}>Change Wi-Fi network</button></div>}
          {status.mode === 'usb' && <><label>Serial port<select value={selectedPort} onChange={(event) => setSelectedPort(event.target.value)}><option value="">Select a port</option>{ports.map((port) => <option key={port.path} value={port.path}>{port.friendlyName}</option>)}</select></label>
          <div className="button-row"><button type="button" onClick={handleConnect}>Connect USB</button><button type="button" className="secondary" onClick={handleDisconnect}>Disconnect</button></div></>}
          <form className="api-form" onSubmit={(event) => { event.preventDefault(); setApiBase(draftApiBase.replace(/\/$/, '')) }}><label>Device API URL<input type="url" value={draftApiBase} onChange={(event) => setDraftApiBase(event.target.value)} /></label><button type="submit" className="secondary">Apply</button></form>
          <p className="connection-message">{connectionMessage}</p>
        </article>
      </section>

      <section className="panel history-panel">
        <div className="panel-header"><div><p className="eyebrow">Recent activity</p><h2>Latest readings</h2></div><p>Session history · newest first</p></div>
        {history.length === 0 ? <div className="empty-state">Readings will appear here when the sensor begins reporting.</div> : <div className="history-list">{history.map((item) => <div key={item.timestamp}><span className={`reading-dot ${item.readingState}`} /><time>{item.timestamp ? new Date(item.timestamp).toLocaleTimeString() : '—'}</time><strong>{item.waterPercent === null || item.readingState !== 'ok' ? 'No Reading' : `${item.waterPercent.toFixed(1)}%`}</strong><span>{item.distanceCm === null ? '—' : `${item.distanceCm.toFixed(1)} cm distance`}</span><small>{item.readingState.replaceAll('_', ' ')}</small></div>)}</div>}
      </section>
    </main>
  )
}

export default App
