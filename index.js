import express from 'express';
import cors from 'cors';
import { WebSocketServer } from 'ws';
import http from 'http';

const app = express();

app.use(cors());
app.use(express.json({ limit: '64kb' }));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// 当前连接的机器人。
// 注意：目前只做单机器人测试；以后可扩展为多机器人。
let robotSocket = null;

wss.on('connection', (ws, request) => {
  const ip = request.socket.remoteAddress;
  console.log(`✅ StackChan 机器人已连接：${ip}`);

  robotSocket = ws;

  ws.on('message', (message) => {
    console.log('📨 收到机器人消息：', message.toString());
  });

  ws.on('close', () => {
    if (robotSocket === ws) {
      robotSocket = null;
    }
    console.log('❌ StackChan 机器人已断开');
  });

  ws.on('error', (error) => {
    console.error('机器人 WebSocket 出错：', error.message);
  });
});

function isRobotOnline() {
  return robotSocket && robotSocket.readyState === 1;
}

function sendToRobot(payload) {
  if (!isRobotOnline()) {
    return false;
  }

  robotSocket.send(JSON.stringify(payload));
  console.log('➡️ 已下发给机器人：', payload);
  return true;
}

/**
 * 状态检查：
 * 浏览器打开 https://你的服务.onrender.com/status
 * 可查看机器人是否在线。
 */
app.get('/status', (req, res) => {
  res.json({
    ok: true,
    robot_online: isRobotOnline()
  });
});

/**
 * 临时的自定义工具描述接口。
 *
 * 注意：
 * 这还不是“标准 MCP Streamable HTTP”协议；
 * 它是我们先用于验证硬件链路的简单 REST 接口。
 */
app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [
      {
        name: 'control_robot',
        description: '控制现实中的 StackChan 机器人切换表情。当前测试版暂不含语音和舵机动作。',
        parameters: {
          type: 'object',
          properties: {
            expression: {
              type: 'string',
              enum: ['happy', 'sad', 'angry', 'doubt', 'sleepy', 'neutral'],
              description: '机器人的表情。'
            },
            text_to_speak: {
              type: 'string',
              description: '预留的未来语音文本字段。当前版本仅记录文本，不会朗读。'
            },
            motion: {
              type: 'string',
              enum: ['nod', 'shake', 'tilt', 'none'],
              description: '预留的未来动作字段。当前固件尚未实现舵机动作。'
            }
          },
          required: ['expression']
        }
      }
    ]
  });
});

/**
 * 控制机器人。
 * 当前实现：只改变表情。
 */
app.post('/mcp/call', (req, res) => {
  const { tool, arguments: args = {} } = req.body;

  if (tool !== 'control_robot') {
    return res.status(404).json({
      error: 'Tool not found'
    });
  }

  const allowedExpressions = [
    'happy',
    'sad',
    'angry',
    'doubt',
    'sleepy',
    'neutral'
  ];

  const expression = allowedExpressions.includes(args.expression)
    ? args.expression
    : 'neutral';

  // 保持 action 为 speak，是为了兼容你现在机器人端已经烧录的固件：
  // 它会根据 expression 切表情；
  // audio_url 留空，因此不会触发音频播放。
  const delivered = sendToRobot({
    action: 'speak',
    expression,
    audio_url: '',
    motion: args.motion || 'none',
    text: args.text_to_speak || ''
  });

  res.json({
    success: true,
    robot_online: delivered,
    content: [
      {
        type: 'text',
        text: delivered
          ? `机器人已切换为 ${expression} 表情。`
          : '指令已收到，但机器人目前不在线。'
      }
    ]
  });
});

app.get('/', (req, res) => {
  res.type('text').send(
    'StackChan relay server is running. Visit /status to check robot connection.'
  );
});

const PORT = process.env.PORT || 3000;

server.listen(PORT, () => {
  console.log(`Server is running on port ${PORT}`);
});

