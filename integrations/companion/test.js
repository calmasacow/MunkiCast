// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { LibertasInstance } from './main.js'
test('Companion actions, feedbacks and presets route native identities correctly', async () => {
  assert.equal(LibertasInstance.prototype.getConfigFields.call({}).find(field => field.id === 'password').type, 'secret-text')
  const calls = []
  const mock = {
    scenes: [{ sceneUuid: 'scene-1', sceneName: 'Camera' }], inputs: [{ inputUuid: 'input-1', inputName: 'Mic' }],
    state: { streaming: true, mainDeck: { program: 'scene-1', studio: true } },
    call: async (...args) => calls.push(args), refresh: async () => {},
    parseVariablesInString: async v => v, log: (level, msg) => { throw new Error(`${level}: ${msg}`) },
    setActionDefinitions: v => { mock.actions = v }, setFeedbackDefinitions: v => { mock.feedbacks = v }, setPresetDefinitions: v => { mock.presets = v },
  }
  LibertasInstance.prototype.defineControls.call(mock, ['GetSceneList'])
  await mock.actions.program.callback({ options: { scene: 'scene-1' } })
  assert.deepEqual(calls.pop(), ['SetCurrentProgramScene', { sceneUuid: 'scene-1' }])
  await mock.actions.replay.callback({ options: { operation: 'Save' } })
  assert.deepEqual(calls.pop(), ['SaveReplayBuffer'])
  await mock.actions.request.callback({ options: { request: 'GetSceneList', data: '{}' } })
  assert.deepEqual(calls.pop(), ['GetSceneList', {}])
  assert(mock.feedbacks.stream.callback())
  assert(mock.feedbacks.scene.callback({ options: { scene: 'scene-1', bus: 'program' } }))
  assert(mock.presets['scene_scene-1'])
  assert(mock.feedbacks.studio.callback())
  mock.state.mainDeck.studio = false
  assert.equal(mock.feedbacks.studio.callback(), false)
  assert.deepEqual(mock.presets.studio.steps[0].down, [{ actionId: 'studio', options: { enabled: 'toggle' } }])
  assert.equal(mock.presets.studio.feedbacks[0].feedbackId, 'studio')
  for (const key of ['stream', 'record']) {
    assert.equal(mock.presets[`${key}_timecode`].style.text, `${key.toUpperCase()}\n$(this:${key}_timecode)`)
    assert.deepEqual(mock.presets[`${key}_timecode`].steps, mock.presets[key].steps)
    assert.deepEqual(mock.presets[`${key}_timecode`].feedbacks, mock.presets[key].feedbacks)
    assert.equal(mock.presets[key].style.text, key.toUpperCase())
  }
  for (const enabled of [true, false]) {
    mock.call = async (...args) => {
      calls.push(args)
      if (args[0] === 'GetStudioModeEnabled') return { studioModeEnabled: enabled }
    }
    await mock.actions.studio.callback({ options: { enabled: 'toggle' } })
    assert.deepEqual(calls.splice(0), [
      ['GetStudioModeEnabled'], ['SetStudioModeEnabled', { studioModeEnabled: !enabled }],
    ])
  }
  for (const enabled of ['true', 'false']) {
    await mock.actions.studio.callback({ options: { enabled } })
    assert.deepEqual(calls.splice(0), [['SetStudioModeEnabled', { studioModeEnabled: enabled === 'true' }]])
  }
})

test('Output timecode variables follow server status, preserve hours and reset when stopped', async () => {
  const statuses = {
    LibertasGetState: { streaming: true, recording: true }, GetSceneList: {},
    GetStreamStatus: { outputActive: true, outputTimecode: '12:34:56.789' },
    GetRecordStatus: { outputActive: true, outputTimecode: '00:01:02.345' },
  }
  const mock = {
    socket: { call: async request => statuses[request] }, generation: 1,
    setVariableValues: values => { mock.variables = values }, checkFeedbacks: () => {},
    log: (_level, message) => { throw new Error(message) },
  }
  await LibertasInstance.prototype.refresh.call(mock)
  assert.equal(mock.variables.stream_timecode, '12:34:56')
  assert.equal(mock.variables.record_timecode, '00:01:02')
  statuses.GetStreamStatus.outputActive = statuses.GetRecordStatus.outputActive = false
  await LibertasInstance.prototype.refresh.call(mock)
  assert.equal(mock.variables.stream_timecode, '00:00:00')
  assert.equal(mock.variables.record_timecode, '00:00:00')
})
