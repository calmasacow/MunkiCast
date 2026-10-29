// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import { InstanceBase, InstanceStatus, runEntrypoint, combineRgb } from '@companion-module/base'
import OBSWebSocket from 'obs-websocket-js/json'

export class LibertasInstance extends InstanceBase {
  async init(config, _isFirstInit, secrets = {}) {
    this.config = config
    this.secrets = secrets
    this.generation = 0
    this.state = {}
    this.scenes = []
    this.inputs = []
    this.setVariableDefinitions([
      { variableId: 'program', name: 'Program scene' },
      { variableId: 'preview', name: 'Preview scene' },
      { variableId: 'streaming', name: 'Streaming active' },
      { variableId: 'recording', name: 'Recording active' },
      { variableId: 'stream_timecode', name: 'Stream elapsed time (HH:MM:SS)' },
      { variableId: 'record_timecode', name: 'Record elapsed time (HH:MM:SS)' },
      { variableId: 'replay', name: 'Replay active' },
      { variableId: 'last_replay', name: 'Last replay file' },
      { variableId: 'revision', name: 'Document revision' },
    ])
    this.defineControls([])
    this.retry = setInterval(() => { if (!this.socket) void this.connectStudio() }, 5000)
    await this.connectStudio()
  }

  getConfigFields() {
    return [
      { type: 'textinput', id: 'host', label: 'Host', width: 8, default: '127.0.0.1' },
      { type: 'number', id: 'port', label: 'Port', width: 4, default: 4455, min: 1024, max: 65535 },
      { type: 'secret-text', id: 'password', label: 'WebSocket password', width: 12, default: '' },
      { type: 'static-text', id: 'help', label: 'Setup', width: 12, value: 'In Libertas: Tools > WebSocket Server Settings. Enable server, choose Libertas Studio mode, set password, Apply.' },
    ]
  }

  async configUpdated(config, secrets = {}) {
    await this.disconnectStudio()
    this.config = config
    this.secrets = secrets
    await this.connectStudio()
  }

  async disconnectStudio() {
    ++this.generation
    clearInterval(this.poll)
    this.poll = undefined
    const socket = this.socket
    this.socket = undefined
    if (socket) { socket.removeAllListeners(); await socket.disconnect().catch(() => {}) }
  }

  async destroy() { clearInterval(this.retry); await this.disconnectStudio() }

  async connectStudio() {
    if (this.socket) return
    const generation = ++this.generation
    const socket = new OBSWebSocket()
    this.socket = socket
    this.updateStatus(InstanceStatus.Connecting)
    socket.on('ConnectionError', () => {})
    socket.on('ConnectionClosed', () => {
      if (generation !== this.generation) return
      this.socket = undefined
      clearInterval(this.poll)
      this.state = {}
      this.checkFeedbacks()
      this.updateStatus(InstanceStatus.Disconnected)
    })
    socket.on('LibertasStateChanged', () => { void this.refresh() })
    socket.on('SceneListChanged', () => { void this.discover() })
    socket.on('InputCreated', () => { void this.discover() })
    socket.on('InputRemoved', () => { void this.discover() })
    socket.on('InputNameChanged', () => { void this.discover() })
    try {
      const host = String(this.config.host || '127.0.0.1')
      if (!/^[a-zA-Z0-9.:[\]-]+$/.test(host)) throw new Error('Invalid host')
      const authority = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host
      await socket.connect(`ws://${authority}:${this.config.port || 4455}`, this.secrets.password || '', { rpcVersion: 1, eventSubscriptions: 2047 })
      const capabilities = await socket.call('LibertasGetCapabilities')
      if (generation !== this.generation) return
      this.capabilities = capabilities
      await this.discover()
      await this.refresh()
      if (generation !== this.generation) return
      this.updateStatus(InstanceStatus.Ok)
      this.poll = setInterval(() => { void this.refresh() }, 1000)
    } catch (error) {
      if (generation !== this.generation) return
      await this.disconnectStudio()
      this.updateStatus(InstanceStatus.ConnectionFailure, error.message)
    }
  }

