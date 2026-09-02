import express from 'express';
import cors from 'cors';
import {
  WebSocketServer,
  WebSocket
} from 'ws';
import http from 'http';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);

const {
  playRobotAudio,
  stopRobotAudio,
  attachAudioProtocolHandlers
} = require('./audioStreamer.cjs');

const app = express();

app.use(cors());
app.use(
  express.json({
    limit: '64kb'
  })
);

const server = http.createServer(app);
const wss = new WebSocketServer({
  server
});

let robotSocket = null;
const ROBOT_ID = 'default';

const pendingRobotEvents = [];

const MAX_PENDING_EVENTS = 50;
const EVENT_TTL_MS = 24 * 60 * 60 * 1000;

function pruneExpiredRobotEvents() {
  const now = Date.now();

  for (
    let index = pendingRobotEvents.length - 1;
    index >= 0;
    index--
  ) {
    const event = pendingRobotEvents[index];
    const receivedAtMs = Date.parse(
      event.received_at
    );

    if (
      Number.isNaN(receivedAtMs) ||
      now - receivedAtMs > EVENT_TTL_MS
    ) {
      pendingRobotEvents.splice(index, 1);
    }
  }
}

function makeRobotEventId() {
  return (
    `evt_${Date.now().toString(36)}_` +
    Math.random().toString(36).slice(2, 8)
  );
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

  while (
    pendingRobotEvents.length >
    MAX_PENDING_EVENTS
  ) {
    pendingRobotEvents.shift();
  }

  console.log(
    '⬅️ 已记录机器人事件：',
    event
  );

  return event;
}

