
import express from 'express';
import cors from 'cors';
import { WebSocketServer } from 'ws';
import http from 'http';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { EdgeTTS } from 'edge-tts-node';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
app.use(cors());
app.use(express.json());

// 存放临时合成的 MP3
const AUDIO_DIR = path.join(__dirname, 'public', 'audio');
if (!fs.existsSync(AUDIO_DIR)) {
  fs.mkdirSync(AUDIO_DIR, { recursive: true });
}
app.use('/audio', express.static(AUDIO_DIR));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// 机器人的 WebSocket 长连接
let robotSocket = null;

wss.on('connection', (ws) => {
  console.log('✅ StackChan 机器人已连接');
  robotSocket = ws;

  ws.on('close', () => {
    console.log('❌ StackChan 机器人已断开');
    robotSocket = null;
  });

  ws.on('message', (msg) => {
    console.log('收到机器人数据:', msg.toString());
  });
});

// Edge-TTS 免费语音生成
async function generateTTS(text, voice = 'zh-CN-XiaoxiaoNeural') {
  const fileName = `tts_${Date.now()}.mp3`;
  const filePath = path.join(AUDIO_DIR, fileName);
  const tts = new EdgeTTS({ voice: voice, lang: 'zh-CN' });
  await tts.ttsToFile(text, filePath);
  return `/audio/${fileName}`;
}

// ==========================================
// MCP 协议标准端点
// ==========================================

// 1. 获取机器人支持的工具列表
app.get('/mcp/tools', (req, res) => {
  res.json({
    tools: [
      {
        name: "control_robot",
        description: "控制你的桌面实体机器人（StackChan）做出表情、动作并发出语音对主人说话。",
        parameters: {
          type: "object",
          properties: {
            text_to_speak: {
              type: "string",
              description: "机器人要用语音朗读出来的台词"
            },
            expression: {
              type: "string",
              enum: ["happy", "sad", "angry", "doubt", "sleepy", "neutral"],
              description: "机器人的面部表情：happy(开心), sad(难过), angry(生气), doubt(疑惑), sleepy(困倦), neutral(平静)"
            },
            motion: {
              type: "string",
              enum: ["nod", "shake", "tilt", "none"],
              description: "机器人的动作：nod(点头), shake(摇头), tilt(歪头), none(不动)"
            }
          },
          required: ["text_to_speak", "expression"]
        }
      }
    ]
  });
});

// 2. 执行工具调用
app.post('/mcp/call', async (req, res) => {
  const { tool, arguments: args } = req.body;

  if (tool === 'control_robot') {
    try {
      // 1. 生成语音音频
      const audioPath = await generateTTS(args.text_to_speak);
      const hostUrl = `${req.protocol}://${req.get('host')}`;
      const fullAudioUrl = `${hostUrl}${audioPath}`;

      // 2. 组装给机器人的指令
      const payload = {
        action: 'speak',
        audio_url: fullAudioUrl,
        expression: args.expression || 'happy',
        motion: args.motion || 'nod'
      };

      // 3. 通过 WebSocket 下发给 StackChan
      let delivered = false;
      if (robotSocket && robotSocket.readyState === 1) {
        robotSocket.send(JSON.stringify(payload));
        delivered = true;
      }

      res.json({
        content: [
          {
            type: "text",
            text: delivered 
              ? `[实体机器人] 成功以 ${args.expression} 表情做出 ${args.motion || 'nod'} 动作并朗读了台词。`
              : `[实体机器人] 指令已生成，但机器人当前未在线。`
          }
        ]
      });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  } else {
    res.status(404).json({ error: "Tool not found" });
  }
});

// 健康检查
app.get('/', (req, res) => {
  res.send('StackChan MCP Server is running! Ready to connect.');
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server is running on port ${PORT}`);
});
