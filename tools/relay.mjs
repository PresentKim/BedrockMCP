import { spawn, execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { appendFileSync, existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.BEDROCK_RELAY_PORT ?? 18081);
const UPSTREAM = process.env.BEDROCK_MCP_URL ?? 'http://127.0.0.1:18080/mcp';
const INJECTOR = resolve(process.env.BEDROCK_MCP_INJECTOR ?? join(here, '..', 'build', 'relwithdebinfo', 'bedrock_mcp_injector.exe'));
const GAME_IMAGE = 'Minecraft.Windows.exe';
const GAME_URI = 'minecraft:';
const STARTUP_GRACE_MS = 8000;
const LAUNCH_TIMEOUT_MS = 120000;
const CALL_WAIT_MS = 20000;
const POLL_MS = 2000;

const dataDir = join(process.env.LOCALAPPDATA ?? here, 'BedrockMCP');
mkdirSync(dataDir, { recursive: true });
const logFile = join(dataDir, 'relay.log');
const toolsFile = join(dataDir, 'tools.json');

const log = (message) => appendFileSync(logFile, `${new Date().toISOString()} ${message}\n`);

const STATUS_TOOL = {
  name: 'game_status',
  description: 'Report whether Minecraft is running and whether the bedrock-mcp payload is injected and answering. Calling it launches Minecraft when it is not running.',
  inputSchema: { type: 'object', properties: {} },
};

const INSTRUCTIONS = [
  'This server is a relay that keeps Minecraft (Windows, GDK) reachable.',
  'When a tool returns an error that says Minecraft is starting or the payload is being injected, wait the suggested number of seconds and call the same tool again.',
  'The relay launches Minecraft by itself when it is not running, and injects the payload automatically.',
].join(' ');

const state = {
  phase: 'stopped',
  pid: 0,
  firstSeen: 0,
  injectedPid: 0,
  attempts: 0,
  lastError: '',
  launchedAt: 0,
  upstreamSession: '',
  upstreamOk: false,
};

function loadCachedTools() {
  try {
    return JSON.parse(readFileSync(toolsFile, 'utf8'));
  } catch {
    return [];
  }
}

let cachedTools = loadCachedTools();

function findGamePid() {
  return new Promise((done) => {
    execFile('tasklist', ['/FI', `IMAGENAME eq ${GAME_IMAGE}`, '/FO', 'CSV', '/NH'], { windowsHide: true }, (err, stdout) => {
      if (err) return done(0);
      const match = stdout.match(/^"[^"]+","(\d+)"/m);
      done(match ? Number(match[1]) : 0);
    });
  });
}

function runInjector() {
  return new Promise((done) => {
    if (!existsSync(INJECTOR)) return done({ code: -1, output: `injector not found at ${INJECTOR}` });
    execFile(INJECTOR, [], { windowsHide: true, timeout: 60000 }, (err, stdout, stderr) => {
      done({ code: err ? (err.code ?? 1) : 0, output: `${stdout}${stderr}`.trim() });
    });
  });
}

function launchGame() {
  if (state.launchedAt && Date.now() - state.launchedAt < LAUNCH_TIMEOUT_MS) return;
  state.launchedAt = Date.now();
  log('launching Minecraft');
  spawn('cmd.exe', ['/c', 'start', '', GAME_URI], { detached: true, stdio: 'ignore', windowsHide: true }).unref();
}

async function readRpc(response) {
  const text = await response.text();
  if ((response.headers.get('content-type') ?? '').includes('text/event-stream')) {
    const data = text.split('\n').filter((line) => line.startsWith('data:')).map((line) => line.slice(5).trim());
    return JSON.parse(data[data.length - 1]);
  }
  return JSON.parse(text);
}

async function upstreamPost(body, timeoutMs) {
  const headers = { 'Content-Type': 'application/json', Accept: 'application/json, text/event-stream' };
  if (state.upstreamSession) headers['Mcp-Session-Id'] = state.upstreamSession;
  return fetch(UPSTREAM, { method: 'POST', headers, body: JSON.stringify(body), signal: AbortSignal.timeout(timeoutMs) });
}

async function upstreamInitialize() {
  state.upstreamSession = '';
  const response = await upstreamPost({
    jsonrpc: '2.0', id: 'relay-init', method: 'initialize',
    params: { protocolVersion: '2025-03-26', capabilities: {}, clientInfo: { name: 'bedrock-relay', version: '1' } },
  }, 5000);
  if (!response.ok) throw new Error(`initialize HTTP ${response.status}`);
  await readRpc(response);
  state.upstreamSession = response.headers.get('mcp-session-id') ?? '';
  await upstreamPost({ jsonrpc: '2.0', method: 'notifications/initialized' }, 5000).catch(() => {});
}

async function upstreamRequest(method, params, timeoutMs = 60000) {
  const body = { jsonrpc: '2.0', id: randomUUID(), method, params };
  let response = await upstreamPost(body, timeoutMs);
  if (response.status === 400 || response.status === 404) {
    await upstreamInitialize();
    response = await upstreamPost(body, timeoutMs);
  }
  if (!response.ok) throw new Error(`${method} HTTP ${response.status}`);
  return readRpc(response);
}

async function refreshTools() {
  const reply = await upstreamRequest('tools/list', {}, 10000);
  const tools = reply.result?.tools;
  if (!Array.isArray(tools) || tools.length === 0) return;
  cachedTools = tools;
  writeFileSync(toolsFile, JSON.stringify(tools, null, 2));
}

async function tick() {
  const pid = await findGamePid();
  if (!pid) {
    if (state.pid) log(`Minecraft exited (pid ${state.pid})`);
    Object.assign(state, { pid: 0, firstSeen: 0, injectedPid: 0, attempts: 0, upstreamOk: false, upstreamSession: '' });
    state.phase = state.launchedAt && Date.now() - state.launchedAt < LAUNCH_TIMEOUT_MS ? 'launching' : 'stopped';
    return;
  }
  if (pid !== state.pid) {
    log(`Minecraft found (pid ${pid})`);
    Object.assign(state, { pid, firstSeen: Date.now(), injectedPid: 0, attempts: 0, lastError: '', upstreamOk: false, upstreamSession: '' });
  }
  state.launchedAt = 0;
  if (state.injectedPid !== pid) {
    if (Date.now() - state.firstSeen < STARTUP_GRACE_MS) {
      state.phase = 'starting';
      return;
    }
    state.phase = 'injecting';
    state.attempts += 1;
    const result = await runInjector();
    log(`inject attempt ${state.attempts} for pid ${pid}: exit ${result.code}: ${result.output.replace(/\s+/g, ' ')}`);
    if (result.code !== 0) {
      state.lastError = result.output.split('\n').pop() || `exit ${result.code}`;
      return;
    }
    state.injectedPid = pid;
  }
  if (!state.upstreamOk) {
    state.phase = 'connecting';
    try {
      await upstreamInitialize();
      await refreshTools();
      state.upstreamOk = true;
      state.phase = 'ready';
      log('payload MCP server is answering');
    } catch (err) {
      state.lastError = String(err.message ?? err);
    }
  }
}

async function loop() {
  for (;;) {
    try {
      await tick();
    } catch (err) {
      log(`tick failed: ${err.stack ?? err}`);
    }
    await new Promise((r) => setTimeout(r, POLL_MS));
  }
}

function describeState() {
  const since = (t) => Math.round((Date.now() - t) / 1000);
  switch (state.phase) {
    case 'ready':
      return { ready: true, text: `Minecraft is running (pid ${state.pid}) and the bedrock-mcp payload is answering.` };
    case 'stopped':
      return { ready: false, retry: 30, text: 'Minecraft is not running. The relay is launching it now.' };
    case 'launching':
      return { ready: false, retry: 15, text: `Minecraft is launching (${since(state.launchedAt)}s since launch). It usually reaches the title screen within a minute.` };
    case 'starting':
      return { ready: false, retry: 5, text: `Minecraft is starting (pid ${state.pid}, ${since(state.firstSeen)}s). The payload is injected a few seconds after the process appears.` };
    case 'injecting':
      return { ready: false, retry: 5, text: `Injecting the bedrock-mcp payload into Minecraft (pid ${state.pid}, attempt ${state.attempts}${state.lastError ? `, last error: ${state.lastError}` : ''}).` };
    default:
      return { ready: false, retry: 3, text: `The payload is injected into Minecraft (pid ${state.pid}) and its MCP server is starting${state.lastError ? ` (last error: ${state.lastError})` : ''}.` };
  }
}

async function waitForReady() {
  const deadline = Date.now() + CALL_WAIT_MS;
  while (state.phase !== 'ready' && Date.now() < deadline) {
    if (state.phase === 'stopped') launchGame();
    await new Promise((r) => setTimeout(r, 500));
  }
  return describeState();
}

const textResult = (text, isError = false) => ({ content: [{ type: 'text', text }], isError });

async function callTool(params) {
  if (params?.name === STATUS_TOOL.name) {
    if (state.phase === 'stopped') launchGame();
    const status = describeState();
    return textResult(status.ready ? status.text : `${status.text} Call game_status again in about ${status.retry} seconds.`);
  }
  const status = await waitForReady();
  if (!status.ready) return textResult(`${status.text} Call ${params?.name} again in about ${status.retry} seconds.`, true);
  try {
    const reply = await upstreamRequest('tools/call', params);
    if (reply.error) return textResult(`bedrock-mcp error: ${reply.error.message ?? JSON.stringify(reply.error)}`, true);
    return reply.result;
  } catch (err) {
    state.upstreamOk = false;
    state.phase = 'connecting';
    log(`forward ${params?.name} failed: ${err.message ?? err}`);
    return textResult(`The bedrock-mcp payload did not answer (${err.message ?? err}). Minecraft may have closed or restarted; the relay is reconnecting. Call ${params?.name} again in about 5 seconds.`, true);
  }
}

async function handle(message) {
  const { id, method, params } = message;
  if (id === undefined) return null;
  const reply = (result) => ({ jsonrpc: '2.0', id, result });
  switch (method) {
    case 'initialize':
      return reply({
        protocolVersion: params?.protocolVersion ?? '2025-03-26',
        capabilities: { tools: {} },
        serverInfo: { name: 'bedrock-relay', version: '1.0.0' },
        instructions: INSTRUCTIONS,
      });
    case 'ping':
      return reply({});
    case 'tools/list':
      return reply({ tools: [STATUS_TOOL, ...cachedTools] });
    case 'tools/call':
      return reply(await callTool(params));
    default:
      return { jsonrpc: '2.0', id, error: { code: -32601, message: `Method not found: ${method}` } };
  }
}

const server = createServer(async (req, res) => {
  if (!req.url?.startsWith('/mcp')) {
    res.writeHead(404).end();
    return;
  }
  if (req.method === 'DELETE') {
    res.writeHead(200).end();
    return;
  }
  if (req.method !== 'POST') {
    res.writeHead(405, { Allow: 'POST, DELETE' }).end();
    return;
  }
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  let body;
  try {
    body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  } catch {
    res.writeHead(400, { 'Content-Type': 'application/json' }).end(JSON.stringify({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } }));
    return;
  }
  const messages = Array.isArray(body) ? body : [body];
  const replies = (await Promise.all(messages.map(handle))).filter(Boolean);
  const headers = { 'Content-Type': 'application/json' };
  if (messages.some((m) => m.method === 'initialize')) headers['Mcp-Session-Id'] = randomUUID();
  if (replies.length === 0) {
    res.writeHead(202, headers).end();
    return;
  }
  res.writeHead(200, headers).end(JSON.stringify(Array.isArray(body) ? replies : replies[0]));
});

server.on('error', (err) => {
  log(`listen failed: ${err.message}`);
  process.exit(err.code === 'EADDRINUSE' ? 0 : 1);
});

server.listen(PORT, '127.0.0.1', () => {
  log(`relay listening on 127.0.0.1:${PORT}, upstream ${UPSTREAM}, injector ${INJECTOR}`);
  loop();
});