function getPendingRobotEvents() {
  pruneExpiredRobotEvents();

  const now = Date.now();

  return pendingRobotEvents.map((event) => {
    const receivedAtMs = Date.parse(
      event.received_at
    );

    return {
      ...event,
      seconds_ago: Math.max(
        0,
        Math.floor(
          (now - receivedAtMs) / 1000
        )
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
    if (
      idSet.has(
        pendingRobotEvents[index].id
      )
    ) {
      pendingRobotEvents.splice(index, 1);
      acknowledged++;
    }
  }

  return acknowledged;
}

wss.on('connection', (ws, req) => {
  console.log(
    `✅ StackChan 机器人已连接：` +
    `${req.socket.remoteAddress}`
  );

  /*
   * 目前只支持一台机器人。
   * 新连接会替换旧连接。
   */
  if (
    robotSocket &&
    robotSocket !== ws &&
    robotSocket.readyState === WebSocket.OPEN
  ) {
    void stopRobotAudio(
      robotSocket,
      ROBOT_ID,
      'replaced_by_new_connection'
    );

    try {
      robotSocket.close();
    } catch {
      // 忽略关闭异常。
    }
  }

  robotSocket = ws;

  ws.on('message', (message) => {
    const text = message.toString();

    console.log('机器人上报：', text);

    let data;

    try {
      data = JSON.parse(text);
    } catch {
      /*
       * 非 JSON 调试内容只记录，不写入事件队列。
       */
      return;
    }

    /*
     * 音频协议消息不进入实体事件队列。
     */
   if (
  data &&
  typeof data.type === 'string' &&
  data.type.startsWith('audio_')
) {
  console.log(
    '[audio] robot protocol message:',
    JSON.stringify(data)
  );

  attachAudioProtocolHandlers(
    ROBOT_ID,
    data
  );

  return;
}


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

    const isShakeEvent =
      data &&
      data.type === 'robot_event' &&
      data.event === 'shake' &&
      Number.isFinite(data.at_ms);

    if (
      isScreenTouchEvent ||
      isHeadTouchEvent ||
      isShakeEvent
    ) {
      addRobotEvent(data);
    }
  });

  ws.on('close', () => {
    if (robotSocket === ws) {
      robotSocket = null;
    }

    void stopRobotAudio(
      ws,
      ROBOT_ID,
      'robot_disconnected'
    );

    console.log(
      '❌ StackChan 机器人已断开'
    );
  });

  ws.on('error', (error) => {
    console.error(
      '机器人 WebSocket 错误：',
      error.message
    );
  });
});

function jsonRpcSuccess(id, result) {
  return {
    jsonrpc: '2.0',
    id,
    result
  };
}

function jsonRpcError(
  id,
  code,
  message,
  data
) {
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

function sanitizeDisplayText(value) {
  if (typeof value !== 'string') {
    return '';
  }

  return value
    .replace(
      /[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]/g,
      ''
    )
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

const controlRobotTool = {
  name: 'control_robot',

  description:
    '控制 StackChan 实体身体的当前状态与一次性互动反馈。可设置基础表情、可选脸部效果、简短屏幕文字、已验证的保守舵机动作和短提示音。每次调用都会更新 expression 与 face_effect；未传或无效的 face_effect 会恢复为 none。当前主动动作支持 nod、shake_head、look_left、look_right、tilt_up、home 和 none。shake 是用户摇晃实体后由 IMU 上报的事件，不是机器人主动动作，不能通过本工具触发。不支持任意角度、自定义动作序列、跳舞、TTS、录音或自定义灯光命令。',

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
        ],
        description:
          '必填。基础图形表情，同时决定实体灯光的基础主题色和呼吸节奏。'
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
        ],
        description:
          '可选的强化脸部效果。'
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
        ],
        description:
          '可选的一次性舵机动作。'
      },

      text_to_display: {
        type: 'string',
        maxLength: 80,
        description:
          '可选。显示在实体屏幕上的文字，最多 80 个字符。'
      },

      display_duration_ms: {
        type: 'integer',
        minimum: 1000,
        maximum: 10000,
        description:
          '屏幕文字的显示时长，单位为毫秒。'
      },

      text_to_speak: {
        type: 'string',
        description:
          '为未来 TTS 预留，目前会被安全忽略。'
      },

      sound: {
        type: 'string',
        enum: [
          'none',
          'message',
          'emotion'
        ],
        description:
          '播放 SD 卡中已验证的短 WAV 提示音。'
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
    '读取 StackChan 实体身体尚未处理的近期互动事件。当前支持 touch_tap、head_touch 和 shake。读取不会删除事件；处理完成后应调用 acknowledge_robot_events 确认。',

  inputSchema: {
    type: 'object',
    properties: {}
  }
};

const acknowledgeRobotEventsTool = {
  name: 'acknowledge_robot_events',

  description:
    '确认角色已处理的 StackChan 实体事件。确认后事件会从待处理队列删除。',

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
          '要确认并删除的事件 ID 列表。'
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
    '播放指定 HTTP 或 HTTPS 音频 URL。服务端会自动使用 ffmpeg 转换为 16kHz、单声道、16-bit PCM，并通过 WebSocket 流式发送给 StackChan。新的播放请求会自动打断当前音频。',

  inputSchema: {
    type: 'object',

    properties: {
      audio_url: {
        type: 'string',
        format: 'uri',
        description:
          '服务端可以访问的 HTTP 或 HTTPS 音频地址。'
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
    '立即停止 StackChan 当前正在接收或播放的网络音频。',

  inputSchema: {
    type: 'object',
    properties: {}
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

  const allowedFaceEffects = [
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
  ];

  const allowedMotions = [
    'nod',
    'shake_head',
    'look_left',
    'look_right',
    'tilt_up',
    'home',
    'none'
  ];

  const allowedSounds = [
    'none',
    'message',
    'emotion'
  ];

  const normalized = {
    expression: allowedExpressions.includes(
      args.expression
    )
      ? args.expression
      : 'neutral',

    face_effect: allowedFaceEffects.includes(
      args.face_effect
    )
      ? args.face_effect
      : 'none',

    motion: allowedMotions.includes(
      args.motion
    )
      ? args.motion
      : 'none',

    sound: allowedSounds.includes(
      args.sound
    )
      ? args.sound
      : 'none'
  };

  if (
    typeof args.text_to_display === 'string'
  ) {
    normalized.text_to_display =
      sanitizeDisplayText(
        args.text_to_display
      );

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

  robotSocket.send(
    JSON.stringify(payload)
  );

  console.log(
    '➡️ 已发送至机器人：',
    payload
  );

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
        `- 用户在 ${event.seconds_ago} 秒前` +
        `触摸了 StackChan 屏幕一次` +
        `（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'head_touch') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前` +
        `摸了 StackChan 的头顶一次` +
        `（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'shake') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前` +
        `摇晃了 StackChan 一次` +
        `（事件 ID：${event.id}）。`
      );
    }

    return (
      `- 未知实体事件：${event.event}` +
      `（事件 ID：${event.id}）。`
    );
  });

  return (
    '尚未处理的实体事件：\n' +
    lines.join('\n')
  );
}

function isValidHttpUrl(value) {
  if (
    typeof value !== 'string' ||
    value.length === 0 ||
    value.length > 2048
  ) {
    return false;
  }

  try {
    const parsed = new URL(value);

    return (
      parsed.protocol === 'http:' ||
      parsed.protocol === 'https:'
    );
  } catch {
    return false;
  }
}

function robotOfflineResult(id) {
  return resJsonRpcToolError(
    id,
    '机器人离线，无法执行音频操作。'
  );
}

function resJsonRpcToolError(id, text) {
  return jsonRpcSuccess(id, {
    content: [
      {
        type: 'text',
        text
      }
    ],
    isError: true
  });
}

app.post('/mcp', async (req, res) => {
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

  console.log(
    `MCP request: ${method}`
  );

  const isNotification =
    id === undefined ||
    id === null;

  if (method === 'initialize') {
    const result = {
      protocolVersion:
        params.protocolVersion ||
        '2025-03-26',

      capabilities: {
        tools: {}
      },

      serverInfo: {
        name: 'stackchan-robot-relay',
        version: '1.6.0'
      },

      instructions:
        '此服务负责 StackChan 实体身体的上下行中继。支持基础表情、脸部效果、短屏幕文字、保守舵机动作、短提示音、网络音频播放和网络音频停止；也可读取并确认 touch_tap、head_touch、shake 实体事件。网络音频使用 play_robot_audio 播放，使用 stop_robot_audio 停止。shake 表示用户摇晃机器人后由 IMU 上报，并非机器人主动舵机动作。tilt_down 不支持。'
    };

    return res
      .status(200)
      .type('application/json')
      .json(
        jsonRpcSuccess(id, result)
      );
  }

  if (
    method === 'notifications/initialized'
  ) {
    return res
      .status(202)
      .end();
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
            acknowledgeRobotEventsTool,
            playRobotAudioTool,
            stopRobotAudioTool
          ]
        })
      );
  }

  if (method === 'tools/call') {
    const toolName = params.name;
    const args = params.arguments ?? {};

    if (
      toolName === 'play_robot_audio' ||
      toolName === 'stop_robot_audio'
    ) {
      if (
        !robotSocket ||
        robotSocket.readyState !== WebSocket.OPEN
      ) {
        return res
          .status(200)
          .type('application/json')
          .json(
            resJsonRpcToolError(
              id,
              '机器人离线，无法执行音频操作。'
            )
          );
      }

      if (toolName === 'play_robot_audio') {
        const audioUrl = args.audio_url;

        if (!isValidHttpUrl(audioUrl)) {
          return res
            .status(200)
            .type('application/json')
            .json(
              resJsonRpcToolError(
                id,
                'audio_url 必须是有效的 HTTP 或 HTTPS URL。'
              )
            );
        }

        try {
          await playRobotAudio(
            robotSocket,
            ROBOT_ID,
            audioUrl
          );

          return res
            .status(200)
            .type('application/json')
            .json(
              jsonRpcSuccess(id, {
                content: [
                  {
                    type: 'text',
                    text: '音频已发送完成。'
                  }
                ]
              })
            );
        } catch (error) {
          console.error(
            '播放机器人音频失败：',
            error
          );

          return res
            .status(200)
            .type('application/json')
            .json(
              resJsonRpcToolError(
                id,
                `音频播放失败：${error.message}`
              )
            );
        }
      }

      try {
        await stopRobotAudio(
          robotSocket,
          ROBOT_ID,
          'stopped_by_tool_call'
        );

        return res
          .status(200)
          .type('application/json')
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text: '已停止机器人音频。'
                }
              ]
            })
          );
      } catch (error) {
        return res
          .status(200)
          .type('application/json')
          .json(
            resJsonRpcToolError(
              id,
              `停止音频失败：${error.message}`
            )
          );
      }
    }

    if (toolName === 'control_robot') {
      const result = sendRobotControl(args);

      if (!result.ok) {
        return res
          .status(200)
          .type('application/json')
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text:
                    `机器人离线：${result.error}`
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
        .type('application/json')
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

    if (toolName === 'get_robot_events') {
      const events = getPendingRobotEvents();

      return res
        .status(200)
        .type('application/json')
        .json(
          jsonRpcSuccess(id, {
            content: [
              {
                type: 'text',
                text: formatRobotEventsText(
                  events
                )
              }
            ],
            structuredContent: {
              events
            }
          })
        );
    }

    if (
      toolName === 'acknowledge_robot_events'
    ) {
      const eventIds =
        args.event_ids;

      if (
        !Array.isArray(eventIds) ||
        eventIds.length === 0 ||
        eventIds.length > 50 ||
        !eventIds.every(
          (item) => typeof item === 'string'
        )
      ) {
        return res
          .status(200)
          .type('application/json')
          .json(
            jsonRpcSuccess(id, {
              content: [
                {
                  type: 'text',
                  text:
                    'event_ids 必须是包含 1 至 50 个事件 ID 的字符串数组。'
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
        .type('application/json')
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
      .type('application/json')
      .json(
        jsonRpcError(
          id,
          -32601,
          `Unknown tool: ${toolName || '(missing)'}`
        )
      );
  }

  if (isNotification) {
    return res
      .status(202)
      .end();
  }

  return res
    .status(200)
    .type('application/json')
    .json(
      jsonRpcError(
        id,
        -32601,
        `Method not found: ${method}`
      )
    );
});

app.get('/mcp', (req, res) => {
  res
    .status(405)
    .set('Allow', 'POST')
    .type('text')
    .send(
      'This MCP endpoint accepts POST JSON-RPC requests.'
    );
});

app.get('/', (req, res) => {
  res
    .type('text')
    .send(
      'StackChan relay server is running.'
    );
});

app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [
      controlRobotTool,
      getRobotEventsTool,
      acknowledgeRobotEventsTool,
      playRobotAudioTool,
      stopRobotAudioTool
    ]
  });
});

app.post('/mcp/call', async (req, res) => {
  const {
    tool,
    arguments: args = {}
  } = req.body ?? {};

  if (
    tool !== 'control_robot' &&
    tool !== 'play_robot_audio' &&
    tool !== 'stop_robot_audio'
  ) {
    return res
      .status(404)
      .json({
        error: 'Tool not found'
      });
  }

  if (tool === 'play_robot_audio') {
    if (
      !robotSocket ||
      robotSocket.readyState !== WebSocket.OPEN
    ) {
      return res
        .status(503)
        .json({
          error:
            'Robot is offline. Please check Wi-Fi and WebSocket connection.'
        });
    }

    if (!isValidHttpUrl(args.audio_url)) {
      return res
        .status(400)
        .json({
          error:
            'audio_url must be a valid HTTP or HTTPS URL'
        });
    }

    try {
      await playRobotAudio(
        robotSocket,
        ROBOT_ID,
        args.audio_url
      );

      return res.json({
        content: [
          {
            type: 'text',
            text: '音频已发送完成。'
          }
        ]
      });
    } catch (error) {
      console.error(
        '旧版接口播放音频失败：',
        error
      );

      return res
        .status(500)
        .json({
          error: error.message
        });
    }
  }

  if (tool === 'stop_robot_audio') {
    if (
      !robotSocket ||
      robotSocket.readyState !== WebSocket.OPEN
    ) {
      return res
        .status(503)
        .json({
          error:
            'Robot is offline. Please check Wi-Fi and WebSocket connection.'
        });
    }

    await stopRobotAudio(
      robotSocket,
      ROBOT_ID,
      'stopped_by_legacy_tool_call'
    );

    return res.json({
      content: [
        {
          type: 'text',
          text: '已停止机器人音频。'
        }
      ]
    });
  }

  const result = sendRobotControl(args);

  if (!result.ok) {
    return res
      .status(result.status)
      .json({
        error: result.error
      });
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
      : `, text_to_display=${textToDisplay || '(clear)'}`;

  return res.json({
    content: [
      {
        type: 'text',
        text:
          `已发送机器人指令：expression=${expression}, ` +
          `face_effect=${faceEffect}, ` +
          `motion=${motion}, ` +
          `sound=${sound}` +
          textDescription
      }
    ]
  });
});

app.get('/robot/events', (req, res) => {
  res.json({
    events: getPendingRobotEvents()
  });
});

app.post('/robot/events/ack', (req, res) => {
  const eventIds = req.body?.event_ids;

  if (
    !Array.isArray(eventIds) ||
    !eventIds.every(
      (item) => typeof item === 'string'
    )
  ) {
    return res
      .status(400)
      .json({
        error:
          'event_ids must be an array of strings'
      });
  }

  const acknowledged =
    acknowledgeRobotEvents(eventIds);

  return res.json({
    acknowledged
  });
});

const PORT = process.env.PORT || 3000;

server.listen(PORT, () => {
  console.log(
    `Server is running on port ${PORT}`
  );
});
