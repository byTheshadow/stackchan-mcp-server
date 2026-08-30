
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

wss.on('connection', (ws, req) => {
  console.log(`✅ StackChan 机器人已连接：${req.socket.remoteAddress}`);
  robotSocket = ws;

  ws.on('message', (message) => {
    console.log('机器人上报：', message.toString());
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

// 当前实际已经完成实体测试的 MCP 工具定义。
// shake / tilt / TTS 尚未在当前正式固件中启用，因此不对 PWA 暴露。
const controlRobotTool = {
  name: 'control_robot',
  description:
    '控制角色 A 的 StackChan 实体身体。可设置表情，并执行已经过实体验证的 nod（点头）或 home（回到正中）。不要使用未提供的动作。',
  inputSchema: {
    type: 'object',
    properties: {
      expression: {
        type: 'string',
        enum: ['happy', 'sad', 'angry', 'doubt', 'sleepy', 'neutral'],
        description: '机器人表情。'
      },
      motion: {
        type: 'string',
        enum: ['nod', 'home', 'none'],
        description:
          '机器人动作：nod 为点头一次且自动回中；home 为立即回到正中；none 为不执行动作。'
      },
      text_to_speak: {
        type: 'string',
        description:
          '角色 A 希望说出的文本。目前机器人 TTS 尚未接入，此字段会被安全忽略。'
      }
    },
    required: ['expression']
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

  return {
    expression: allowedExpressions.includes(args.expression)
      ? args.expression
      : 'neutral',

    motion: allowedMotions.includes(args.motion)
      ? args.motion
      : 'none',

    text: typeof args.text_to_speak === 'string'
      ? args.text_to_speak
      : ''
  };
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

/*
 * 标准 MCP Streamable HTTP 入口。
 *
 * PWA 应填写：
 * https://stackchan-mcp-server.onrender.com/mcp
 *
 * 支持：
 * - initialize
 * - notifications/initialized
 * - tools/list
 * - tools/call
 *
 * 这是无状态 JSON 响应模式：每次请求独立处理，
 * 不保存 PWA 的聊天记录、人设或模型 API Key。
 */
app.post('/mcp', (req, res) => {
  const request = req.body;

  if (!request || request.jsonrpc !== '2.0' || typeof request.method !== 'string') {
    return res.status(400).json(
      jsonRpcError(null, -32600, 'Invalid JSON-RPC request')
    );
  }

  const { id, method, params = {} } = request;

  console.log(`MCP request: ${method}`);

  // JSON-RPC notification 没有 id，按规范返回 202 且不带响应体。
  const isNotification = id === undefined || id === null;

  if (method === 'initialize') {
    const result = {
      protocolVersion: params.protocolVersion || '2025-03-26',
      capabilities: {
        tools: {}
      },
      serverInfo: {
        name: 'stackchan-robot-relay',
        version: '1.0.0'
      },
      instructions:
        '此服务只负责转发角色 A 对 StackChan 实体身体的控制命令。当前可用：表情、nod 点头、home 回中。'
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
          tools: [controlRobotTool]
        })
      );
  }

  if (method === 'tools/call') {
    if (params.name !== 'control_robot') {
      return res
        .status(200)
        .type('application/json')
        .json(
          jsonRpcError(id, -32601, `Unknown tool: ${params.name || '(missing)'}`)
        );
    }

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

    const { expression, motion } = result.payload;

    return res
      .status(200)
      .type('application/json')
      .json(
        jsonRpcSuccess(id, {
          content: [
            {
              type: 'text',
              text: `机器人已收到指令：表情=${expression}，动作=${motion}`
            }
          ]
        })
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

// Streamable HTTP MCP 客户端有时会发 GET 探测。
// 当前服务使用无状态、非 SSE 的 JSON 响应模式，因此明确返回 405。
app.get('/mcp', (req, res) => {
  res
    .status(405)
    .set('Allow', 'POST')
    .type('text')
    .send('This MCP endpoint accepts POST JSON-RPC requests.');
});

/*
 * 以下是旧的临时接口。
 * 保留它，以免影响你此前在浏览器 Console 中已测试成功的 fetch 调用。
 */

// 健康检查
app.get('/', (req, res) => {
  res.type('text').send('StackChan relay server is running.');
});

// 旧工具说明接口
app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [controlRobotTool]
  });
});

// 旧的自定义控制接口
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

  const { expression, motion } = result.payload;

  return res.json({
    content: [
      {
        type: 'text',
        text: `已发送机器人指令：expression=${expression}, motion=${motion}`
      }
    ]
  });
});

const PORT = process.env.PORT || 3000;

server.listen(PORT, () => {
  console.log(`Server is running on port ${PORT}`);
});

