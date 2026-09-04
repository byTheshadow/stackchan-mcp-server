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

/*
 * CORS 配置。
 *
 * GitHub Pages 的实际 origin 是：
 * https://bytheshadow.github.io
 *
 * 不要把 /WHEN-I-with-U/ 写入 origin。
 */
const allowedOrigins = [
  'https://bytheshadow.github.io',
  'http://localhost:5173',
  'http://localhost:4173'
];

app.use(
  cors({
    origin(origin, callback) {
      /*
       * 没有 origin 的请求通常来自 curl、Render 健康检查或服务器间请求。
       */
      if (!origin || allowedOrigins.includes(origin)) {
        return callback(null, true);
      }

      return callback(
        new Error(`CORS origin not allowed: ${origin}`)
      );
    },

    methods: [
      'GET',
      'POST',
      'OPTIONS'
    ],

    allowedHeaders: [
      'Content-Type',
      'Authorization',
      'Accept',
      'MCP-Protocol-Version'
    ],

    exposedHeaders: [
      'Mcp-Session-Id',
      'Content-Type'
    ],

    credentials: false
  })
);

app.use(
  express.json({
    limit: '64kb'
  })
);

const server = http.createServer(app);

const wss = new WebSocketServer({
  server,
  perMessageDeflate: false
});

let robotSocket = null;

const ROBOT_ID = 'default';

const pendingRobotEvents = [];

const MAX_PENDING_EVENTS = 50;

const EVENT_TTL_MS =
  24 * 60 * 60 * 1000;


/* =========================================================
 * 机器人事件
 * ======================================================= */

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
    '已记录机器人事件：',
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


/* =========================================================
 * WebSocket：StackChan 机器人连接
 * ======================================================= */

