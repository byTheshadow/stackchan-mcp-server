

import express from 'express';
import cors from 'cors';
import { WebSocketServer, WebSocket } from 'ws';
import http from 'http';
import { spawn } from 'child_process';
import ffmpegPath from 'ffmpeg-static';
import crypto from 'crypto';
import dns from 'dns/promises';
import net from 'net';


/*
 * ============================================================
 * 基础配置
 * ============================================================
 */

const app = express();

const PORT = Number(process.env.PORT || 3000);

const API_TOKEN =
  typeof process.env.API_TOKEN === 'string'
    ? process.env.API_TOKEN.trim()
    : '';

const ROBOT_WS_TOKEN =
  typeof process.env.ROBOT_WS_TOKEN === 'string'
    ? process.env.ROBOT_WS_TOKEN.trim()
    : '';

/*
 * 允许访问的音频域名。
 *
 * 推荐配置：
 *
 * AUDIO_ALLOWED_HOSTS=cdn.example.com,files.example.com
 *
 * 如果为空，则使用私网地址拦截策略。
 * 生产环境建议配置白名单。
 */
const AUDIO_ALLOWED_HOSTS = new Set(
  (process.env.AUDIO_ALLOWED_HOSTS || '')
    .split(',')
    .map((item) => item.trim().toLowerCase())
    .filter(Boolean)
);


/*
 * CORS 不等于身份认证。
 *
 * 如果设置 ALLOWED_ORIGINS，则只允许这些来源。
 * 例如：
 *
 * ALLOWED_ORIGINS=https://example.com,https://app.example.com
 */
const allowedOrigins = new Set(
  (process.env.ALLOWED_ORIGINS || '')
    .split(',')
    .map((item) => item.trim())
    .filter(Boolean)
);

if (allowedOrigins.size > 0) {
  app.use(
    cors({
      origin(origin, callback) {
        /*
         * 允许无 Origin 的服务端请求，例如 curl、MCP 客户端。
         */
        if (!origin || allowedOrigins.has(origin)) {
          callback(null, true);
          return;
        }

        callback(new Error('CORS origin is not allowed'));
      }
    })
  );
} else {
  /*
   * 如果没有配置白名单，仍保持兼容，但必须依靠 API_TOKEN。
   */
  app.use(cors());
}

app.use(
  express.json({
    limit: '64kb'
  })
);

const server = http.createServer(app);
const wss = new WebSocketServer({
  server,
  maxPayload: 64 * 1024
});


/*
 * ============================================================
 * 通用工具
 * ============================================================
 */

function safeEqualString(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') {
    return false;
  }

  const aBuffer = Buffer.from(a);
  const bBuffer = Buffer.from(b);

  if (aBuffer.length !== bBuffer.length) {
    return false;
  }

  return crypto.timingSafeEqual(aBuffer, bBuffer);
}


function getBearerToken(req) {
  const authorization = req.headers.authorization;

  if (typeof authorization !== 'string') {
    return '';
  }

  if (!authorization.startsWith('Bearer ')) {
    return '';
  }

  return authorization.slice('Bearer '.length).trim();
}


function requireApiAuth(req, res, next) {
  /*
   * 没有配置 API_TOKEN 时拒绝启动后的所有控制 API。
   * 这样可以避免忘记配置认证导致公网裸奔。
   */
  if (!API_TOKEN) {
    return res.status(503).json({
      error: 'Server API_TOKEN is not configured'
    });
  }

  const receivedToken = getBearerToken(req);

  if (!safeEqualString(receivedToken, API_TOKEN)) {
    return res.status(401).json({
      error: 'Unauthorized'
    });
  }

  next();
}


function getWebSocketToken(req) {
  const authorization = req.headers.authorization;

  if (
    typeof authorization === 'string' &&
    authorization.startsWith('Bearer ')
  ) {
    return authorization.slice('Bearer '.length).trim();
  }

  const requestUrl = new URL(
    req.url || '/',
    `http://${req.headers.host || 'localhost'}`
  );

  return requestUrl.searchParams.get('token') || '';
}


function isPrivateIpAddress(address) {
  const version = net.isIP(address);

  if (version === 4) {
    const parts = address.split('.').map(Number);

    if (parts.length !== 4 || parts.some(Number.isNaN)) {
      return true;
    }

    const [a, b] = parts;

    return (
      a === 0 ||
      a === 10 ||
      a === 127 ||
      (a === 100 && b >= 64 && b <= 127) ||
      (a === 169 && b === 254) ||
      (a === 172 && b >= 16 && b <= 31) ||
      (a === 192 && b === 168) ||
      a >= 224
    );
  }

  if (version === 6) {
    const normalized = address.toLowerCase();

    return (
      normalized === '::' ||
      normalized === '::1' ||
      normalized.startsWith('fc') ||
      normalized.startsWith('fd') ||
      normalized.startsWith('fe8') ||
      normalized.startsWith('fe9') ||
      normalized.startsWith('fea') ||
      normalized.startsWith('feb')
    );
  }

  return true;
}


function isHostnameAllowed(hostname) {
  const normalized = hostname.toLowerCase();

  if (AUDIO_ALLOWED_HOSTS.size === 0) {
    return true;
  }

  for (const allowedHost of AUDIO_ALLOWED_HOSTS) {
    if (
      normalized === allowedHost ||
      normalized.endsWith(`.${allowedHost}`)
    ) {
      return true;
    }
  }

  return false;
}


