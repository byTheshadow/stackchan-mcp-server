import express from 'express';
import cors from 'cors';
import { WebSocketServer } from 'ws';
import http from 'http';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';
import { EdgeTTS } from 'edge-tts-node';
import OpenAI from 'openai';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
app.use(cors());
app.use(express.json());

// 创建用于存放临时音频的目录
const AUDIO_DIR = path.join(__dirname, 'public', 'audio');
if (!fs.existsSync(AUDIO_DIR)) {
  fs.mkdirSync(AUDIO_DIR, { recursive: true });
}
app.use('/audio', express.static(AUDIO_DIR));

const server = http.createServer(app);
const wss = new WebSocketServer({ server });

// 存储当前在线的 StackChan 机器人连接
let robotSocket = null;

wss.on('connection', (ws) => {
  console.log('✅ StackChan 机器人已成功连接！');
  robotSocket = ws;

  ws.on('close', () => {
    console.log('❌ StackChan 机器人已断开连接');
    robotSocket = null;
  });

  ws.on('message', (msg) => {
    console.log('收到机器人上报消息:', msg.toString());
  });
});

// 辅助函数：调用 Edge-TTS 生成 MP3
async function generateTTS(text, voice = 'zh-CN-XiaoxiaoNeural') {
  const fileName = `tts_${Date.now()}.mp3`;
  const filePath = path.join(AUDIO_DIR, fileName);
  const tts = new EdgeTTS({ voice: voice, lang: 'zh-CN' });
  await tts.ttsToFile(text, filePath);
  return `/audio/${fileName}`;
}

// 辅助函数：向机器人推送动作与音频
function sendToRobot(actionData) {
  if (robotSocket && robotSocket.readyState === 1) {
    robotSocket.send(JSON.stringify(actionData));
    return true;
  }
  return false;
}

// ==========================================
// 1. MCP 协议端点（供 PWA 与大模型工具调用）
// ==========================================
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
              description: "机器人的面部表情"
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

app.post('/mcp/call', async (req, res) => {
  const { tool, arguments: args } = req.body;
  if (tool === 'control_robot') {
    try {
      const audioUrl = await generateTTS(args.text_to_speak);
      const fullAudioUrl = `${req.protocol}://${req.get('host')}${audioUrl}`;
      
      const payload = {
        action: 'speak',
        audio_url: fullAudioUrl,
        expression: args.expression || 'happy',
        motion: args.motion || 'nod'
      };

      const delivered = sendToRobot(payload);
      res.json({
        success: true,
        delivered: delivered,
        message: delivered ? "动作与语音已成功推送到机器人" : "指令已生成，但机器人当前未在线"
      });
    } catch (err) {
      res.status(500).json({ error: err.message });
    }
  } else {
    res.status(404).json({ error: "Tool not found" });
  }
});

// ==========================================
// 2. 主动说话模块（定时随机找你说话）
// ==========================================
const PROACTIVE_INTERVAL_MINUTES = 30; // 每隔 30 分钟检查一次是否要主动说话
const PROACTIVE_PROBABILITY = 0.6;     // 60% 概率触发

async function proactiveSpeak() {
  if (!robotSocket) return; // 机器人不在线就不打扰

  const apiKey = process.env.OPENAI_API_KEY;
  const baseURL = process.env.OPENAI_BASE_URL || 'https://api.openai.com/v1';
  const rolePrompt = process.env.ROLE_PROMPT || '你是主人的可爱桌面伴侣角色A。主人正在忙，请用一两句话主动关心一下主人。';

  if (!apiKey) return;

  try {
    const openai = new OpenAI({ apiKey, baseURL });
    const completion = await openai.chat.completions.create({
      model: process.env.MODEL_NAME || 'gpt-4o-mini',
      messages: [
        { role: 'system', content: rolePrompt },
        { role: 'user', content: '（环境提示：主人正在安静工作，你想要主动对主人说一句话陪伴他/她）' }
      ]
    });

    const replyText = completion.choices[0].message.content;
    const audioUrl = await generateTTS(replyText);
    const host = process.env.SERVER_URL || 'http://localhost:3000';

    sendToRobot({
      action: 'speak',
      audio_url: `${host}${audioUrl}`,
      expression: 'happy',
      motion: 'nod'
    });
    console.log('🤖 主动触发问候:', replyText);
  } catch (e) {
    console.error('主动问候触发失败:', e.message);
  }
}

// 启动定时检查
setInterval(() => {
  if (Math.random() < PROACTIVE_PROBABILITY) {
    proactiveSpeak();
  }
}, PROACTIVE_INTERVAL_MINUTES * 60 * 1000);

// 健康检查路由
app.get('/', (req, res) => {
  res.send('StackChan MCP & Relay Server is running!');
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
  console.log(`Server listening on port ${PORT}`);
});
