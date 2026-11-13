'use strict'
// Signal K Autopilot-Provider (Autopilot API v2) für den ESP32-Autopiloten.
// Kommunikation: WebSocket (JSON, Winkel in GRAD auf der Leitung).
// Signal K Autopilot API: Winkel in RADIANT.
//
// Plugin -> ESP:  {"cmd":"engage"} {"cmd":"disengage"} {"cmd":"mode","value":"compass"}
//                 {"cmd":"target","value":<grad>} {"cmd":"adjust","value":<grad>}
// ESP -> Plugin:  {"state":"auto"|"standby","mode":"compass","target":<grad>,
//                  "heading":<grad>,"rudder":<grad>}   (bei Änderung und ca. 2 Hz)

const WebSocket = require('ws')

const DEG = Math.PI / 180
const DEVICE = 'esp-pilot'

module.exports = function (app) {
  const plugin = {
    id: 'esp-autopilot-provider',
    name: 'ESP Autopilot Provider'
  }

  let ws = null
  let retryTimer = null
  let stopped = true
  // Zielwert intern in Radiant (wie in Signal K)
  const st = { state: 'off-line', mode: 'compass', target: 0 }

  plugin.schema = {
    type: 'object',
    properties: {
      url: {
        type: 'string',
        title: 'ESP WebSocket URL',
        default: 'ws://esp-autopilot.local:81/'
      }
    }
  }

  function push() {
    try {
      app.autopilotUpdate(DEVICE, {
        state: st.state,
        mode: st.mode,
        target: st.target
      })
    } catch (e) {
      app.debug('autopilotUpdate: ' + e.message)
    }
  }

  function send(obj) {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
      throw new Error('ESP nicht verbunden')
    }
    ws.send(JSON.stringify(obj))
  }

  function connect(url) {
    if (stopped) return
    ws = new WebSocket(url)

    ws.on('open', () => {
      app.debug('ESP verbunden: ' + url)
      app.setPluginStatus('ESP verbunden')
    })

    ws.on('message', (data) => {
      let m
      try {
        m = JSON.parse(data.toString())
      } catch (e) {
        return
      }
      if (typeof m.state === 'string') st.state = m.state
      if (typeof m.mode === 'string') st.mode = m.mode
      if (typeof m.target === 'number') st.target = m.target * DEG
      push()
    })

    ws.on('error', (err) => app.debug('ESP-WebSocket: ' + err.message))

    ws.on('close', () => {
      st.state = 'off-line'
      push()
      app.setPluginStatus('ESP nicht erreichbar, neuer Versuch ...')
      if (!stopped) retryTimer = setTimeout(() => connect(url), 3000)
    })
  }

  const unsupported = async () => {
    throw new Error('Operation wird nicht unterstützt')
  }

  const provider = {
    getData: async () => ({
      options: {
        states: [
          { name: 'standby', engaged: false },
          { name: 'auto', engaged: true }
        ],
        modes: ['compass']
      },
      state: st.state,
      mode: st.mode,
      target: st.target,
      engaged: st.state === 'auto'
    }),
    getState: async () => st.state,
    setState: async (state) => {
      if (state === 'auto') send({ cmd: 'engage' })
      else if (state === 'standby') send({ cmd: 'disengage' })
      else throw new Error('Unbekannter Zustand: ' + state)
    },
    getMode: async () => st.mode,
    setMode: async (mode) => {
      if (mode !== 'compass') throw new Error('Nur Modus "compass" unterstützt')
      send({ cmd: 'mode', value: mode })
    },
    getTarget: async () => st.target,
    setTarget: async (value) => send({ cmd: 'target', value: value / DEG }),
    adjustTarget: async (value) => send({ cmd: 'adjust', value: value / DEG }),
    engage: async () => send({ cmd: 'engage' }),
    disengage: async () => send({ cmd: 'disengage' }),
    tack: unsupported,
    gybe: unsupported,
    dodge: unsupported
  }

  plugin.start = function (options) {
    stopped = false
    const url = (options && options.url) || 'ws://esp-autopilot.local:81/'
    try {
      app.registerAutopilotProvider(provider, [DEVICE])
    } catch (e) {
      app.setPluginError('Registrierung fehlgeschlagen: ' + e.message)
      return
    }
    connect(url)
  }

  plugin.stop = function () {
    stopped = true
    if (retryTimer) clearTimeout(retryTimer)
    if (ws) ws.close()
  }

  return plugin
}