async function validateAudioTarget(parsedUrl) {
  const hostname = parsedUrl.hostname.toLowerCase();

  if (!isHostnameAllowed(hostname)) {
    return {
      ok: false,
      error: 'Audio host is not allow-listed'
    };
  }

  /*
   * 直接访问 IP 时立即检查。
   */
  if (net.isIP(hostname)) {
    if (isPrivateIpAddress(hostname)) {
      return {
        ok: false,
        error: 'Private or local audio address is not allowed'
      };
    }

    return {
      ok: true
    };
  }

  /*
   * 解析域名并检查所有返回地址。
   */
  try {
    const addresses = await dns.lookup(hostname, {
      all: true,
      verbatim: true
    });

    if (
      !Array.isArray(addresses) ||
      addresses.length === 0 ||
      addresses.some((item) => isPrivateIpAddress(item.address))
    ) {
      return {
        ok: false,
        error: 'Audio host resolves to a private or local address'
      };
    }
  } catch {
    return {
      ok: false,
      error: 'Audio host DNS lookup failed'
    };
  }

  return {
    ok: true
  };
}


function jsonRpcSuccess(id, result) {
  return {
    jsonrpc: '2.0',
    id,
    result
  };
}


function jsonRpcError(id, code, message, data) {
  const error = {
    code,
    message
  };

  if (data !== undefined) {
    error.data = data;
  }

  return {
    jsonrpc: '2.0',
    id: id ?? null,
    error
  };
}


/*
 * ============================================================
 * 机器人连接
 * ============================================================
 */

let robotSocket = null;


/*
 * 等待机器人返回的音频状态。
 *
 * key 格式：
 *
 * stream_id:status_type
 */
const robotStatusWaiters = new Map();


function robotStatusKey(streamId, type) {
  return `${streamId}:${type}`;
}


function waitForRobotStatus(streamId, types, timeoutMs) {
  const wantedTypes = Array.isArray(types) ? types : [types];

  return new Promise((resolve, reject) => {
    const entries = [];

    const timer = setTimeout(() => {
      for (const entry of entries) {
        robotStatusWaiters.delete(entry.key);
      }

      reject(
        new Error(
          `Timed out waiting for robot status: ${wantedTypes.join(', ')}`
        )
      );
    }, timeoutMs);

    for (const type of wantedTypes) {
      const key = robotStatusKey(streamId, type);

      const entry = {
        key,
        resolve,
        reject,
        timer,
        wantedTypes
      };

      entries.push(entry);
      robotStatusWaiters.set(key, entry);
    }
  });
}


function resolveRobotStatus(data) {
  if (!data || typeof data !== 'object') {
    return false;
  }

  const streamId = data.stream_id;
  const type = data.type;

  if (
    typeof streamId !== 'string' ||
    typeof type !== 'string'
  ) {
    return false;
  }

  const key = robotStatusKey(streamId, type);
  const waiter = robotStatusWaiters.get(key);

  if (!waiter) {
    return false;
  }

  clearTimeout(waiter.timer);

  for (const candidateType of waiter.wantedTypes) {
    robotStatusWaiters.delete(
      robotStatusKey(streamId, candidateType)
    );
  }

  if (type === 'audio_error') {
    waiter.reject(
      new Error(data.error || 'Robot audio error')
    );
  } else {
    waiter.resolve(data);
  }

  return true;
}


function rejectAllRobotStatusWaiters(error) {
  for (const waiter of robotStatusWaiters.values()) {
    clearTimeout(waiter.timer);
    waiter.reject(error);
  }

  robotStatusWaiters.clear();
}


function sendRobotJson(payload) {
  const ws = robotSocket;

  if (!ws || ws.readyState !== WebSocket.OPEN) {
    return false;
  }

  try {
    ws.send(JSON.stringify(payload));
    return true;
  } catch (error) {
    console.error('[WS] Failed to send JSON:', error.message);
    return false;
  }
}


async function waitForRobotSendBuffer(deadlineMs) {
  while (
    robotSocket &&
    robotSocket.readyState === WebSocket.OPEN &&
    robotSocket.bufferedAmount > ROBOT_BUFFER_LIMIT_BYTES
  ) {
    if (Date.now() >= deadlineMs) {
      throw new Error('Robot WebSocket send buffer timeout');
    }

    await new Promise((resolve) => {
      setTimeout(resolve, 20);
    });
  }

  if (
    !robotSocket ||
    robotSocket.readyState !== WebSocket.OPEN
  ) {
    throw new Error('Robot disconnected');
  }

  return true;
}


