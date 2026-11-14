// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Run by munkicast-ui --check --check-mcp against an isolated project and real UI.
import assert from 'node:assert/strict'
import { mkdir, writeFile } from 'node:fs/promises'
import { join } from 'node:path'
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js'
import { StdioClientTransport } from '@modelcontextprotocol/sdk/client/stdio.js'
const url = new URL(process.env.MUNKICAST_MCP_URL)
const token = process.env.MUNKICAST_MCP_TOKEN
const headers = { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' }
const request = { jsonrpc: '2.0', id: 1, method: 'ping' }
assert.equal((await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(request) })).status, 401)
assert.equal((await fetch(url, { method: 'POST', headers: { ...headers, Origin: 'https://untrusted.example' }, body: JSON.stringify(request) })).status, 403)
assert.equal((await fetch(url, { method: 'GET', headers })).status, 405)
assert.equal((await fetch(url, { method: 'POST', headers, body: '{broken' })).status, 400)
const client = new Client({ name: 'munkicast-native-check', version: '1.0.0' })
await client.connect(new StreamableHTTPClientTransport(url, { requestInit: { headers: { Authorization: `Bearer ${token}` } } }))
try {
  const { tools } = await client.listTools()
  const names = tools.map(t => t.name)
  assert(names.length > 200, `Expected app-wide controls, got ${names.length}`)
  assert.equal(new Set(names).size, names.length)
  for (const name of ['document_addScene', 'document_addNewSourceToScene', 'document_setNodePosition',
    'document_applySourceSettings', 'document_addFilter', 'document_setEncodeBitrate',
    'workspace_addWorkspace', 'preferences_setCachingEnabled', 'shortcuts_setTrigger', 'cache_build',
    'preview_startRecording', 'ui_tree', 'ui_pointer', 'ui_key', 'ui_text', 'ui_screenshot']) assert(names.includes(name), name)
  assert(!names.includes('SetCurrentProgramScene'))
  const raw = (name, args = {}) => client.callTool({ name, arguments: args })
  const call = async (name, args = {}) => {
    const response = await raw(name, args)
    assert(!response.isError, `${name}: ${JSON.stringify(response)}`)
    return JSON.parse(response.content[0].text)
  }
  const native = async (name, args = {}) => (await call(name, args)).result
  const properties = (target = 'document') => call('app_properties', { target })
  const initial = await properties()
  const project = await call('app_project')
  assert(project.sceneDocuments.length)
  assert((await call('app_command_templates')).templates.length > 40)
  assert(initial.sourceTypes.some(t => t.id === 'source.color' || t.type === 'source.color'))
  const schema = await native('document_sourceSettingsSchema', { type: 'source.color' })
  assert(Array.isArray(schema) && schema.length)
  await native('document_addScene', { name: 'Native MCP scene' })
  const scene = (await properties()).selectedScene
  assert.notEqual(scene, initial.selectedScene)
  await native('document_addNewSourceToScene', { name: 'Native MCP colour', type: 'source.color', visible: true })
  const created = await properties()
  const node = created.selectedNode, source = created.nodeProperties.sourceId
  assert(node && source)
  await native('document_applySourceSettings', { id: source, name: 'Native MCP colour', values: { color: '#123456' } })
  await native('document_setNodePosition', { x: 123, y: 234 })
  await native('document_setNodeScale', { x: 0.75, y: 0.5 })
  await native('document_setNodeRotation', { degrees: 17 })
  let p = (await properties()).nodeProperties
  // A position is stored as a fraction of the canvas and reported back in pixels, rounded to a
  // hundredth of one (ADR-0070). Exact equality is the point: 123/1920 is not representable in
  // binary, so without the rounding this reads 122.99999999999999.
  assert.equal(p.positionX, 123); assert.equal(p.positionY, 234)
  assert.equal(p.scaleX, 0.75); assert.equal(p.scaleY, 0.5); assert.equal(p.rotation, 17)
  await native('document_undo')
  assert.equal((await properties()).nodeProperties.rotation, 0)
  await native('document_redo')
  assert.equal((await properties()).nodeProperties.rotation, 17)
  await native('document_setNodeLocked', { id: node, locked: true })
  assert((await raw('document_rotateBy', { degrees: 1 })).isError)
  assert.equal((await properties()).nodeProperties.positionX, 123)
  await native('document_setNodeLocked', { id: node, locked: false })
  await assert.rejects(raw('document_setNodePosition', { x: 'invalid', y: 2 }))
  assert((await raw('app_command', { command: { cmd: 'invalid' } })).isError)
  const modelRef = (await properties()).scenes
  assert(modelRef?.objectId)
  const rows = await call('app_model', { objectId: modelRef.objectId, limit: 1 })
  assert.equal(rows.rows.length, 1); assert(rows.total > 1)
  const filters = await native('document_filterTypes', { owner: source })
  assert(Array.isArray(filters) && filters.length)
  const filterType = filters.find(t => t.id || t.type)
  assert(filterType)
  if (filterType) {
    await native('document_addFilter', { owner: source, type: filterType.id || filterType.type })
    assert((await native('document_filterDetails', { target: source })).effects.length)
  }
  await native('document_addBus', { name: 'Native MCP bus', kind: 'submix' })
  const busModel = (await properties()).buses.objectId
  let buses = (await call('app_model', { objectId: busModel })).rows
  const bus = buses.find(b => b.name === 'Native MCP bus')
  assert(bus)
  await native('document_setBusGain', { id: bus.busId, gainDb: -6, dragging: false })
  await native('document_setBusMute', { id: bus.busId, mute: true })
  buses = (await call('app_model', { objectId: busModel })).rows
  assert.equal(buses.find(b => b.busId === bus.busId).gainDb, -6)
  assert.equal(buses.find(b => b.busId === bus.busId).mute, true)
  await native('document_initializeEncodeGraph')
  const encode = (await call('app_model', { objectId: (await properties()).encodes.objectId })).rows[0]
  assert(encode)
  await native('document_setEncodeBitrate', { id: encode.encodeId, kbps: 4200 })
  assert.equal((await native('document_encodeSettings', { id: encode.encodeId })).bitrate, 4200)
  const pref = await properties('preferences')
  await native('preferences_setShowOutputMissedFrames', { on: !pref.showOutputMissedFrames })
  assert.equal((await properties('preferences')).showOutputMissedFrames, !pref.showOutputMissedFrames)
  await native('preferences_setShowOutputMissedFrames', { on: pref.showOutputMissedFrames })
  const workspaces = await properties('workspace')
  await native('workspace_addWorkspace', { name: 'Native MCP workspace' })
  assert.equal((await properties('workspace')).workspaceCount, workspaces.workspaceCount + 1)
  await native('workspace_removeWorkspace', { index: (await properties('workspace')).activeWorkspace })
  await native('workspace_selectWorkspace', { index: workspaces.activeWorkspace })
  const resource = await client.readResource({ uri: 'studio://application' })
  const app = JSON.parse(resource.contents[0].text)
  assert.equal(app.websocket.running, false)
  assert(app.document && app.workspace && app.preferences && app.preview)
  const coverage = JSON.parse((await client.readResource({ uri: 'studio://coverage' })).contents[0].text)
  assert(coverage.tools.length === tools.length)
  assert.deepEqual(coverage.nonJsonMethods, [])
  assert((await client.listPrompts()).prompts.some(p => p.name === 'studio-control'))
  const tree = await call('ui_tree', { search: 'menuMcpSettings' })
  const menu = tree.objects.find(o => o.objectName === 'menuMcpSettings')
  assert(menu)
  assert((await raw('ui_invoke', { objectId: menu.objectId, method: 'deleteLater()', arguments: [] })).isError)
  await call('ui_invoke', { objectId: menu.objectId, method: 'triggered()', arguments: [] })
  const dialog = (await call('ui_tree', { search: 'mcpSettingsDialog' })).objects.find(o => o.objectName === 'mcpSettingsDialog')
  assert(dialog?.visible)
  await native('preview_requestSize', { owner: dialog.objectId, edge: 256 })
  const inspect = await call('ui_inspect', { objectId: dialog.objectId })
  assert(inspect.methods.some(m => m.method === 'close()'))
  const screenshot = await raw('ui_screenshot', { objectId: dialog.objectId })
  assert(!screenshot.isError, JSON.stringify(screenshot))
  assert.equal(screenshot.content[0].mimeType, 'image/png')
  assert.equal(Buffer.from(screenshot.content[0].data, 'base64').subarray(1, 4).toString(), 'PNG')
  if (process.argv[2]) {
    await mkdir(process.argv[2], { recursive: true })
    await writeFile(join(process.argv[2], 'mcp-settings.png'), Buffer.from(screenshot.content[0].data, 'base64'))
    await writeFile(join(process.argv[2], 'native-mcp-inventory.json'), JSON.stringify(coverage, null, 2) + '\n')
  }
  await call('ui_key', { objectId: dialog.objectId, key: 'Escape' })
  // Test real input: show the settings, click the enable checkbox twice, keep running state untouched.
  await call('ui_invoke', { objectId: menu.objectId, method: 'triggered()', arguments: [] })
  const enabled = (await call('ui_tree', { search: 'mcpEnabled' })).objects.find(o => o.objectName === 'mcpEnabled')
  const checked = enabled.checked
  await call('ui_pointer', { objectId: enabled.objectId, action: 'click', x: 8, y: 10 })
  assert.equal((await call('ui_inspect', { objectId: enabled.objectId })).properties.checked, !checked)
  await call('ui_pointer', { objectId: enabled.objectId, action: 'click', x: 8, y: 10 })
  const input = (await call('ui_tree', { search: 'textLineInput' })).objects.find(o => o.windowId === dialog.objectId && o.visible)
  assert(input)
  await call('ui_pointer', { objectId: input.objectId, action: 'click', x: 8, y: 10 })
  await call('ui_key', { objectId: dialog.objectId, key: 'Ctrl+A' })
  await call('ui_text', { objectId: dialog.objectId, text: token })
  assert.equal((await call('ui_inspect', { objectId: input.objectId })).properties.length, token.length)
  await call('ui_key', { objectId: dialog.objectId, key: 'Tab' })
  await call('ui_invoke', { objectId: dialog.objectId, method: 'close()', arguments: [] })
  // Verify stdio compatibility forwards to the same native server, with no WebSocket credentials.
  const stdio = new Client({ name: 'munkicast-stdio-check', version: '1.0.0' })
  try {
    await stdio.connect(new StdioClientTransport({ command: process.execPath, args: [process.env.MUNKICAST_MCP_ENTRYPOINT || new URL('./main.js', import.meta.url).pathname], env: { ...process.env } }))
    assert((await stdio.listTools()).tools.some(t => t.name === 'document_addScene'))
    assert(!(await stdio.callTool({ name: 'app_project' })).isError)
  } finally { await stdio.close() }
  console.log(`Native MCP: ${tools.length} tools; scene/source creation, settings, transforms, undo/redo, validation, filters, audio buses, encode settings, models, preferences, workspaces, menus, pointer/key input, PNG capture, resources and stdio passed with WebSocket OFF.`)
} finally { await client.close() }
