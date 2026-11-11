// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { configuration } from './main.js'

test('stdio transport uses separate MCP configuration and validates credentials and transport', async () => {
  const token = 'a'.repeat(64)
  const config = await configuration({ MUNKICAST_MCP_TOKEN: token })
  assert.equal(config.url.href, 'http://127.0.0.1:4466/mcp')
  assert.equal(config.token, token)
  for (const url of ['ws://127.0.0.1:4455', 'http://remote.example/mcp', 'http://name:password@localhost/mcp', 'http://localhost/mcp?secret=abc'])
    await assert.rejects(configuration({ MUNKICAST_MCP_URL: url, MUNKICAST_MCP_TOKEN: token }))
  await assert.rejects(configuration({ MUNKICAST_WEBSOCKET_PASSWORD: token }))
  await assert.rejects(configuration({ MUNKICAST_MCP_TOKEN: 'short' }))
})
