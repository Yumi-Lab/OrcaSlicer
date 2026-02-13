#!/usr/bin/env node
/**
 * OrcaSlicer MCP Bridge
 *
 * Translates MCP stdio transport (used by Claude Code) into
 * HTTP/SSE transport (used by OrcaSlicer's built-in MCP server).
 *
 * Architecture:
 *   Claude Code  <--stdio-->  Bridge  <--HTTP/SSE-->  OrcaSlicer
 *
 * The bridge:
 *   1. Connects to OrcaSlicer's SSE endpoint to get a session_id
 *   2. Initializes the MCP session via HTTP POST
 *   3. Discovers available tools from OrcaSlicer
 *   4. Forwards tool calls from Claude to OrcaSlicer via HTTP POST
 *   5. Reads responses from the SSE stream and returns them to Claude
 */

import { Server } from '@modelcontextprotocol/sdk/server/index.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js';
import http from 'node:http';

// Configuration (overridable via environment variables)
const ORCA_PORT = parseInt(process.env.ORCA_MCP_PORT || '13619');
const ORCA_HOST = process.env.ORCA_MCP_HOST || '127.0.0.1';
const CONNECT_TIMEOUT = parseInt(process.env.ORCA_CONNECT_TIMEOUT || '10000');

const log = (msg) => process.stderr.write(`[orca-bridge] ${msg}\n`);

// ============================================================
// OrcaSlicer SSE Client — handles HTTP/SSE communication
// ============================================================

class OrcaSSEClient {
  constructor(host, port) {
    this.host = host;
    this.port = port;
    this.sessionId = null;
    this.pending = new Map();   // id -> { resolve, reject, timer }
    this.nextId = 1;
    this.sseReq = null;
    this.connected = false;
  }