wss.on('connection', (ws, req) => {
  console.log(
    'StackChan 机器人已连接：',
    req.socket.remoteAddress
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

    console.log(
      '机器人上报：',
      text
    );

    let data;

    try {
      data = JSON.parse(text);
    } catch {
      /*
       * 非 JSON 调试内容只记录，
       * 不写入事件队列。
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
      'StackChan 机器人已断开'
    );
  });

  ws.on('error', (error) => {
    console.error(
      '机器人 WebSocket 错误：',
      error.message
    );
  });
});


/* =========================================================
 * JSON-RPC 基础函数
 * ======================================================= */

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


/* =========================================================
 * 机器人参数处理
 * ======================================================= */

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

function normalizeRobotArguments(args = {}) {
  const normalized = {};

  if (
    args.expression &&
    allowedExpressions.includes(args.expression)
  ) {
    normalized.expression = args.expression;
  }

  if (
    args.face_effect &&
    allowedFaceEffects.includes(args.face_effect)
  ) {
    normalized.face_effect = args.face_effect;
  }

  if (
    args.motion &&
    allowedMotions.includes(args.motion)
  ) {
    normalized.motion = args.motion;
  }

  if (
    args.sound &&
    allowedSounds.includes(args.sound)
  ) {
    normalized.sound = args.sound;
  }

  if (typeof args.volume === 'number') {
    normalized.volume = Math.max(
      0,
      Math.min(
        255,
        Math.floor(args.volume)
      )
    );
  }

  const rawText =
    args.text_to_display ??
    args.text ??
    args.message ??
    args.content;

  if (
    typeof rawText === 'string' &&
    rawText.length > 0
  ) {
    normalized.text_to_display =
      sanitizeDisplayText(rawText);

    normalized.display_duration_ms =
      normalizeDisplayDuration(
        args.display_duration_ms ??
        args.duration_ms
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
    '已发送至机器人：',
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
        '触摸了 StackChan 屏幕一次' +
        `（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'head_touch') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前` +
        '摸了 StackChan 的头顶一次' +
        `（事件 ID：${event.id}）。`
      );
    }

    if (event.event === 'shake') {
      return (
        `- 用户在 ${event.seconds_ago} 秒前` +
        '摇晃了 StackChan 一次' +
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


/* =========================================================
 * MCP 工具定义
 *
 * 这里是当前版本缺失、导致 ReferenceError 的部分。
 * ======================================================= */

const controlRobotTool = {
  name: 'control_robot',

  
   description:
  '控制你的 StackChan 实体身体。可以根据当前对话、情绪和互动语境，自主选择基础图形表情、自定义脸部效果、简短英文文字和 ASCII 颜文字，并执行已经过实体验证的动作。支持基础表情、扩展脸部效果、shake_head、look_left、look_right、tilt_up、nod 和 home。不要使用未提供的动作、表情、自定义效果或声音。'


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
          '实体身体的基础图形表情。happy=开心、友好、感谢；sad=难过、遗憾、安慰；angry=不满、认真或强调；doubt=疑惑、思考、不确定；sleepy=困倦、晚安或休息；neutral=平静、默认状态。只能使用提供的枚举值。'
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
          '可选的自定义脸部效果。none 使用原生眼睛、嘴巴和眉毛；heart_eyes 为爱心眼；sparkle_eyes 为闪光眼；dizzy_eyes 为眩晕眼；tear_eyes 为泪眼；surprised_face 为惊讶大圆眼和 O 嘴；pout_face 为嘟嘴；shy_face 为害羞小眼、微笑嘴和腮红；smug_face 为得意歪嘴和挑眉；confused_face 为一大一小的困惑眼和挑眉；laugh_face 为弯月笑眼和张嘴大笑；kiss_face 为原生眼与圆形嘟嘴；nervous_face 为小圆眼、波浪嘴与高低眉；relieved_face 为放松闭眼和宽笑嘴；determined_face 为压缩眼、压低眉和坚定平嘴。通常使用 none，只有语境明显需要时才使用自定义效果。'
      },

      motion: {
        type: 'string',

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
    '机器人动作：nod 为点头一次且自动回中；shake_head 为摇头；look_left 为看向左侧；look_right 为看向右侧；tilt_up 为抬头；home 为立即回到正中；none 为不执行动作。只能使用提供的枚举值。'
},

      },

      text_to_display: {
        type: 'string',
        maxLength: 80,

        description:
          '可选。显示在实体身体屏幕上的简短文字或 ASCII 颜文字。最多 80 个字符。'
      },

      display_duration_ms: {
        type: 'integer',
        minimum: 1000,
        maximum: 10000,

        description:
          '可选。文字在实体身体屏幕上停留的时间，单位为毫秒。范围为 1000 至 10000，默认 5000。'
      },

      text_to_speak: {
        type: 'string',

        description:
          '保留给未来 TTS 使用。目前机器人 TTS 尚未接入，此字段会安全忽略；请使用 text_to_display 显示屏幕文字。'
      },

      sound: {
        type: 'string',

        enum: [
          'none',
          'message',
          'emotion'
        ],

        description:
          '可选。播放 SD 卡中的短 WAV 提示音：message 为收到新消息提示；emotion 为情绪回应提示；none 为不播放。'
      },

      volume: {
        type: 'integer',
        minimum: 0,
        maximum: 255,

        description:
          '可选。机器人音量，范围为 0 至 255。'
      }
    },

    required: [
      'expression'
    ],

    additionalProperties: false
  }
};

const getRobotEventsTool = {
  name: 'get_robot_events',

  description:
    '读取 StackChan 实体身体尚未处理的近期感应事件。当前支持 touch_tap、head_touch 和 shake。每条事件提供 received_at 和 seconds_ago。读取不会自动删除事件；角色处理后应调用 acknowledge_robot_events，避免未来重复提及。',

  inputSchema: {
    type: 'object',
    properties: {},
    additionalProperties: false
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

    required: [
      'event_ids'
    ],

    additionalProperties: false
  }
};

const playRobotAudioTool = {
  name: 'play_robot_audio',

  description:
    '让 StackChan 播放一个来自 HTTP 或 HTTPS URL 的网络音频。音频地址必须是有效的 HTTP 或 HTTPS URL。',

  inputSchema: {
    type: 'object',

    properties: {
      audio_url: {
        type: 'string',
        format: 'uri',

        description:
          '要播放的音频文件地址，必须是 HTTP 或 HTTPS URL。'
      }
    },

    required: [
      'audio_url'
    ],

    additionalProperties: false
  }
};

const stopRobotAudioTool = {
  name: 'stop_robot_audio',

  description:
    '停止 StackChan 当前正在播放的网络音频。',

  inputSchema: {
    type: 'object',

    properties: {},

    additionalProperties: false
  }
};


/* =========================================================
 * MCP HTTP 接口
 * ======================================================= */

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

      if (
        toolName === 'play_robot_audio'
      ) {
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
                text: formatRobotEventsText(events)
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
      const eventIds = args.event_ids;

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


/* =========================================================
 * GET /mcp
 * ======================================================= */

app.get('/mcp', (req, res) => {
  res
    .status(405)
    .set('Allow', 'POST')
    .type('text')
    .send(
      'This MCP endpoint accepts POST JSON-RPC requests.'
    );
});


/* =========================================================
 * 健康检查
 * ======================================================= */

app.get('/', (req, res) => {
  res
    .type('text')
    .send(
      'StackChan relay server is running.'
    );
});


/* =========================================================
 * 旧版工具说明接口
 * ======================================================= */

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


/* =========================================================
 * 旧版自定义控制接口
 * ======================================================= */

app.post('/mcp/call', async (req, res) => {
  const {
    tool,
    arguments: args = {}
  } = req.body ?? {};

  console.log(
    `[mcp-call] ${new Date().toISOString()} ` +
    `tool=${tool} ` +
    `audio_url=${args?.audio_url || ''}`
  );

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

    try {
      await stopRobotAudio(
        robotSocket,
        ROBOT_ID,
        'stopped_by_legacy_tool_call'
      );
    } catch (error) {
      console.error(
        '旧版接口停止音频失败：',
        error
      );

      return res
        .status(500)
        .json({
          error: error.message
        });
    }

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


/* =========================================================
 * 机器人事件旧接口
 * ======================================================= */

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


/* =========================================================
 * 错误处理
 * ======================================================= */

app.use((error, req, res, next) => {
  console.error(
    'Express error:',
    error
  );

  if (res.headersSent) {
    return next(error);
  }

  return res
    .status(500)
    .json({
      error: error.message || 'Internal server error'
    });
});


/* =========================================================
 * 启动服务
 * ======================================================= */

const PORT = process.env.PORT || 3000;

server.listen(
  PORT,
  '0.0.0.0',
  () => {
    console.log(
      `Server is running on port ${PORT}`
    );
  }
);

