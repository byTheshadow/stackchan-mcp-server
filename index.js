import express from 'express';
import cors from 'cors';
import { WebSocketServer, WebSocket } from 'ws';
import http from 'http';

const app = express();

app.use(cors());
app.use(express.json({ limit: '64kb' }));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// 当前在线的机器人连接。
// 目前仅支持一台机器人；未来可用 robot_id 扩展多台。
let robotSocket = null;

/*
 * 实体身体上报的待处理事件。
 *
 * 这是短期中继队列，不是角色记忆库：
 * - 最多 50 条；
 * - 24 小时后过期；
 * - Render 重启后会清空；
 * - 角色 A 处理完成后应调用 acknowledge_robot_events 确认。
 */
const pendingRobotEvents = [];
const MAX_PENDING_EVENTS = 50;
const EVENT_TTL_MS = 24 * 60 * 60 * 1000;

function pruneExpiredRobotEvents() {
  const now = Date.now();

  for (let index = pendingRobotEvents.length - 1; index >= 0; index--) {
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

function makeRobotEventId() {
  return `evt_${Date.now().toString(36)}_${Math.random()
    .toString(36)
    .slice(2, 8)}`;
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

  console.log('⬅️ 已记录机器人事件：', event);

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

  for (let index = pendingRobotEvents.length - 1; index >= 0; index--) {
    if (idSet.has(pendingRobotEvents[index].id)) {
      pendingRobotEvents.splice(index, 1);
      acknowledged++;
    }
  }

  return acknowledged;
}

wss.on('connection', (ws, req) => {
  console.log(`✅ StackChan 机器人已连接：${req.socket.remoteAddress}`);
  robotSocket = ws;

  ws.on('message', (message) => {
    const text = message.toString();

    console.log('机器人上报：', text);

    let data;

    try {
      data = JSON.parse(text);
    } catch {
      // 机器人偶尔发送的非 JSON 调试内容仅记录日志，不作为事件处理。
      return;
    }

    /*
     * 第一版只接受 touch_tap。
     * 不能让机器人任意上报内容写入事件队列。
     */
    const isScreenTouchEvent =
  data &&
  data.type === 'robot_event' &&
  data.event === 'touch_tap' &&
  Number.isInteger(data.x) &&
  Number.isInteger(data.y) &&
  Number.isFinite(data.at_ms);

const isHeadTouchEvent =
  data &&
  data.type === 'robot_event' &&
  data.event === 'head_touch' &&
  Number.isFinite(data.at_ms);

if (isScreenTouchEvent || isHeadTouchEvent) {
  addRobotEvent(data);
}

  });

  ws.on('close', () => {
    if (robotSocket === ws) {
      robotSocket = null;
    }
    console.log('❌ StackChan 机器人已断开');
  });

  ws.on('error', (error) => {
    console.error('机器人 WebSocket 错误：', error.message);
  });
});

function jsonRpcSuccess(id, result) {
  return {
    jsonrpc: '2.0',
    id,
    result
  };
}

function jsonRpcError(id, code, message, data) {
  const error = { code, message };

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
 * 第一轮屏幕文字仅允许可打印 ASCII：
 * - 英文字母、数字、英文标点
 * - ASCII 颜文字，例如 ^_^、:)、T_T、o_O
 */
function sanitizeDisplayText(value) {
  if (typeof value !== 'string') {
    return '';
  }

  let result = '';

  for (const char of value) {
    const code = char.charCodeAt(0);

    if (code >= 32 && code <= 126) {
      result += char;
    }

    if (result.length >= 80) {
      break;
    }
  }

  return result;
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

const controlRobotTool = {
  name: 'control_robot',
 description:
  '控制你的 StackChan 实体身体。你可以根据当前对话、情绪和互动语境，自主选择合适的图形表情、简短英文文字和 ASCII 颜文字，并执行已经过实体验证的 nod（点头）或 home（回到正中）。不要使用未提供的动作、表情或声音。',
  inputSchema: {
    type: 'object',
    properties: {
      expression: {
        type: 'string',
        enum: ['happy', 'sad', 'angry', 'doubt', 'sleepy', 'neutral'],
        description:
  '你的实体身体的图形表情。根据语境自行选择：happy=开心、友好、感谢；sad=难过、遗憾、安慰；angry=不满、认真或强调；doubt=疑惑、思考、不确定；sleepy=困倦、晚安或休息；neutral=平静、默认状态。只能使用提供的枚举值。'
      },
      motion: {
        type: 'string',
        enum: ['nod', 'home', 'none'],
        description:
          '机器人动作：nod 为点头一次且自动回中；home 为立即回到正中；none 为不执行动作。'
      },
      text_to_display: {
        type: 'string',
        maxLength: 80,
        description:
  '可选。显示在你的实体身体屏幕上的简短英文文字或 ASCII 颜文字。你应根据对话和情绪自行选择自然、简短的内容，而不是固定使用同一句话。仅支持可打印 ASCII：英文字母、数字、半角英文标点、空格和 ASCII 颜文字；不要使用中文、emoji、Unicode 颜文字、全角字符或换行。最多 80 个字符。推荐优先使用 1 至 8 个英文单词，必要时搭配一个 ASCII 颜文字。示例： "^_^ Hi!"、"I am here :)"、"Yay! \\o/"、"Hmm... o_O"、"Thank you! <3"、"Sorry... T_T"、"Good night... z_z"。'

      },
      display_duration_ms: {
        type: 'integer',
        minimum: 1000,
        maximum: 10000,
        description:
  '可选。文字在你的实体身体屏幕上停留的时间，单位毫秒；范围为 1000 至 10000，默认 5000。极短颜文字通常用 3000 至 5000；含英文短句通常用 4000 至 7000。'

      },
      text_to_speak: {
        type: 'string',
        description:
          '保留给未来 TTS 使用。目前机器人 TTS 尚未接入，此字段会安全忽略；请使用 text_to_display 显示屏幕文字。'
      },
      sound: {
  type: 'string',
  enum: ['none', 'message', 'emotion'],
  description:
    '可选。播放 SD 卡中的短 WAV 提示音：message 为收到新消息提示；emotion 为情绪回应提示；none 为不播放。'
}

    },
    required: ['expression']
  }
};

const getRobotEventsTool = {
  name: 'get_robot_events',
  description:
    '读取 StackChan 实体身体尚未处理的近期感应事件。当前仅支持 touch_tap，表示用户触摸了一次屏幕。每条事件提供 received_at 和 seconds_ago。读取不会自动删除事件；角色处理后应调用 acknowledge_robot_events，避免未来重复提及。',
  inputSchema: {
    type: 'object',
    properties: {}
  }
};

const acknowledgeRobotEventsTool = {
  name: 'acknowledge_robot_events',
  description:
    '确认角色已处理的 StackChan 实体事件。确认后事件会从待处理队列删除，不会再由 get_robot_events 返回。',
  inputSchema: {
    type: 'object',
    properties: {
      event_ids: {
        type: 'array',
        items: {
          type: 'string'
        },
        minItems: 1,
        maxItems: 50,
        description:
          '要确认并删除的事件 ID 列表；ID 来自 get_robot_events。'
      }
    },
    required: ['event_ids']
  }
};

function normalizeRobotArguments(args = {}) {
  const allowedExpressions = [
    'happy',
    'sad',
    'angry',
    'doubt',
    'sleepy',
    'neutral'
  ];

  const allowedMotions = ['nod', 'home', 'none'];
  const allowedSounds = ['none', 'message', 'emotion'];


  const normalized = {
    expression: allowedExpressions.includes(args.expression)
      ? args.expression
      : 'neutral',

    motion: allowedMotions.includes(args.motion)
      ? args.motion
      : 'none',

      sound: allowedSounds.includes(args.sound)
  ? args.sound
  : 'none'

  };

  /*
   * 只有调用方明确传入 text_to_display 时才发送文字字段，
   * 以保证旧调用不会清除当前文字。
   */
  if (typeof args.text_to_display === 'string') {
    normalized.text_to_display = sanitizeDisplayText(args.text_to_display);
    normalized.display_duration_ms = normalizeDisplayDuration(
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

  if (!robotSocket || robotSocket.readyState !== WebSocket.OPEN) {
    return {
      ok: false,
      status: 503,
      error: 'Robot is offline. Please check Wi-Fi and WebSocket connection.'
    };
  }

  robotSocket.send(JSON.stringify(payload));
  console.log('➡️ 已发送至机器人：', payload);

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
  return `- 用户在 ${event.seconds_ago} 秒前触摸了 StackChan 屏幕一次（事件 ID：${event.id}）。`;
}

if (event.event === 'head_touch') {
  return `- 用户在 ${event.seconds_ago} 秒前摸了 StackChan 的头顶一次（事件 ID：${event.id}）。`;
}

return `- 未知实体事件：${event.event}（事件 ID：${event.id}）。`;

  });

  return `尚未处理的实体事件：\n${lines.join('\n')}`;
}

/*
 * 标准 MCP Streamable HTTP 入口。
 * PWA 地址：
 * https://stackchan-mcp-server.onrender.com/mcp
 */
app.post('/mcp', (req, res) => {
  const request = req.body;

  if (
    !request ||
    request.jsonrpc !== '2.0' ||
    typeof request.method !== 'string'
  ) {
    return res.status(400).json(
      jsonRpcError(null, -32600, 'Invalid JSON-RPC request')
    );
  }

  const { id, method, params = {} } = request;

  console.log(`MCP request: ${method}`);

  const isNotification = id === undefined || id === null;

  if (method === 'initialize') {
    const result = {
      protocolVersion: params.protocolVersion || '2025-03-26',
      capabilities: {
        tools: {}
      },
      serverInfo: {
        name: 'stackchan-robot-relay',
        version: '1.2.0'
      },
      instructions:
        '此服务负责 StackChan 实体身体的上下行中继。可控制表情、短文字、nod、home；也可读取和确认近期实体触摸事件。'
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
    return res
      .status(200)
      .type('application/json')
      .json(
        jsonRpcSuccess(id, {
          tools: [
            controlRobotTool,
            getRobotEventsTool,
            acknowledgeRobotEventsTool
          ]
        })
      );
  }

  if (method === 'tools/call') {
    if (params.name === 'control_robot') {
      const result = sendRobotControl(params.arguments ?? {});

      if (!result.ok) {
        return res
          .status(200)
          .type('application/json')
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text: `机器人离线：${result.error}`
                }
              ],
              isError: true
            })
          );
      }

      const {
  expression,
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
        .type('application/json')
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
               text: `机器人已收到指令：表情=${expression}，动作=${motion}，声音=${sound}${textDescription}`
              }
            ]
          })
        );
    }

    if (params.name === 'get_robot_events') {
      const events = getPendingRobotEvents();

      return res
        .status(200)
        .type('application/json')
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

    if (params.name === 'acknowledge_robot_events') {
      const eventIds = params.arguments?.event_ids;

      if (
        !Array.isArray(eventIds) ||
        eventIds.length === 0 ||
        eventIds.length > 50 ||
        !eventIds.every((item) => typeof item === 'string')
      ) {
        return res
          .status(200)
          .type('application/json')
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text: 'event_ids 必须是包含 1 至 50 个事件 ID 的字符串数组。'
                }
              ],
              isError: true
            })
          );
      }

      const acknowledged = acknowledgeRobotEvents(eventIds);

      return res
        .status(200)
        .type('application/json')
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text: `已确认 ${acknowledged} 条实体事件。`
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
      .type('application/json')
      .json(
        jsonRpcError(
          id,
          -32601,
          `Unknown tool: ${params.name || '(missing)'}`
        )
      );
  }

  if (isNotification) {
    return res.status(202).end();
  }

  return res
    .status(200)
    .type('application/json')
    .json(jsonRpcError(id, -32601, `Method not found: ${method}`));
});