  // Connect to the SSE endpoint and extract the session_id
  connectSSE() {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        if (this.sseReq) this.sseReq.destroy();
        reject(new Error(
          `Timeout (${CONNECT_TIMEOUT}ms) connecting to OrcaSlicer at ${this.host}:${this.port}. ` +
          'Is OrcaSlicer running with --mcp-server?'
        ));
      }, CONNECT_TIMEOUT);

      this.sseReq = http.get({
        hostname: this.host,
        port: this.port,
        path: '/sse',
        headers: { 'Accept': 'text/event-stream', 'Cache-Control': 'no-cache' }
      }, (res) => {
        if (res.statusCode !== 200) {
          clearTimeout(timeout);
          reject(new Error(`SSE connection failed: HTTP ${res.statusCode}`));
          return;
        }

        let buffer = '';
        let currentEvent = '';
        let dataLines = [];

        res.on('data', (chunk) => {
          buffer += chunk.toString();
          const lines = buffer.split('\n');
          buffer = lines.pop(); // keep incomplete line in buffer

          for (const line of lines) {
            if (line.startsWith('event: ')) {
              currentEvent = line.slice(7).trim();
              dataLines = [];
            } else if (line.startsWith('data: ')) {
              dataLines.push(line.slice(6));
            } else if (line.trim() === '' && currentEvent) {
              // Empty line = end of SSE event
              const data = dataLines.join('\n').trim();
              this._handleSSEEvent(currentEvent, data, resolve, timeout);
              currentEvent = '';
              dataLines = [];
            }
          }
        });

        res.on('error', (err) => {
          clearTimeout(timeout);
          this.connected = false;
          log(`SSE stream error: ${err.message}`);
        });

        res.on('close', () => {
          this.connected = false;
          log('SSE connection closed by OrcaSlicer');
          // Reject all pending requests
          for (const [id, { reject: rej, timer }] of this.pending) {
            clearTimeout(timer);
            rej(new Error('SSE connection closed'));
          }
          this.pending.clear();
        });
      });

      this.sseReq.on('error', (err) => {
        clearTimeout(timeout);
        reject(new Error(
          `Cannot connect to OrcaSlicer at ${this.host}:${this.port}. ` +
          `Is it running with --mcp-server? (${err.message})`
        ));
      });
    });
  }

  _handleSSEEvent(event, data, resolveConnect, connectTimeout) {
    if (event === 'endpoint') {
      const match = data.match(/session_id=([^&\s]+)/);
      if (match) {
        this.sessionId = match[1];
        clearTimeout(connectTimeout);
        resolveConnect();
      }
    } else if (event === 'message') {
      try {
        const msg = JSON.parse(data);
        if (msg.id !== undefined && this.pending.has(msg.id)) {
          const { resolve, timer } = this.pending.get(msg.id);
          clearTimeout(timer);
          this.pending.delete(msg.id);
          resolve(msg);
        }
      } catch (e) {
        log(`Failed to parse SSE message: ${e.message}`);
      }
    }
    // Ignore heartbeat events
  }

  // Send a JSON-RPC request via HTTP POST and wait for SSE response
  request(method, params = {}, timeoutMs = 10000) {
    if (!this.connected) {
      return Promise.reject(new Error('Not connected to OrcaSlicer'));
    }

    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`Timeout (${timeoutMs}ms) waiting for response to '${method}'`));
      }, timeoutMs);

      this.pending.set(id, { resolve, reject, timer });

      this._post({ jsonrpc: '2.0', id, method, params }).catch((err) => {
        clearTimeout(timer);
        this.pending.delete(id);
        reject(err);
      });
    });
  }

  // Send a JSON-RPC notification (no response expected)
  notify(method, params = {}) {
    return this._post({ jsonrpc: '2.0', method, params });
  }

  _post(body) {
    return new Promise((resolve, reject) => {
      const data = JSON.stringify(body);
      const req = http.request({
        hostname: this.host,
        port: this.port,
        path: `/message?session_id=${this.sessionId}`,
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': Buffer.byteLength(data)
        }
      }, (res) => {
        // OrcaSlicer returns 200 or 202, actual response comes via SSE
        let responseBody = '';
        res.on('data', (chunk) => responseBody += chunk);
        res.on('end', () => {
          if (res.statusCode >= 400) {
            reject(new Error(`HTTP ${res.statusCode}: ${responseBody}`));
          } else {
            resolve();
          }
        });
      });
      req.on('error', reject);
      req.write(data);
      req.end();
    });
  }

  // Full initialization: SSE connect → MCP initialize → notifications/initialized
  async initialize() {
    await this.connectSSE();
    this.connected = true;
    log(`SSE connected, session: ${this.sessionId}`);

    const initResult = await this.request('initialize', {
      protocolVersion: '2024-11-05',
      capabilities: {},
      clientInfo: { name: 'claude-code-bridge', version: '1.0.0' }
    }, 10000);

    log(`MCP initialized: ${JSON.stringify(initResult?.result?.serverInfo || {})}`);

    await this.notify('notifications/initialized');

    return initResult;
  }

  destroy() {
    this.connected = false;
    if (this.sseReq) {
      this.sseReq.destroy();
      this.sseReq = null;
    }
    for (const [, { timer }] of this.pending) {
      clearTimeout(timer);
    }
    this.pending.clear();
  }
}

// ============================================================
// Tool timeouts — longer for expensive operations
// ============================================================

const TOOL_TIMEOUTS = {
  slice:              300000,  // 5 min
  load_model:          30000,
  open_project:        30000,
  arrange_objects:     20000,
  auto_orient:         15000,
  take_screenshot:     15000,
  set_filament_count:  10000,
  set_filament_color:  10000,
  select_preset:       10000,
  set_print_settings:  10000,
  create_preset:       10000,
  duplicate_preset:    10000,
  import_preset:       10000,
  export_preset:       10000,
  duplicate_object:    10000,
  set_camera_view:     10000,
};

function getToolTimeout(name) {
  return TOOL_TIMEOUTS[name] || 10000;
}

// ============================================================
// Main
// ============================================================

