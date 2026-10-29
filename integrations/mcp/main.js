#!/usr/bin/env node
// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Optional stdio transport for clients that cannot connect directly over Streamable HTTP.
import { Client } from '@modelcontextprotocol/sdk/client/index.js'
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js'
import { Server } from '@modelcontextprotocol/sdk/server/index.js'
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js'
import {
  CallToolRequestSchema, ListToolsRequestSchema, ListResourcesRequestSchema,
  ReadResourceRequestSchema, ListResourceTemplatesRequestSchema,
  ListPromptsRequestSchema, GetPromptRequestSchema,
} from '@modelcontextprotocol/sdk/types.js'
import { readFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import { resolve } from 'node:path'

export async function configuration(environment = process.env) {
  const url = new URL(environment.LIBERTAS_MCP_URL || 'http://127.0.0.1:4466/mcp')
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash)
    throw new Error('LIBERTAS_MCP_URL must be an http(s) endpoint without credentials, query or fragment')
  if (url.protocol === 'http:' && !['127.0.0.1', 'localhost', '[::1]'].includes(url.hostname))
    throw new Error('Plain HTTP MCP connections must use loopback')
  const token = environment.LIBERTAS_MCP_TOKEN || (environment.LIBERTAS_MCP_TOKEN_FILE
    ? (await readFile(environment.LIBERTAS_MCP_TOKEN_FILE, 'utf8')).replace(/\r?\n$/, '') : '')
  if (!/^[A-Za-z0-9]{32,256}$/.test(token)) throw new Error('Set LIBERTAS_MCP_TOKEN or LIBERTAS_MCP_TOKEN_FILE to the token from MCP Server Settings')
  return { url, token }
}
export async function start() {
  const { url, token } = await configuration()
  const client = new Client({ name: 'libertas-stdio-transport', version: '2.0.0' })
  const transport = new StreamableHTTPClientTransport(url, { requestInit: { headers: { Authorization: `Bearer ${token}` } } })
  await client.connect(transport)
  const server = new Server({ name: 'libertas-studio-native-stdio', version: '2.0.0' }, {
    capabilities: { tools: {}, resources: {}, prompts: {} }, instructions: client.getInstructions(),
  })
  server.setRequestHandler(ListToolsRequestSchema, request => client.listTools(request.params))
  server.setRequestHandler(CallToolRequestSchema, request => client.callTool(request.params))
  server.setRequestHandler(ListResourcesRequestSchema, request => client.listResources(request.params))
  server.setRequestHandler(ReadResourceRequestSchema, request => client.readResource(request.params))
  server.setRequestHandler(ListResourceTemplatesRequestSchema, request => client.listResourceTemplates(request.params))
  server.setRequestHandler(ListPromptsRequestSchema, request => client.listPrompts(request.params))
  server.setRequestHandler(GetPromptRequestSchema, request => client.getPrompt(request.params))
  const close = async () => { await client.close(); await server.close() }
  process.once('SIGINT', () => { void close() })
  process.once('SIGTERM', () => { void close() })
  await server.connect(new StdioServerTransport())
  process.stdin.once('end', () => { void close() })
}
if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  start().catch(error => { console.error(error.message); process.exitCode = 1 })
