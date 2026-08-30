import express from 'express';
import cors from 'cors';
import { WebSocketServer } from 'ws';
import http from 'http';

const app = express();

app.use(cors());
app.use(express.json({ limit: '64kb' }));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// 当前在线的机器人连接。
// 目前仅支持一台机器人；以后可以用 robot_id 扩展多台。
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

// 健康检查：在浏览器打开 Render 根网址时会看到这个响应。
app.get('/', (req, res) => {
  res.type('text').send('StackChan relay server is running.');
});

// 目前对外提供的“工具说明”。
// 注意：这只是临时 JSON 接口，还不是完整标准的 MCP Streamable HTTP 协议。
// 我们在硬件功能验证完成后，再根据你的 PWA 的 MCP 接入方式改为标准协议。
app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [
      {
        name: 'control_robot',
        description: '控制 StackChan 的表情和动作。当前固件阶段仅验证表情；语音和舵机动作将在后续单独实现。',
        inputSchema: {
          type: 'object',
          properties: {
            expression: {
              type: 'string',
              enum: ['happy', 'sad', 'angry', 'doubt', 'sleepy', 'neutral'],
              description: '表情名称'
            },
            motion: {
              type: 'string',
              enum: ['nod', 'shake', 'tilt', 'home', 'none'],
              description: '动作名称；当前固件尚未实现舵机动作，仅会原样接收。'
            },
            text_to_speak: {
              type: 'string',
              description: '未来给 TTS 使用的文本；当前阶段不会播放。'
            }
          },
          required: ['expression']
        }
      }
    ]
  });
});

// 临时控制接口：把安全、简单的 JSON 指令经 WebSocket 转给机器人。
app.post('/mcp/call', (req, res) => {
  const { tool, arguments: args = {} } = req.body ?? {};

  if (tool !== 'control_robot') {
    return res.status(404).json({ error: 'Tool not found' });
  }

  const allowedExpressions = ['happy', 'sad', 'angry', 'doubt', 'sleepy', 'neutral'];
 const allowedMotions = ['nod', 'shake', 'tilt', 'home', 'none'];


  const expression = allowedExpressions.includes(args.expression)
    ? args.expression
    : 'neutral';

  const motion = allowedMotions.includes(args.motion)
    ? args.motion
    : 'none';

  if (!robotSocket || robotSocket.readyState !== 1) {
    return res.status(503).json({
      error: 'Robot is offline. Please check Wi-Fi and WebSocket connection.'
    });
  }

  // 此阶段不要发送 audio_url：
  // 因为我们尚未选定并接入可靠的 TTS 服务。
  const payload = {
    action: 'speak',
    expression,
    motion,
    text: typeof args.text_to_speak === 'string' ? args.text_to_speak : ''
  };

  robotSocket.send(JSON.stringify(payload));
  console.log('➡️ 已发送至机器人：', payload);

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