  async discover() {
    if (!this.socket || this.discovering) return
    this.discovering = true
    const socket = this.socket, generation = this.generation
    try {
      const [scenes, inputs] = await Promise.all([socket.call('GetSceneList'), socket.call('GetInputList')])
      if (generation !== this.generation) return
      this.scenes = scenes.scenes
      this.inputs = inputs.inputs
      this.defineControls(Object.entries(this.capabilities.requests).filter(([, r]) => r.supported).map(([name]) => name))
    } catch (error) { this.log('warn', `Discovery: ${error.message}`) }
    finally { this.discovering = false }
  }

  async refresh() {
    if (!this.socket || this.refreshing) return
    this.refreshing = true
    const socket = this.socket, generation = this.generation
    try {
      const [state, scenes, stream, record] = await Promise.all([
        socket.call('LibertasGetState'), socket.call('GetSceneList'),
        socket.call('GetStreamStatus'), socket.call('GetRecordStatus'),
      ])
      if (generation !== this.generation) return
      this.state = state
      this.setVariableValues({
        program: scenes.currentProgramSceneName || '', preview: scenes.currentPreviewSceneName || '',
        streaming: String(state.streaming), recording: String(state.recording), replay: String(state.replayActive),
        stream_timecode: stream.outputActive ? stream.outputTimecode.split('.')[0] : '00:00:00',
        record_timecode: record.outputActive ? record.outputTimecode.split('.')[0] : '00:00:00',
        last_replay: state.lastReplayPath || '', revision: state.revision,
      })
      this.checkFeedbacks()
    } catch (error) { this.log('warn', `State: ${error.message}`) }
    finally { this.refreshing = false }
  }

  async call(type, data = {}) {
    if (!this.socket) throw new Error('Libertas Studio is disconnected')
    return this.socket.call(type, data)
  }

