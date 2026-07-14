import { useEffect, useMemo, useRef, useState } from 'react'
import type { FormEvent } from 'react'
import './App.css'

type Mode = 'wifi' | 'usb'
type ReadingState = 'ok' | 'pending_confirmation' | 'too_close' | 'out_of_range' | 'invalid' | 'stale' | 'no_data'
type DeviceStatus = 'connected' | 'disconnected' | 'reading' | 'error'
type LiveState = 'connecting' | 'live' | 'offline'

type AppConfig = {
  schemaVersion?: number
  containerDepthCm: number
  containerName: string
  warningThresholdPercent: number
  criticalThresholdPercent: number
  preferredMode: Mode
  measurement?: {
    calibrationMode: 'container_depth' | 'full_empty'
    fullDistanceCm: number | null
    emptyDistanceCm: number | null
    minimumValidDistanceCm: number
    maximumValidDistanceCm: number
    medianWindowSize: number
    maximumStepCm: number
    stepConfirmationSamples: number
    invalidSamplesBeforeFault: number
  }
  power?: {
    powerSavingEnabled: boolean
    sampleIntervalSeconds: number
    displayTimeoutSeconds: number
    scheduledSleepEnabled: boolean
    awakeWindowSeconds: number
    batteryMonitoringEnabled: boolean
    batteryLowVoltage: number
    batteryCriticalVoltage: number
    batteryCalibrationMultiplier: number
  }
  network?: { maintenanceApEnabled: boolean; maintenanceApDelaySeconds: number; maintenanceApIdleTimeoutSeconds: number }
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
  maintenanceApActive?: boolean
  scheduledSleepEnabled?: boolean
  clockSynchronized?: boolean
  batteryVoltage?: number | null
  batteryState?: string
}