wss.on('connection', (ws, req) => {
  /*
   * 机器人 WebSocket 必须配置 ROBOT_WS_TOKEN。
   */
  if (!ROBOT_WS_TOKEN) {
    console.error(
      '[WS] ROBOT_WS_TOKEN is not configured; connection rejected'
    );

    ws.close(1008, 'Server authentication is not configured');
    return;
  }

  const receivedToken = getWebSocketToken(req);

  if (!safeEqualString(receivedToken, ROBOT_WS_TOKEN)) {
    console.warn(
      '[WS] Rejected unauthenticated connection:',
      req.socket.remoteAddress
    );

    ws.close(1008, 'Unauthorized');
    return;
  }

  /*
   * 当前只支持一台机器人。
   * 不允许新连接覆盖已连接的机器人。
   */
  if (
    robotSocket &&
    robotSocket.readyState === WebSocket.OPEN
  ) {
    console.warn('[WS] Rejected second robot connection');
    ws.close(1013, 'Robot already connected');
    return;
  }

  robotSocket = ws;

  console.log(
    '[WS] Authenticated StackChan robot connected:',
    req.socket.remoteAddress
  );

  ws.on('message', (message, isBinary) => {
    if (isBinary) {
      console.warn(
        '[WS] Ignored unexpected binary frame from robot'
      );
      return;
    }

    const text = message.toString();

    let data;

    try {
      data = JSON.parse(text);
    } catch {
      console.warn('[WS] Ignored invalid JSON from robot');
      return;
    }

    /*
     * 音频状态消息优先交给等待器。
     */
    if (resolveRobotStatus(data)) {
      return;
    }

    /*
     * 只接受明确的机器人事件。
     */
    if (
      data &&
      data.type === 'robot_event' &&
      (
        data.event === 'touch_tap' ||
        data.event === 'head_touch' ||
        data.event === 'shake'
      ) &&
      Number.isFinite(data.at_ms)
    ) {
      if (
        data.event === 'touch_tap' &&
        (
          !Number.isInteger(data.x) ||
          !Number.isInteger(data.y)
        )
      ) {
        return;
      }

      addRobotEvent(data);
      return;
    }

    console.log('[WS] Unhandled robot message:', data);
  });

  ws.on('close', () => {
    if (robotSocket === ws) {
      robotSocket = null;

      rejectAllRobotStatusWaiters(
        new Error('Robot WebSocket disconnected')
      );

      if (currentAudioJob) {
        currentAudioJob.cancelled = true;

        if (currentAudioJob.controller) {
          currentAudioJob.controller.abort();
        }

        if (
          currentAudioJob.ffmpeg &&
          !currentAudioJob.ffmpeg.killed
        ) {
          currentAudioJob.ffmpeg.kill('SIGTERM');
        }

        currentAudioJob = null;
      }
    }

    console.log('[WS] StackChan robot disconnected');
  });

  ws.on('error', (error) => {
    console.error('[WS] Robot WebSocket error:', error.message);
  });
});


/*
 * ============================================================
 * 实体事件
 * ============================================================
 */

const pendingRobotEvents = [];

const MAX_PENDING_EVENTS = 50;
const EVENT_TTL_MS = 24 * 60 * 60 * 1000;


function makeRobotEventId() {
  return `evt_${Date.now().toString(36)}_${crypto
    .randomBytes(6)
    .toString('hex')}`;
}


function pruneExpiredRobotEvents() {
  const now = Date.now();

  for (
    let index = pendingRobotEvents.length - 1;
    index >= 0;
    index--
  ) {
    const event = pendingRobotEvents[index];
    const receivedAtMs = Date.parse(event.received_at);

    if (
      Number.isNaN(receivedAtMs) ||
      now - receivedAtMs > EVENT_TTL_MS
    ) {
      pendingRobotEvents.splice(index, 1);
    }
  }
}


function addRobotEvent(rawEvent) {
  pruneExpiredRobotEvents();

  const event = {
    id: makeRobotEventId(),
    source: 'stackchan',
    event: rawEvent.event,
    x: rawEvent.x,
    y: rawEvent.y,
    device_uptime_ms: rawEvent.at_ms,
    received_at: new Date().toISOString()
  };

  pendingRobotEvents.push(event);

  while (pendingRobotEvents.length > MAX_PENDING_EVENTS) {
    pendingRobotEvents.shift();
  }

  console.log('[EVENT] Robot event recorded:', event);

  return event;
}


function getPendingRobotEvents() {
  pruneExpiredRobotEvents();

  const now = Date.now();

  return pendingRobotEvents.map((event) => {
    const receivedAtMs = Date.parse(event.received_at);

    return {
      ...event,
      seconds_ago: Math.max(
        0,
        Math.floor((now - receivedAtMs) / 1000)
      )
    };
  });
}


function acknowledgeRobotEvents(eventIds) {
  pruneExpiredRobotEvents();

  const idSet = new Set(eventIds);
  let acknowledged = 0;

  for (
    let index = pendingRobotEvents.length - 1;
    index >= 0;
    index--
  ) {
    if (idSet.has(pendingRobotEvents[index].id)) {
      pendingRobotEvents.splice(index, 1);
      acknowledged++;
    }
  }

  return acknowledged;
}


function validateEventIds(eventIds) {
  return (
    Array.isArray(eventIds) &&
    eventIds.length >= 1 &&
    eventIds.length <= 50 &&
    eventIds.every(
      (item) =>
        typeof item === 'string' &&
        item.length >= 1 &&
        item.length <= 128
    )
  );
}


/*
 * ============================================================
 * 音频处理
 * ============================================================
 */

const MAX_AUDIO_INPUT_BYTES = 20 * 1024 * 1024;
const MAX_AUDIO_OUTPUT_BYTES = 20 * 1024 * 1024;
const AUDIO_TIMEOUT_MS = 120000;
const ROBOT_BUFFER_LIMIT_BYTES = 128 * 1024;
const AUDIO_PCM_CHUNK_BYTES = 4096;
const ROBOT_STATUS_TIMEOUT_MS = 15000;

let currentAudioJob = null;
let audioGeneration = 0;