async function main() {
  const orca = new OrcaSSEClient(ORCA_HOST, ORCA_PORT);
  let cachedTools = [];
  let orcaConnected = false;

  // Try to connect to OrcaSlicer (non-blocking — bridge starts even if Orca is down)
  async function ensureConnected() {
    if (orcaConnected && orca.connected) return;

    // If we lost the connection, clean up first
    if (!orca.connected && orcaConnected) {
      log('Lost connection to OrcaSlicer, reconnecting...');
      orca.destroy();
      orcaConnected = false;
    }

    await orca.initialize();
    orcaConnected = true;

    // Fetch tools
    const toolsResult = await orca.request('tools/list', {}, 10000);
    cachedTools = toolsResult?.result?.tools || [];
    log(`Discovered ${cachedTools.length} tools from OrcaSlicer`);
  }

  // ---- MCP Server (stdio side, facing Claude Code) ----

  const server = new Server(
    { name: 'orcaslicer', version: '1.0.0' },
    { capabilities: { tools: {} } }
  );

  // tools/list handler
  server.setRequestHandler(ListToolsRequestSchema, async () => {
    try {
      await ensureConnected();
    } catch (err) {
      log(`Connection failed during tools/list: ${err.message}`);
      return {
        tools: [{
          name: 'connection_status',
          description: `OrcaSlicer is not connected: ${err.message}. Start OrcaSlicer with --mcp-server flag.`,
          inputSchema: { type: 'object', properties: {} }
        }]
      };
    }
    return { tools: cachedTools };
  });

  // tools/call handler
  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    const { name, arguments: args } = request.params;

    // Special pseudo-tool for connection status
    if (name === 'connection_status') {
      try {
        await ensureConnected();
        return {
          content: [{ type: 'text', text: JSON.stringify({
            connected: true,
            session_id: orca.sessionId,
            tools_count: cachedTools.length
          }, null, 2) }]
        };
      } catch (err) {
        return {
          content: [{ type: 'text', text: `Not connected: ${err.message}` }],
          isError: true
        };
      }
    }

    // Ensure we're connected
    try {
      await ensureConnected();
    } catch (err) {
      return {
        content: [{
          type: 'text',
          text: `Cannot reach OrcaSlicer: ${err.message}\n\nStart OrcaSlicer with: --mcp-server`
        }],
        isError: true
      };
    }

    // Forward tool call to OrcaSlicer
    const timeout = getToolTimeout(name);
    try {
      const response = await orca.request(
        'tools/call',
        { name, arguments: args || {} },
        timeout
      );

      if (response.error) {
        return {
          content: [{ type: 'text', text: `OrcaSlicer error: ${response.error.message || JSON.stringify(response.error)}` }],
          isError: true
        };
      }

      // Pass through the result directly
      return response.result || { content: [{ type: 'text', text: 'No result returned' }] };
    } catch (err) {
      // On connection error, mark as disconnected for retry next time
      if (err.message.includes('SSE connection closed') || err.message.includes('ECONNREFUSED')) {
        orcaConnected = false;
      }
      return {
        content: [{ type: 'text', text: `Bridge error calling '${name}': ${err.message}` }],
        isError: true
      };
    }
  });

  // ---- Start ----

  // Try eager connection (best effort, bridge starts regardless)
  try {
    await ensureConnected();
    log(`Ready — ${cachedTools.length} tools available`);
  } catch (err) {
    log(`OrcaSlicer not available yet: ${err.message}`);
    log('Bridge started — will retry connection on first tool call');
  }

  // Connect stdio transport
  const transport = new StdioServerTransport();
  await server.connect(transport);
  log('stdio transport connected — bridge is live');

  // Graceful shutdown
  const cleanup = () => {
    orca.destroy();
    process.exit(0);
  };
  process.on('SIGINT', cleanup);
  process.on('SIGTERM', cleanup);
}

main().catch((err) => {
  log(`Fatal: ${err.message}`);
  process.exit(1);
});