type ReadingPayload = {
  distanceCm: number | null
  waterDepthCm: number | null
  waterPercent: number | null
  rawDurationUs: number | null
  readingState: ReadingState
  timestamp: string | null
  source: 'json' | 'text' | 'mock'
  rawDistanceCm?: number | null
  outsideCalibrationRange?: boolean
  consecutiveInvalidSamples?: number
  sampleSequence?: number
  uptimeMilliseconds?: number
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
  measurement: { calibrationMode: 'container_depth', fullDistanceCm: null, emptyDistanceCm: null, minimumValidDistanceCm: 20, maximumValidDistanceCm: 450, medianWindowSize: 5, maximumStepCm: 25, stepConfirmationSamples: 3, invalidSamplesBeforeFault: 3 },
  power: { powerSavingEnabled: false, sampleIntervalSeconds: 0.5, displayTimeoutSeconds: 0, scheduledSleepEnabled: false, awakeWindowSeconds: 30, batteryMonitoringEnabled: false, batteryLowVoltage: 3.4, batteryCriticalVoltage: 3.2, batteryCalibrationMultiplier: 1 },
  network: { maintenanceApEnabled: true, maintenanceApDelaySeconds: 30, maintenanceApIdleTimeoutSeconds: 900 },
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

const normalizeConfig = (value: AppConfig): AppConfig => ({
  ...defaultConfig,
  ...value,
  measurement: { ...defaultConfig.measurement!, ...value.measurement },
  power: { ...defaultConfig.power!, ...value.power },
  network: { ...defaultConfig.network!, ...value.network },
})

const isViteDevelopment = window.location.port === '5173'
const defaultApiBase = isViteDevelopment
  ? `${window.location.protocol}//${window.location.hostname}:8787`
  : window.location.origin

function App() {
  const [authenticated, setAuthenticated] = useState<boolean | null>(null)
  const [setupRequired, setSetupRequired] = useState(false)
  const [csrfToken, setCsrfToken] = useState('')
  const [loginPassword, setLoginPassword] = useState('')
  const [newPassword, setNewPassword] = useState('')
  const [apPassword, setApPassword] = useState('')
  const [authMessage, setAuthMessage] = useState('Enter the device administrator password.')
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
  const [diagnostics, setDiagnostics] = useState<Record<string, unknown> | null>(null)
  const [wifiSsid, setWifiSsid] = useState('')
  const [wifiPassword, setWifiPassword] = useState('')
  const [wifiHostname, setWifiHostname] = useState('water-level')
  const lastHistoryKey = useRef<string | null>(null)

  const acceptReading = (nextReading: ReadingPayload) => {
    setReading(nextReading)
    const historyKey = nextReading.sampleSequence ? `sample-${nextReading.sampleSequence}` : nextReading.timestamp
    if (!historyKey || historyKey === lastHistoryKey.current) return
    lastHistoryKey.current = historyKey
    setHistory((current) => [nextReading, ...current].slice(0, 8))
  }

  const requestJson = async <T,>(path: string, init?: RequestInit): Promise<T> => {
    const headers = new Headers(init?.headers)
    if (init?.method && init.method !== 'GET' && csrfToken) headers.set('X-Water-Level-CSRF', csrfToken)
    const response = await fetch(`${apiBase}${path}`, { ...init, headers, credentials: 'include' })
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
    fetch(`${apiBase}/api/auth/session`, { credentials: 'include' })
      .then(async (response) => {
        if (response.status === 404) return { authenticated: true, setupRequired: false, csrfToken: '' }
        if (!response.ok) throw new Error('Unable to check device login.')
        return response.json() as Promise<{ authenticated: boolean; setupRequired: boolean; csrfToken?: string }>
      })
      .then((session) => { setAuthenticated(session.authenticated); setSetupRequired(session.setupRequired); setCsrfToken(session.csrfToken ?? '') })
      .catch(() => { setAuthenticated(true); setSetupRequired(false) })
  }, [apiBase])

  const handleLogin = async (event: FormEvent) => {
    event.preventDefault()
    try {
      const session = await requestJson<{ authenticated: boolean; setupRequired: boolean; csrfToken: string }>('/api/auth/login', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ password: loginPassword }),
      })
      setCsrfToken(session.csrfToken); setSetupRequired(session.setupRequired); setAuthenticated(true); setLoginPassword('')
    } catch (error) { setAuthMessage(error instanceof Error ? error.message : 'Login failed.') }
  }

  const handleInitialCredentials = async (event: FormEvent) => {
    event.preventDefault()
    try {
      await requestJson('/api/auth/password', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ newPassword, maintenanceApPassword: apPassword }) })
      setSetupRequired(false); setNewPassword(''); setApPassword(''); setAuthMessage('Credentials saved.')
    } catch (error) { setAuthMessage(error instanceof Error ? error.message : 'Could not save credentials.') }
  }

  useEffect(() => {
    if (!authenticated || setupRequired) return
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
        setConfig(normalizeConfig(configData as AppConfig))
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
  }, [apiBase, authenticated, setupRequired])

  useEffect(() => {
    if (!authenticated || setupRequired) return
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
          if (payload.type === 'config') setConfig(normalizeConfig(payload.data))
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
  }, [apiBase, authenticated, setupRequired])

  const alertTone = useMemo(() => {
    if (reading.readingState !== 'ok' || reading.waterPercent === null) return 'neutral'
    if (reading.waterPercent <= config.criticalThresholdPercent) return 'critical'
    if (reading.waterPercent <= config.warningThresholdPercent) return 'warning'
    return 'healthy'
  }, [config.criticalThresholdPercent, config.warningThresholdPercent, reading])

  const alert = useMemo(() => {
    if (reading.readingState === 'stale') return { tone: 'critical', title: 'Reading is stale', detail: 'The sensor has not produced a fresh accepted sample.' }
    if (reading.readingState === 'too_close') return { tone: 'critical', title: 'Inside sensor blind zone', detail: 'Move the sensor farther from the highest water surface.' }
    if (reading.readingState === 'pending_confirmation') return { tone: 'neutral', title: 'Confirming level change', detail: 'A large change must repeat before it is accepted.' }
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

  const captureCalibration = async (point: 'full' | 'empty') => {
    if (!window.confirm(`Capture the current stable reading as the ${point} point?`)) return
    try {
      const next = await requestJson<AppConfig>(`/api/calibration/capture-${point}`, { method: 'POST' })
      setConfig(normalizeConfig(next)); setConfigMessage(`${point === 'full' ? 'Full' : 'Empty'} point captured.`)
    } catch (error) { setConfigMessage(error instanceof Error ? error.message : 'Calibration capture failed.') }
  }

  const loadDiagnostics = async () => {
    try { setDiagnostics(await requestJson<Record<string, unknown>>('/api/diagnostics')) }
    catch (error) { setConnectionMessage(error instanceof Error ? error.message : 'Diagnostics unavailable.') }
  }

  const handleNetworkSetup = async (event: FormEvent) => {
    event.preventDefault()
    try {
      await requestJson('/api/setup/connect', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ssid: wifiSsid, password: wifiPassword, hostname: wifiHostname }) })
      setWifiPassword(''); setConnectionMessage(`Connecting to ${wifiSsid}…`)
    } catch (error) { setConnectionMessage(error instanceof Error ? error.message : 'Could not start Wi-Fi setup.') }
  }

  const handleCredentialRotation = async (event: FormEvent) => {
    event.preventDefault()
    try {
      await requestJson('/api/auth/password', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ newPassword, maintenanceApPassword: apPassword }) })
      setNewPassword(''); setApPassword(''); setConnectionMessage('Administrator and maintenance AP credentials updated.')
    } catch (error) { setConnectionMessage(error instanceof Error ? error.message : 'Credential update failed.') }
  }

  const handleLogout = async () => {
    try { await requestJson('/api/auth/logout', { method: 'POST' }) } catch { /* legacy bridge or expired session */ }
    setAuthenticated(false); setCsrfToken('')
  }

  const percentLabel = reading.waterPercent === null || reading.readingState !== 'ok' ? '—' : `${reading.waterPercent.toFixed(1)}%`
  const fillPercent = reading.waterPercent === null || reading.readingState !== 'ok' ? 0 : reading.waterPercent
  const updatedLabel = reading.timestamp ? new Date(reading.timestamp).toLocaleString() : 'No reading yet'
  const endpoint = status.serialPort ?? status.ipAddress ?? 'No device endpoint'
  const measurement = config.measurement ?? defaultConfig.measurement!
  const power = config.power ?? defaultConfig.power!
  const networkConfig = config.network ?? defaultConfig.network!

  if (authenticated === null) return <main className="auth-shell"><section className="auth-card"><h1>Waterwatch</h1><p>Checking device security…</p></section></main>
  if (!authenticated) return <main className="auth-shell"><form className="auth-card" onSubmit={handleLogin}><p className="eyebrow">Protected device</p><h1>Sign in</h1><p>{authMessage}</p><label>Password<input type="password" autoComplete="current-password" value={loginPassword} onChange={(event) => setLoginPassword(event.target.value)} required /></label><button type="submit">Sign in</button><small>New devices use the deployment bootstrap credential once.</small></form></main>
  if (setupRequired) return <main className="auth-shell"><form className="auth-card" onSubmit={handleInitialCredentials}><p className="eyebrow">Commission device</p><h1>Replace bootstrap access</h1><p>Create two different credentials. The maintenance AP password is needed to join the sensor directly when the router is unavailable.</p><label>Administrator password<input type="password" minLength={10} value={newPassword} onChange={(event) => setNewPassword(event.target.value)} required /></label><label>Maintenance AP password<input type="password" minLength={8} value={apPassword} onChange={(event) => setApPassword(event.target.value)} required /></label><button type="submit">Save device credentials</button><small>{authMessage}</small></form></main>

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
          {config.schemaVersion && <button type="button" className="text-button" onClick={handleLogout}>Log out</button>}
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
            {config.schemaVersion && <>
              <div className="wide form-section"><strong>Measurement validation</strong><small>Full/empty calibration is more accurate than container depth. Capture points only at known stable levels.</small></div>
              <label>Calibration mode<select value={measurement.calibrationMode} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, calibrationMode: event.target.value as 'container_depth' | 'full_empty' } }))}><option value="container_depth">Container depth</option><option value="full_empty">Full / empty points</option></select></label>
              <label>Valid sensor range <span>cm min</span><input type="number" min="2" max="449" step="0.1" value={measurement.minimumValidDistanceCm} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, minimumValidDistanceCm: Number(event.target.value) } }))} /></label>
              <div className="wide calibration-row"><button type="button" className="secondary" onClick={() => captureCalibration('full')}>Capture full ({measurement.fullDistanceCm?.toFixed(1) ?? '—'} cm)</button><button type="button" className="secondary" onClick={() => captureCalibration('empty')}>Capture empty ({measurement.emptyDistanceCm?.toFixed(1) ?? '—'} cm)</button></div>
              <label>Maximum valid distance <span>cm</span><input type="number" min="3" max="450" step="0.1" value={measurement.maximumValidDistanceCm} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, maximumValidDistanceCm: Number(event.target.value) } }))} /></label>
              <label>Median samples<select value={measurement.medianWindowSize} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, medianWindowSize: Number(event.target.value) } }))}>{[3,5,7,9,11,13,15].map((value) => <option key={value}>{value}</option>)}</select></label>
              <label>Maximum sudden change <span>cm</span><input type="number" min="0" step="0.1" value={measurement.maximumStepCm} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, maximumStepCm: Number(event.target.value) } }))} /></label>
              <label>Confirmation samples<input type="number" min="1" max="10" value={measurement.stepConfirmationSamples} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, stepConfirmationSamples: Number(event.target.value) } }))} /></label>
              <label>Failures before fault<input type="number" min="1" max="20" value={measurement.invalidSamplesBeforeFault} onChange={(event) => setConfig((current) => ({ ...current, measurement: { ...measurement, invalidSamplesBeforeFault: Number(event.target.value) } }))} /></label>
              <div className="wide form-section"><strong>Optional power controls</strong><small>Disabled by default. Scheduled sleep makes the dashboard unreachable between wake windows.</small></div>
              <label className="check-label"><input type="checkbox" checked={power.powerSavingEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, powerSavingEnabled: event.target.checked } }))} /> Enable power saving</label>
              <label>Sample interval <span>seconds</span><input type="number" min="0.5" max="3600" step="0.5" value={power.sampleIntervalSeconds} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, sampleIntervalSeconds: Number(event.target.value) } }))} /></label>
              <label>OLED timeout <span>seconds · 0 always</span><input type="number" min="0" max="86400" value={power.displayTimeoutSeconds} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, displayTimeoutSeconds: Number(event.target.value) } }))} /></label>
              <label className="check-label"><input type="checkbox" checked={power.scheduledSleepEnabled} disabled={!power.powerSavingEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, scheduledSleepEnabled: event.target.checked } }))} /> Scheduled deep sleep</label>
              <label>Awake window <span>seconds</span><input type="number" min="15" max="600" value={power.awakeWindowSeconds} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, awakeWindowSeconds: Number(event.target.value) } }))} /></label>
              <label className="check-label"><input type="checkbox" checked={power.batteryMonitoringEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, batteryMonitoringEnabled: event.target.checked } }))} /> Battery hardware installed</label>
              <label>Battery low threshold <span>volts</span><input type="number" min="0" step="0.05" value={power.batteryLowVoltage} disabled={!power.batteryMonitoringEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, batteryLowVoltage: Number(event.target.value) } }))} /></label>
              <label>Battery critical threshold <span>volts</span><input type="number" min="0" step="0.05" value={power.batteryCriticalVoltage} disabled={!power.batteryMonitoringEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, batteryCriticalVoltage: Number(event.target.value) } }))} /></label>
              <label>Battery calibration multiplier<input type="number" min="0.1" step="0.01" value={power.batteryCalibrationMultiplier} disabled={!power.batteryMonitoringEnabled} onChange={(event) => setConfig((current) => ({ ...current, power: { ...power, batteryCalibrationMultiplier: Number(event.target.value) } }))} /></label>
              <div className="wide form-section"><strong>Maintenance access</strong><small>The protected access point makes this dashboard available at 192.168.4.1 without a router.</small></div>
              <label className="check-label"><input type="checkbox" checked={networkConfig.maintenanceApEnabled} onChange={(event) => setConfig((current) => ({ ...current, network: { ...networkConfig, maintenanceApEnabled: event.target.checked } }))} /> Enable maintenance AP</label>
              <label>AP start delay <span>seconds</span><input type="number" min="0" max="600" value={networkConfig.maintenanceApDelaySeconds} onChange={(event) => setConfig((current) => ({ ...current, network: { ...networkConfig, maintenanceApDelaySeconds: Number(event.target.value) } }))} /></label>
              <label>AP idle timeout <span>seconds · 0 always</span><input type="number" min="0" max="86400" value={networkConfig.maintenanceApIdleTimeoutSeconds} onChange={(event) => setConfig((current) => ({ ...current, network: { ...networkConfig, maintenanceApIdleTimeoutSeconds: Number(event.target.value) } }))} /></label>
            </>}
            <button className="wide" type="submit" disabled={isSaving}>{isSaving ? 'Saving…' : 'Save configuration'}</button>
          </form>
        </article>

        <article className="panel connection-panel">
          <div className="panel-header"><div><p className="eyebrow">Connectivity</p><h2>Device connection</h2></div><span className={`status-badge ${status.status}`}>{status.status}</span></div>
          <div className="connection-summary"><span>Mode<strong>{status.mode.toUpperCase()}</strong></span><span>Firmware<strong>{status.firmwareVersion}</strong></span><span>Payload<strong>{reading.source.toUpperCase()}</strong></span></div>
          {status.mode === 'wifi' && network && <div className="wifi-summary"><span>Network<strong>{network.ssid || 'Not configured'}</strong></span><span>Signal<strong>{network.connected ? `${network.signalDbm} dBm` : 'Offline'}</strong></span><button type="button" className="secondary" onClick={handleWifiReset}>Change Wi-Fi network</button></div>}
          {status.mode === 'wifi' && config.schemaVersion && <form className="network-form" onSubmit={handleNetworkSetup}><strong>Connect or change local Wi-Fi</strong><label>Network name<input value={wifiSsid} maxLength={32} onChange={(event) => setWifiSsid(event.target.value)} required /></label><label>Wi-Fi password<input type="password" value={wifiPassword} maxLength={64} onChange={(event) => setWifiPassword(event.target.value)} /></label><label>Sensor hostname<input value={wifiHostname} pattern="[a-z0-9-]+" maxLength={32} onChange={(event) => setWifiHostname(event.target.value.toLowerCase())} required /></label><button type="submit" className="secondary">Connect Wi-Fi</button></form>}
          {status.mode === 'usb' && <><label>Serial port<select value={selectedPort} onChange={(event) => setSelectedPort(event.target.value)}><option value="">Select a port</option>{ports.map((port) => <option key={port.path} value={port.path}>{port.friendlyName}</option>)}</select></label>
          <div className="button-row"><button type="button" onClick={handleConnect}>Connect USB</button><button type="button" className="secondary" onClick={handleDisconnect}>Disconnect</button></div></>}
          <form className="api-form" onSubmit={(event) => { event.preventDefault(); setApiBase(draftApiBase.replace(/\/$/, '')) }}><label>Device API URL<input type="url" value={draftApiBase} onChange={(event) => setDraftApiBase(event.target.value)} /></label><button type="submit" className="secondary">Apply</button></form>
          <p className="connection-message">{connectionMessage}</p>
          {config.schemaVersion && <><button type="button" className="secondary" onClick={loadDiagnostics}>Load diagnostics</button>{diagnostics && <pre className="diagnostics">{JSON.stringify(diagnostics, null, 2)}</pre>}<form className="network-form" onSubmit={handleCredentialRotation}><strong>Rotate device credentials</strong><label>New administrator password<input type="password" minLength={10} value={newPassword} onChange={(event) => setNewPassword(event.target.value)} required /></label><label>New maintenance AP password<input type="password" minLength={8} value={apPassword} onChange={(event) => setApPassword(event.target.value)} required /></label><button type="submit" className="secondary">Update credentials</button></form></>}
        </article>
      </section>

      <section className="panel history-panel">
        <div className="panel-header"><div><p className="eyebrow">Recent activity</p><h2>Latest readings</h2></div><p>Session history · newest first</p></div>
        {history.length === 0 ? <div className="empty-state">Readings will appear here when the sensor begins reporting.</div> : <div className="history-list">{history.map((item) => <div key={item.sampleSequence ?? item.timestamp}><span className={`reading-dot ${item.readingState}`} /><time>{item.timestamp ? new Date(item.timestamp).toLocaleTimeString() : `#${item.sampleSequence ?? '—'}`}</time><strong>{item.waterPercent === null || item.readingState !== 'ok' ? 'No Reading' : `${item.waterPercent.toFixed(1)}%`}</strong><span>{item.distanceCm === null ? '—' : `${item.distanceCm.toFixed(1)} cm distance`}</span><small>{item.readingState.replaceAll('_', ' ')}</small></div>)}</div>}
      </section>
    </main>
  )
}

export default App
