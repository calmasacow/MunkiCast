// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import assert from 'node:assert/strict'
import OBSWebSocket from 'obs-websocket-js'
const port = Number(process.argv[2])
const obs = new OBSWebSocket()
obs.on('ConnectionError', () => {})
const address = `ws://127.0.0.1:${port}`
await obs.connect(address, process.env.LIBERTAS_WEBSOCKET_PASSWORD, { rpcVersion: 1 })
assert.equal((await obs.call('GetVersion')).rpcVersion, 1)
const batch = await obs.callBatch([{ requestType: 'GetSceneList' }, { requestType: 'GetInputList' }, { requestType: 'GetRecordStatus' }])
assert(batch.every(r => r.requestStatus.result))
await obs.disconnect()
// Drive the native Companion module against the real listener without a Companion UI.
const { LibertasInstance } = await import('../companion/main.js')
const companion = Object.assign(Object.create(LibertasInstance.prototype), {
  config: { host: '127.0.0.1', port }, secrets: { password: process.env.LIBERTAS_WEBSOCKET_PASSWORD },
  generation: 0, scenes: [], inputs: [], state: {},
  updateStatus: (_status, message) => { if (message) throw new Error(message) },
  setActionDefinitions: value => { companion.actions = value },
  setFeedbackDefinitions: value => { companion.feedbacks = value },
  setPresetDefinitions: value => { companion.presets = value },
  setVariableValues: value => { companion.variables = value },
  parseVariablesInString: async value => value, checkFeedbacks: () => {},
  log: (_level, message) => { throw new Error(message) },
})
try {
  await companion.connectStudio()
  assert(companion.socket && companion.capabilities)
  assert(companion.variables.program)
  assert(Object.keys(companion.presets).length > 0)
  await companion.actions.request.callback({ options: { request: 'GetSceneList', data: '{}' } })
  assert.equal(typeof companion.feedbacks.record.callback(), 'boolean')
  for (const key of ['stream', 'record']) {
    assert.match(companion.variables[`${key}_timecode`], /^\d{2,}:\d{2}:\d{2}$/)
    assert(companion.presets[`${key}_timecode`])
  }
  const originalStudio = (await companion.call('GetStudioModeEnabled')).studioModeEnabled
  try {
    await companion.actions.studio.callback({ options: { enabled: 'toggle' } })
    assert.equal((await companion.call('GetStudioModeEnabled')).studioModeEnabled, !originalStudio)
    await companion.refresh()
    assert.equal(companion.feedbacks.studio.callback(), !originalStudio)
  } finally {
    await companion.actions.studio.callback({ options: { enabled: String(originalStudio) } })
  }
  assert.equal((await companion.call('GetStudioModeEnabled')).studioModeEnabled, originalStudio)
} finally { await companion.disconnectStudio() }
console.log('Official OBS JS transport, native Companion connection/actions/feedback/variables and batches passed.')