function makeAudioStreamId() {
  return `audio_${Date.now().toString(36)}_${crypto
    .randomBytes(6)
    .toString('hex')}`;
}


async function normalizeAudioUrl(value) {
  if (typeof value !== 'string' || value.length === 0) {
    return null;
  }

  if (value.length > 2048) {
    return null;
  }

  let parsed;

  try {
    parsed = new URL(value);
  } catch {
    return null;
  }

  if (
    parsed.protocol !== 'http:' &&
    parsed.protocol !== 'https:'
  ) {
    return null;
  }

  /*
   * 不允许 URL 中携带用户名和密码。
   */
  if (parsed.username || parsed.password) {
    return null;
  }

  /*
   * 防止 localhost、内网 IP、云元数据地址。
   */
  const validation = await validateAudioTarget(parsed);

  if (!validation.ok) {
    console.warn('[AUDIO] Rejected URL:', validation.error);
    return null;
  }

  return parsed.toString();
}


function stopCurrentAudioJob(reason = 'stopped') {
  const job = currentAudioJob;

  if (!job) {
    sendRobotJson({
      type: 'audio_stop',
      reason
    });

    return;
  }

  job.cancelled = true;
  audioGeneration++;

  if (job.timeoutHandle) {
    clearTimeout(job.timeoutHandle);
  }

  if (job.controller) {
    job.controller.abort();
  }

  if (
    job.ffmpeg &&
    !job.ffmpeg.killed
  ) {
    job.ffmpeg.kill('SIGTERM');
  }

  sendRobotJson({
    type: 'audio_stop',
    stream_id: job.streamId,
    reason
  });

  if (currentAudioJob === job) {
    currentAudioJob = null;
  }

  console.log(`[AUDIO] Current audio stopped: ${reason}`);
}


