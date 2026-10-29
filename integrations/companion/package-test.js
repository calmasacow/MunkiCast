// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import { test } from 'node:test'
import assert from 'node:assert/strict'
import { fork } from 'node:child_process'
import { mkdtemp, cp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
test('relocated Companion package registers through the real module IPC entrypoint', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'libertas-companion-'))
  let child
  try {
    await cp(new URL('./pkg', import.meta.url), directory, { recursive: true })
    child = fork(join(directory, 'main.js'), [], { silent: true, env: { ...process.env,
      MODULE_MANIFEST: join(directory, 'companion/manifest.json'), CONNECTION_ID: 'package-check', VERIFICATION_TOKEN: 'test-token' } })
    let output = ''
    child.stderr.on('data', data => { output += data })
    const message = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`Package did not register: ${output}`)), 4000)
      child.once('error', error => { clearTimeout(timer); reject(error) })
      child.once('exit', code => { clearTimeout(timer); reject(new Error(`Package exited ${code}: ${output}`)) })
      child.on('message', value => { if (value.name === 'register') { clearTimeout(timer); resolve(value) } })
    })
    assert.equal(message.direction, 'call')
    assert.equal(JSON.parse(message.payload).connectionId, 'package-check')
  } finally {
    if (child && child.exitCode === null && child.signalCode === null) { const exited = new Promise(resolve => child.once('exit', resolve)); child.kill(); await exited }
    await rm(directory, { recursive: true, force: true })
  }
})