app.get('/mcp', (req, res) => {
  res
    .status(405)
    .set('Allow', 'POST')
    .type('text')
    .send('This MCP endpoint accepts POST JSON-RPC requests.');
});

// 健康检查
app.get('/', (req, res) => {
  res.type('text').send('StackChan relay server is running.');
});

// 旧工具说明接口
app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [
      controlRobotTool,
      getRobotEventsTool,
      acknowledgeRobotEventsTool
    ]
  });
});

// 旧自定义控制接口，继续保留。
app.post('/mcp/call', (req, res) => {
  const { tool, arguments: args = {} } = req.body ?? {};

  if (tool !== 'control_robot') {
    return res.status(404).json({ error: 'Tool not found' });
  }

  const result = sendRobotControl(args);

  if (!result.ok) {
    return res.status(result.status).json({
      error: result.error
    });
  }

  const {
    expression,
    motion,
    sound,
    text_to_display: textToDisplay
  } = result.payload;

  const textDescription =
    textToDisplay === undefined
      ? ''
      : `, text_to_display=${textToDisplay || '(clear)'}`;

  return res.json({
    content: [
      {
        type: 'text',
        text:
          `已发送机器人指令：expression=${expression}, ` +
          `motion=${motion}, sound=${sound}` +
          textDescription
      }
    ]
  });
});


/*
 * 仅供当前浏览器控制台手工测试的临时事件接口。
 * PWA 正式集成时请使用 MCP 工具，不需要依赖这两个 HTTP 路由。
 */
app.get('/robot/events', (req, res) => {
  res.json({
    events: getPendingRobotEvents()
  });
});

app.post('/robot/events/ack', (req, res) => {
  const eventIds = req.body?.event_ids;

  if (
    !Array.isArray(eventIds) ||
    !eventIds.every((item) => typeof item === 'string')
  ) {
    return res.status(400).json({
      error: 'event_ids must be an array of strings'
    });
  }

  const acknowledged = acknowledgeRobotEvents(eventIds);

  return res.json({
    acknowledged
  });
});

const PORT = process.env.PORT || 3000;

server.listen(PORT, () => {
  console.log(`Server is running on port ${PORT}`);
});