async function playRobotAudio(args = {}) {
  const audioUrl = await normalizeAudioUrl(args.audio_url);

  if (!audioUrl) {
    return {
      ok: false,
      status: 400,
      error:
        'audio_url 必须是有效且允许访问的 HTTP/HTTPS URL。'
    };
  }

  if (
    !robotSocket ||
    robotSocket.readyState !== WebSocket.OPEN
  ) {
    return {
      ok: false,
      status: 503,
      error:
        'Robot is offline. Please check Wi-Fi and WebSocket connection.'
    };
  }

  if (!ffmpegPath) {
    return {
      ok: false,
      status: 500,
      error: 'ffmpeg-static 未提供可执行文件。'
    };
  }

  if (currentAudioJob) {
    stopCurrentAudioJob('replaced_by_new_audio');
  }

  const streamId = makeAudioStreamId();
  const controller = new AbortController();
  const generation = ++audioGeneration;

  const ffmpeg = spawn(
    ffmpegPath,
    [
      '-hide_banner',
      '-loglevel',
      'error',
      '-nostdin',

      '-i',
      'pipe:0',

      '-map',
      '0:a:0',
      '-vn',
      '-sn',
      '-dn',

      '-acodec',
      'pcm_s16le',
      '-ar',
      '16000',
      '-ac',
      '1',

      '-f',
      's16le',
      'pipe:1'
    ],
    {
      stdio: ['pipe', 'pipe', 'pipe']
    }
  );

 const job = {
  streamId,
  controller,
  ffmpeg,
  cancelled: false,
  generation,
  inputBytes: 0,
  outputBytes: 0,
  timeoutHandle: null,

  // 如果上一次收到奇数个 PCM 字节，就暂存最后 1 字节
  pendingPcmByte: null
};


  currentAudioJob = job;

  



  job.timeoutHandle = setTimeout(() => {
    stopCurrentAudioJob('audio_timeout');
  }, AUDIO_TIMEOUT_MS);

  const ensureCurrentJob = () => {
    if (
      job.cancelled ||
      currentAudioJob !== job ||
      audioGeneration !== generation
    ) {
      throw new Error('Audio job was superseded');
    }
  };

  const failJob = (error) => {
    if (job.cancelled) {
      return;
    }

    job.cancelled = true;

    console.error('[AUDIO] Audio job failed:', error.message);

    if (job.controller) {
      job.controller.abort();
    }

    if (
      job.ffmpeg &&
      !job.ffmpeg.killed
    ) {
      job.ffmpeg.kill('SIGTERM');
    }

    sendRobotJson({
      type: 'audio_abort',
      stream_id: streamId,
      error: String(error.message || error)
    });
  };

  try {
    /*
     * 注册等待器必须在发送 audio_start 之前完成。
     */
    const readyPromise = waitForRobotStatus(
      streamId,
      ['audio_ready', 'audio_error'],
      ROBOT_STATUS_TIMEOUT_MS
    );

    if (
      !sendRobotJson({
        type: 'audio_start',
        stream_id: streamId,
        format: 'pcm_s16le',
        sample_rate: 16000,
        channels: 1,
        bits_per_sample: 16
      })
    ) {
      throw new Error('Robot disconnected before audio_start');
    }

    await readyPromise;
    ensureCurrentJob();

    const response = await fetch(audioUrl, {
      signal: controller.signal,
      redirect: 'error'
    });

    if (!response.ok || !response.body) {
      throw new Error(
        `Audio download failed: HTTP ${response.status}`
      );
    }

    const contentLength =
      response.headers.get('content-length');

    if (
      contentLength &&
      Number.isFinite(Number(contentLength)) &&
      Number(contentLength) > MAX_AUDIO_INPUT_BYTES
    ) {
      throw new Error('Input audio file is larger than 20 MB');
    }

    const inputPump = (async () => {
      try {
        for await (const chunk of response.body) {
          ensureCurrentJob();

          job.inputBytes += chunk.length;

          if (job.inputBytes > MAX_AUDIO_INPUT_BYTES) {
            throw new Error(
              'Input audio file exceeded 20 MB'
            );
          }

          if (!ffmpeg.stdin.write(chunk)) {
            await new Promise((resolve, reject) => {
              const onDrain = () => {
                cleanup();
                resolve();
              };

              const onError = (error) => {
                cleanup();
                reject(error);
              };

              const cleanup = () => {
                ffmpeg.stdin.off('drain', onDrain);
                ffmpeg.stdin.off('error', onError);
              };

              ffmpeg.stdin.once('drain', onDrain);
              ffmpeg.stdin.once('error', onError);
            });
          }
        }

        if (!job.cancelled) {
          ffmpeg.stdin.end();
        }
      } catch (error) {
        if (!ffmpeg.stdin.destroyed) {
          ffmpeg.stdin.destroy(error);
        }

        throw error;
      }
    })();

    const outputPump = (async () => {
  for await (const chunk of ffmpeg.stdout) {
    ensureCurrentJob();

    let pcmChunk = chunk;

    /*
     * 如果上一次留下了 1 个字节，
     * 先和这次收到的数据拼起来。
     */
    if (job.pendingPcmByte !== null) {
      const combined = Buffer.allocUnsafe(
        pcmChunk.length + 1
      );

      combined[0] = job.pendingPcmByte;
      pcmChunk.copy(combined, 1);

      pcmChunk = combined;
      job.pendingPcmByte = null;
    }

    /*
     * s16le PCM 每个采样点占 2 字节。
     *
     * 如果当前数据长度是奇数，
     * 暂存最后 1 字节，留到下一轮拼接。
     */
    if ((pcmChunk.length & 1) !== 0) {
      job.pendingPcmByte =
        pcmChunk[pcmChunk.length - 1];

      pcmChunk = pcmChunk.subarray(
        0,
        pcmChunk.length - 1
      );
    }

    /*
     * 当前只有 1 个字节，先不发送。
     */
    if (pcmChunk.length === 0) {
      continue;
    }

    job.outputBytes += pcmChunk.length;

    if (
      job.outputBytes > MAX_AUDIO_OUTPUT_BYTES
    ) {
      throw new Error(
        'Decoded PCM exceeded 20 MB'
      );
    }

    /*
     * 每次最多发送 4096 字节。
     */
    for (
      let offset = 0;
      offset < pcmChunk.length;
      offset += AUDIO_PCM_CHUNK_BYTES
    ) {
      ensureCurrentJob();

      const end = Math.min(
        offset + AUDIO_PCM_CHUNK_BYTES,
        pcmChunk.length
      );

      const audioChunk =
        pcmChunk.subarray(offset, end);

      /*
       * 经过前面的处理后，
       * 这里理论上一定是偶数长度。
       */
      if ((audioChunk.length & 1) !== 0) {
        throw new Error(
          'Internal PCM alignment error'
        );
      }

      await waitForRobotSendBuffer(
        Date.now() + AUDIO_TIMEOUT_MS
      );

      ensureCurrentJob();

      const ws = robotSocket;

      if (
        !ws ||
        ws.readyState !== WebSocket.OPEN
      ) {
        throw new Error(
          'Robot disconnected during PCM send'
        );
      }

      ws.send(audioChunk, {
        binary: true
      });
    }
  }

  /*
   * 整个 ffmpeg 输出结束后，
   * 如果还剩 1 个字节，说明 PCM 数据不完整。
   */
  if (job.pendingPcmByte !== null) {
    throw new Error(
      'Decoded PCM ended with an incomplete sample'
    );
  }
})();


    let stderr = '';

    ffmpeg.stderr.on('data', (data) => {
      stderr += data.toString();

      if (stderr.length > 4096) {
        stderr = stderr.slice(-4096);
      }
    });

    const ffmpegExitPromise = new Promise((resolve, reject) => {
      ffmpeg.once('error', reject);

      ffmpeg.once('close', (code, signal) => {
        resolve({
          code,
          signal
        });
      });
    });

    await Promise.all([
      inputPump,
      outputPump
    ]);

    ensureCurrentJob();

    const exit = await ffmpegExitPromise;

    if (exit.code !== 0) {
      throw new Error(
        `ffmpeg exited with code ${exit.code}: ${stderr}`
      );
    }

    if (job.outputBytes === 0) {
      throw new Error(
        'ffmpeg produced no PCM audio'
      );
    }

    ensureCurrentJob();

    const startedPromise = waitForRobotStatus(
      streamId,
      ['audio_started', 'audio_error'],
      ROBOT_STATUS_TIMEOUT_MS
    );

    if (
      !sendRobotJson({
        type: 'audio_end',
        stream_id: streamId,
        pcm_bytes: job.outputBytes
      })
    ) {
      throw new Error(
        'Robot disconnected before audio_end'
      );
    }

    await startedPromise;

    ensureCurrentJob();

    console.log(
      `[AUDIO] Completed stream ${streamId}: ` +
      `${job.inputBytes} input bytes, ` +
      `${job.outputBytes} PCM bytes`
    );

    return {
      ok: true,
      stream_id: streamId,
      input_bytes: job.inputBytes,
      pcm_bytes: job.outputBytes
    };
  } catch (error) {
    failJob(error);

    return {
      ok: false,
      status: 502,
      error: `音频处理失败：${error.message}`
    };
  } finally {
    if (job.timeoutHandle) {
      clearTimeout(job.timeoutHandle);
    }

    if (currentAudioJob === job) {
      currentAudioJob = null;
    }
  }
}