  defineControls(requests) {
    const scenes = this.scenes.map(s => ({ id: s.sceneUuid, label: s.sceneName }))
    const inputs = this.inputs.map(s => ({ id: s.inputUuid, label: s.inputName }))
    const dropdown = (id, label, choices) => ({ type: 'dropdown', id, label, default: choices[0]?.id || '', choices })
    const jsonField = (id, label, value) => ({ type: 'textinput', id, label, default: value, useVariables: true })
    const safe = fn => async event => { try { await fn(event.options); await this.refresh() } catch (error) { this.log('error', error.message) } }
    const actions = {
      program: { name: 'Scene: Take to program', options: [dropdown('scene', 'Scene', scenes)], callback: safe(o => this.call('SetCurrentProgramScene', { sceneUuid: o.scene })) },
      preview: { name: 'Scene: Set preview', options: [dropdown('scene', 'Scene', scenes)], callback: safe(o => this.call('SetCurrentPreviewScene', { sceneUuid: o.scene })) },
      take: { name: 'Transition: Take preview', options: [], callback: safe(() => this.call('TriggerStudioModeTransition')) },
      studio: { name: 'Studio mode', options: [dropdown('enabled', 'Enabled', [{ id: 'true', label: 'On' }, { id: 'false', label: 'Off' }, { id: 'toggle', label: 'Toggle' }])], callback: safe(async o => {
        const enabled = o.enabled === 'toggle' ? !(await this.call('GetStudioModeEnabled')).studioModeEnabled : o.enabled === 'true'
        await this.call('SetStudioModeEnabled', { studioModeEnabled: enabled })
      }) },
      mute: { name: 'Input: Mute', options: [dropdown('input', 'Input', inputs), dropdown('mute', 'Mute', [{ id: 'toggle', label: 'Toggle' }, { id: 'true', label: 'On' }, { id: 'false', label: 'Off' }])], callback: safe(o => this.call(o.mute === 'toggle' ? 'ToggleInputMute' : 'SetInputMute', { inputUuid: o.input, inputMuted: o.mute === 'true' })) },
      volume: { name: 'Input: Volume (dB)', options: [dropdown('input', 'Input', inputs), { type: 'number', id: 'db', label: 'dB', default: 0, min: -100, max: 26, step: 0.1 }], callback: safe(o => this.call('SetInputVolume', { inputUuid: o.input, inputVolumeDb: Number(o.db) })) },
      request: { name: 'API: Any supported request', options: [dropdown('request', 'Request', requests.map(r => ({ id: r, label: r }))), jsonField('data', 'Request data (JSON object)', '{}')], callback: safe(async o => {
        const raw = await this.parseVariablesInString(o.data)
        const data = JSON.parse(raw)
        if (!data || Array.isArray(data) || typeof data !== 'object') throw new Error('Request data must be a JSON object')
        await this.call(o.request, data)
      }) },
      command: { name: 'Native: Document command (undoable)', options: [jsonField('command', 'Serialized document command', '{"cmd":"project.set-name","name":"My show"}')], callback: safe(async o => this.call('LibertasExecuteCommand', { command: JSON.parse(await this.parseVariablesInString(o.command)) })) },
      invoke: { name: 'Native: Invoke document method', options: [jsonField('method', 'Exact signature (LibertasGetMethods)', 'undo()'), jsonField('args', 'Arguments (JSON array)', '[]')], callback: safe(async o => this.call('LibertasInvoke', { method: await this.parseVariablesInString(o.method), arguments: JSON.parse(await this.parseVariablesInString(o.args)) })) },
    }
    for (const [key, family] of [['stream', 'Stream'], ['record', 'Record'], ['replay', 'ReplayBuffer']]) {
      actions[key] = { name: `${key}: Control`, options: [dropdown('operation', 'Operation', ['Toggle', 'Start', 'Stop', ...(key === 'replay' ? ['Save'] : [])].map(r => ({ id: r, label: r })))], callback: safe(o => this.call(o.operation + family)) }
    }
    this.setActionDefinitions(actions)
    const style = { color: combineRgb(255, 255, 255), bgcolor: combineRgb(180, 30, 30) }
    const feedbacks = {}
    for (const [key, field] of [['stream', 'streaming'], ['record', 'recording'], ['replay', 'replayActive']]) {
      feedbacks[key] = { type: 'boolean', name: `${key}: Active`, defaultStyle: style, options: [], callback: () => Boolean(this.state[field]) }
    }
    feedbacks.studio = { type: 'boolean', name: 'Studio mode: Enabled', defaultStyle: style, options: [], callback: () => Boolean(this.state.mainDeck?.studio) }
    feedbacks.scene = { type: 'boolean', name: 'Scene on program / preview', defaultStyle: style, options: [dropdown('scene', 'Scene', scenes), dropdown('bus', 'Bus', [{ id: 'program', label: 'Program' }, { id: 'preview', label: 'Preview' }])], callback: event => this.state.mainDeck?.[event.options.bus] === event.options.scene }
    this.setFeedbackDefinitions(feedbacks)
    const presets = {}
    for (const scene of scenes) presets[`scene_${scene.id}`] = { type: 'button', category: 'Scenes', name: scene.label,
      style: { text: scene.label, size: 'auto', color: combineRgb(255, 255, 255), bgcolor: combineRgb(20, 20, 20) },
      steps: [{ down: [{ actionId: 'program', options: { scene: scene.id } }], up: [] }],
      feedbacks: [{ feedbackId: 'scene', options: { scene: scene.id, bus: 'program' }, style }] }
    for (const key of ['stream', 'record', 'replay']) presets[key] = { type: 'button', category: 'Outputs', name: key,
      style: { text: key.toUpperCase(), size: 'auto', color: combineRgb(255, 255, 255), bgcolor: combineRgb(20, 20, 20) },
      steps: [{ down: [{ actionId: key, options: { operation: 'Toggle' } }], up: [] }], feedbacks: [{ feedbackId: key, options: {}, style }] }
    presets.studio = { type: 'button', category: 'Studio', name: 'Studio mode toggle',
      style: { text: 'STUDIO\nMODE', size: 'auto', color: combineRgb(255, 255, 255), bgcolor: combineRgb(20, 20, 20) },
      steps: [{ down: [{ actionId: 'studio', options: { enabled: 'toggle' } }], up: [] }],
      feedbacks: [{ feedbackId: 'studio', options: {}, style }] }
    for (const key of ['stream', 'record']) presets[`${key}_timecode`] = {
      ...presets[key], name: `${key} with timecode`,
      style: { ...presets[key].style, text: `${key.toUpperCase()}\n$(this:${key}_timecode)` },
    }
    this.setPresetDefinitions(presets)
  }
}
if (process.env.MODULE_MANIFEST) runEntrypoint(LibertasInstance, [])