/*
 * ============================================================
 * 控制参数与 MCP 工具定义
 * ============================================================
 */

function sanitizeDisplayText(value) {
  if (typeof value !== 'string') {
    return '';
  }

  return value
    .replace(/[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g, '')
    .slice(0, 80);
}


function normalizeDisplayDuration(value) {
  const defaultDurationMs = 5000;
  const minDurationMs = 1000;
  const maxDurationMs = 10000;

  if (!Number.isInteger(value)) {
    return defaultDurationMs;
  }

  return Math.min(
    maxDurationMs,
    Math.max(minDurationMs, value)
  );
}


function normalizeRobotArguments(args = {}) {
  if (!args || typeof args !== 'object') {
    args = {};
  }

  const allowedExpressions = new Set([
    'happy',
    'sad',
    'angry',
    'doubt',
    'sleepy',
    'neutral'
  ]);

  const allowedFaceEffects = new Set([
    'none',
    'heart_eyes',
    'sparkle_eyes',
    'dizzy_eyes',
    'tear_eyes',
    'surprised_face',
    'pout_face',
    'shy_face',
    'smug_face',
    'confused_face',
    'laugh_face',
    'kiss_face',
    'nervous_face',
    'relieved_face',
    'determined_face'
  ]);

  const allowedMotions = new Set([
    'nod',
    'shake_head',
    'look_left',
    'look_right',
    'tilt_up',
    'home',
    'none'
  ]);

  const allowedSounds = new Set([
    'none',
    'message',
    'emotion'
  ]);

  const normalized = {
    expression: allowedExpressions.has(args.expression)
      ? args.expression
      : 'neutral',

    face_effect: allowedFaceEffects.has(args.face_effect)
      ? args.face_effect
      : 'none',

    motion: allowedMotions.has(args.motion)
      ? args.motion
      : 'none',

    sound: allowedSounds.has(args.sound)
      ? args.sound
      : 'none'
  };

  if (typeof args.text_to_display === 'string') {
    normalized.text_to_display =
      sanitizeDisplayText(args.text_to_display);

    normalized.display_duration_ms =
      normalizeDisplayDuration(
        args.display_duration_ms
      );
  }

  return normalized;
}


function sendRobotControl(args = {}) {
  const payload = {
    action: 'speak',
    ...normalizeRobotArguments(args)
  };

  if (
    !robotSocket ||
    robotSocket.readyState !== WebSocket.OPEN
  ) {
    return {
      ok: false,
      status: 503,
      error:
        'Robot is offline. Please check Wi-Fi and WebSocket connection.'
    };
  }

  if (!sendRobotJson(payload)) {
    return {
      ok: false,
      status: 503,
      error: 'Failed to send command to robot'
    };
  }

  console.log('[CONTROL] Sent to robot:', payload);

  return {
    ok: true,
    payload
  };
}


function formatRobotEventsText(events) {
  if (events.length === 0) {
    return '没有尚未处理的实体事件。';
  }

  const lines = events.map((event) => {
    if (event.event === 'touch_tap') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前触摸了 ` +
        `StackChan 屏幕一次（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'head_touch') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前摸了 ` +
        `StackChan 的头顶一次（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'shake') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前摇晃了 ` +
        `StackChan 一次（事件 ID：${event.id}）。`
      );
    }

    return (
      `- 未知实体事件：${event.event} ` +
      `（事件 ID：${event.id}）。`
    );
  });

  return (
    `尚未处理的实体事件：\n${lines.join('\n')}`
  );
}


const controlRobotTool = {
  name: 'control_robot',

  description:
    '控制 StackChan 的表情、脸部效果、屏幕文字、保守舵机动作和短提示音。支持 nod、shake_head、look_left、look_right、tilt_up、home 和 none。',

  inputSchema: {
    type: 'object',

    properties: {
      expression: {
        type: 'string',
        enum: [
          'happy',
          'sad',
          'angry',
          'doubt',
          'sleepy',
          'neutral'
        ]
      },

      face_effect: {
        type: 'string',
        enum: [
          'none',
          'heart_eyes',
          'sparkle_eyes',
          'dizzy_eyes',
          'tear_eyes',
          'surprised_face',
          'pout_face',
          'shy_face',
          'smug_face',
          'confused_face',
          'laugh_face',
          'kiss_face',
          'nervous_face',
          'relieved_face',
          'determined_face'
        ]
      },

      motion: {
        type: 'string',
        enum: [
          'nod',
          'shake_head',
          'look_left',
          'look_right',
          'tilt_up',
          'home',
          'none'
        ]
      },

      text_to_display: {
        type: 'string',
        maxLength: 80
      },

      display_duration_ms: {
        type: 'integer',
        minimum: 1000,
        maximum: 10000
      },

      sound: {
        type: 'string',
        enum: [
          'none',
          'message',
          'emotion'
        ]
      },

      text_to_speak: {
        type: 'string'
      }
    },

    required: [
      'expression'
    ]
  }
};


const getRobotEventsTool = {
  name: 'get_robot_events',

  description:
    '读取 StackChan 尚未处理的 touch_tap、head_touch 和 shake 事件。读取不会删除事件。',

  inputSchema: {
    type: 'object',
    properties: {}
  }
};


const acknowledgeRobotEventsTool = {
  name: 'acknowledge_robot_events',

  description:
    '确认并删除已经处理的 StackChan 实体事件。',

  inputSchema: {
    type: 'object',

    properties: {
      event_ids: {
        type: 'array',
        minItems: 1,
        maxItems: 50,
        items: {
          type: 'string',
          maxLength: 128
        }
      }
    },

    required: [
      'event_ids'
    ]
  }
};


const playRobotAudioTool = {
  name: 'play_robot_audio',

  description:
    '播放远程 HTTP/HTTPS 音频。服务端使用 ffmpeg 转换为 16000 Hz、16-bit、单声道 PCM，并等待机器人确认接收和开始播放。',

  inputSchema: {
    type: 'object',

    properties: {
      audio_url: {
        type: 'string',
        format: 'uri'
      }
    },

    required: [
      'audio_url'
    ]
  }
};


const stopRobotAudioTool = {
  name: 'stop_robot_audio',

  description:
    '停止当前正在传输或播放的机器人语音，并清理机器人上的临时音频文件。',

  inputSchema: {
    type: 'object',
    properties: {}
  }
};


const allTools = [
  controlRobotTool,
  playRobotAudioTool,
  stopRobotAudioTool,
  getRobotEventsTool,
  acknowledgeRobotEventsTool
];


/*
 * ============================================================
 * MCP 接口
 * ============================================================
 */

app.post('/mcp', requireApiAuth, async (req, res) => {
  const request = req.body;

  if (
    !request ||
    request.jsonrpc !== '2.0' ||
    typeof request.method !== 'string'
  ) {
    return res
      .status(400)
      .json(
        jsonRpcError(
          null,
          -32600,
          'Invalid JSON-RPC request'
        )
      );
  }

  const {
    id,
    method,
    params = {}
  } = request;

  const isNotification =
    id === undefined ||
    id === null;

  if (
    params === null ||
    typeof params !== 'object'
  ) {
    if (isNotification) {
      return res.status(202).end();
    }

    return res
      .status(200)
      .json(
        jsonRpcError(
          id,
          -32602,
          'Invalid params'
        )
      );
  }

  console.log('[MCP] Request:', method);

  if (method === 'initialize') {
    if (isNotification) {
      return res.status(202).end();
    }

    const result = {
      protocolVersion: '2025-03-26',

      capabilities: {
        tools: {}
      },

      serverInfo: {
        name: 'stackchan-robot-relay',
        version: '2.0.0'
      },

      instructions:
        '此服务负责 StackChan 实体身体的上下行中继。支持基础表情、脸部效果、短屏幕文字、保守舵机动作、短提示音、远程音频播放，以及 touch_tap、head_touch、shake 事件读取与确认。'
    };

    return res
      .status(200)
      .type('application/json')
      .json(jsonRpcSuccess(id, result));
  }

  if (method === 'notifications/initialized') {
    return res.status(202).end();
  }

  if (method === 'tools/list') {
    if (isNotification) {
      return res.status(202).end();
    }

    return res
      .status(200)
      .type('application/json')
      .json(
        jsonRpcSuccess(id, {
          tools: allTools
        })
      );
  }

  if (method === 'tools/call') {
    if (
      !params ||
      typeof params.name !== 'string'
    ) {
      return res
        .status(200)
        .json(
          jsonRpcError(
            id,
            -32602,
            'tools/call requires params.name'
          )
        );
    }

    const toolName = params.name;
    const args = params.arguments ?? {};

    if (toolName === 'control_robot') {
      const result = sendRobotControl(args);

      if (!result.ok) {
        return res
          .status(200)
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text: `机器人控制失败：${result.error}`
                }
              ],
              isError: true
            })
          );
      }

      const {
        expression,
        face_effect: faceEffect,
        motion,
        sound,
        text_to_display: textToDisplay
      } = result.payload;

      const textDescription =
        textToDisplay === undefined
          ? ''
          : `，屏幕文字=${textToDisplay || '(清除)'}`;

      return res
        .status(200)
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text:
                  '机器人已收到指令：' +
                  `表情=${expression}，` +
                  `眼睛效果=${faceEffect}，` +
                  `动作=${motion}，` +
                  `声音=${sound}` +
                  textDescription
              }
            ]
          })
        );
    }

    if (toolName === 'play_robot_audio') {
      const result = await playRobotAudio(args);

      if (!result.ok) {
        return res
          .status(200)
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text: result.error
                }
              ],
              isError: true
            })
          );
      }

      return res
        .status(200)
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text:
                  `机器人语音已确认播放：` +
                  `stream_id=${result.stream_id}，` +
                  `输入=${result.input_bytes} bytes，` +
                  `PCM=${result.pcm_bytes} bytes。`
              }
            ],
            structuredContent: result
          })
        );
    }

    if (toolName === 'stop_robot_audio') {
      stopCurrentAudioJob('requested_by_mcp');

      return res
        .status(200)
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text: '已请求机器人停止当前语音。'
              }
            ]
          })
        );
    }

    if (toolName === 'get_robot_events') {
      const events = getPendingRobotEvents();

      return res
        .status(200)
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text: formatRobotEventsText(events)
              }
            ],
            structuredContent: {
              events
            }
          })
        );
    }

    if (toolName === 'acknowledge_robot_events') {
      const eventIds = args.event_ids;

      if (!validateEventIds(eventIds)) {
        return res
          .status(200)
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text:
                    'event_ids 必须是包含 1 至 50 个短字符串 ID 的数组。'
                }
              ],
              isError: true
            })
          );
      }

      const acknowledged =
        acknowledgeRobotEvents(eventIds);

      return res
        .status(200)
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text:
                  `已确认 ${acknowledged} 条实体事件。`
              }
            ],
            structuredContent: {
              acknowledged
            }
          })
        );
    }

    return res
      .status(200)
      .json(
        jsonRpcError(
          id,
          -32601,
          `Unknown tool: ${toolName}`
        )
      );
  }

  if (isNotification) {
    return res.status(202).end();
  }

  return res
    .status(200)
    .json(
      jsonRpcError(
        id,
        -32601,
        `Method not found: ${method}`
      )
    );
});


/*
 * ============================================================
 * 兼容旧版接口
 * ============================================================
 */

app.get('/mcp', requireApiAuth, (req, res) => {
  res
    .status(405)
    .set('Allow', 'POST')
    .type('text')
    .send(
      'This MCP endpoint accepts POST JSON-RPC requests.'
    );
});


app.get('/mcp/tools', requireApiAuth, (req, res) => {
  res.json({
    tools: allTools
  });
});


app.post('/mcp/call', requireApiAuth, async (req, res) => {
  const body = req.body ?? {};

  const tool = body.tool;
  const args = body.arguments ?? {};

  if (typeof tool !== 'string') {
    return res.status(400).json({
      error: 'tool must be a string'
    });
  }

  if (tool === 'control_robot') {
    const result = sendRobotControl(args);

    if (!result.ok) {
      return res
        .status(result.status)
        .json({
          error: result.error
        });
    }

    return res.json({
      content: [
        {
          type: 'text',
          text: '已发送机器人控制指令。'
        }
      ],
      payload: result.payload
    });
  }

  if (tool === 'play_robot_audio') {
    const result = await playRobotAudio(args);

    if (!result.ok) {
      return res
        .status(result.status)
        .json({
          error: result.error
        });
    }

    return res.json({
      content: [
        {
          type: 'text',
          text:
            `机器人已确认播放音频：` +
            `stream_id=${result.stream_id}。`
        }
      ],
      stream_id: result.stream_id,
      input_bytes: result.input_bytes,
      pcm_bytes: result.pcm_bytes
    });
  }

  if (tool === 'stop_robot_audio') {
    stopCurrentAudioJob('requested_by_legacy_api');

    return res.json({
      content: [
        {
          type: 'text',
          text: '已请求机器人停止当前语音。'
        }
      ]
    });
  }

  if (tool === 'get_robot_events') {
    const events = getPendingRobotEvents();

    return res.json({
      events
    });
  }

  if (tool === 'acknowledge_robot_events') {
    const eventIds = args.event_ids;

    if (!validateEventIds(eventIds)) {
      return res.status(400).json({
        error:
          'event_ids must contain 1 to 50 short strings'
      });
    }

    return res.json({
      acknowledged:
        acknowledgeRobotEvents(eventIds)
    });
  }

  return res
    .status(404)
    .json({
      error: 'Tool not found'
    });
});


/*
 * 调试/事件接口也必须鉴权。
 */
app.get('/robot/events', requireApiAuth, (req, res) => {
  res.json({
    events: getPendingRobotEvents()
  });
});


app.post(
  '/robot/events/ack',
  requireApiAuth,
  (req, res) => {
    const eventIds = req.body?.event_ids;

    if (!validateEventIds(eventIds)) {
      return res.status(400).json({
        error:
          'event_ids must contain 1 to 50 short strings'
      });
    }

    return res.json({
      acknowledged:
        acknowledgeRobotEvents(eventIds)
    });
  }
);


/*
 * 健康检查不暴露控制信息。
 */
app.get('/', (req, res) => {
  res
    .type('text')
    .send('StackChan relay server is running.');
});


/*
 * Express 错误处理。
 */
app.use((error, req, res, next) => {
  console.error('[HTTP] Unhandled error:', error);

  if (res.headersSent) {
    return next(error);
  }

  res.status(500).json({
    error: 'Internal server error'
  });
});


server.listen(PORT, () => {
  console.log(
    `StackChan relay server is running on port ${PORT}`
  );

  if (!API_TOKEN) {
    console.error(
      '[SECURITY] API_TOKEN is not configured'
    );
  }

  if (!ROBOT_WS_TOKEN) {
    console.error(
      '[SECURITY] ROBOT_WS_TOKEN is not configured'
    );
  }

  if (AUDIO_ALLOWED_HOSTS.size === 0) {
    console.warn(
      '[SECURITY] AUDIO_ALLOWED_HOSTS is empty; ' +
      'private IP blocking is enabled, but host allow-listing ' +
      'is recommended'
    );
  }

  console.log(
    `[AUDIO] ffmpeg path: ${ffmpegPath || '(missing)'}`
  );
});
