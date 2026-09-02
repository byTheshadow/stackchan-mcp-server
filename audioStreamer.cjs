'use strict';

const { spawn } = require('child_process');
const { once } = require('events');
const { EventEmitter } = require('events');
const ffmpegPath = require('ffmpeg-static');

if (!ffmpegPath) {
  throw new Error(
    'ffmpeg-static did not provide an ffmpeg executable'
  );
}

const OPEN_STATE = 1;

const SAMPLE_RATE = 16000;
const CHANNELS = 1;
const BITS_PER_SAMPLE = 16;

const CHUNK_SIZE = 4096;

const DOWNLOAD_TIMEOUT_MS = 15000;
const CHUNK_ACK_TIMEOUT_MS = 10000;
const AUDIO_READY_TIMEOUT_MS = 10000;
const MAX_BUFFERED_AMOUNT_BYTES = 256 * 1024;

const activeStreamsByRobot = new Map();
const robotAudioQueues = new Map();

console.log('[audio] ffmpeg path:', ffmpegPath);

/**
 * 为同一机器人串行执行音频任务。
 *
 * 注意：
 * stopRobotAudio 不使用这个队列，
 * 因为停止操作必须能够立即打断当前任务。
 */
function withRobotAudioLock(robotId, task) {
  const previous =
    robotAudioQueues.get(robotId) ||
    Promise.resolve();

  const next = previous.then(
    task,
    task
  );

  const tracked = next
    .catch(() => {})
    .finally(() => {
      if (
        robotAudioQueues.get(robotId) === tracked
      ) {
        robotAudioQueues.delete(robotId);
      }
    });

  robotAudioQueues.set(robotId, tracked);

  return next;
}

function makeStreamId() {
  return (
    'audio_' +
    Date.now().toString(36) +
    '_' +
    Math.random().toString(36).slice(2, 8)
  );
}

function isSocketOpen(robotSocket) {
  return (
    robotSocket &&
    robotSocket.readyState === OPEN_STATE
  );
}

/**
 * 由 index.js 的 WebSocket message 处理函数调用。
 */
function attachAudioProtocolHandlers(robotId, msg) {
  const state =
    activeStreamsByRobot.get(robotId);

  if (
    !state ||
    !state.streamId ||
    !msg ||
    msg.stream_id !== state.streamId
  ) {
    return;
  }

  if (msg.type === 'audio_ready') {
    state.emitter.emit('audio_ready');
    return;
  }

  if (msg.type === 'audio_chunk_ack') {
    const bytesWritten = Number(
      msg.bytes_written
    );

    if (
      !Number.isFinite(bytesWritten) ||
      bytesWritten < 0
    ) {
      console.warn(
        '[audio] invalid audio_chunk_ack:',
        msg
      );
      return;
    }

    state.emitter.emit(
      'audio_chunk_ack',
      bytesWritten
    );

    return;
  }

  if (msg.type === 'audio_abort') {
    state.emitter.emit(
      'audio_abort',
      msg.reason || 'unknown'
    );
  }
}

async function waitForSendBufferDrain(robotSocket) {
  while (
    Number(robotSocket.bufferedAmount || 0) >
    MAX_BUFFERED_AMOUNT_BYTES
  ) {
    await new Promise((resolve) => {
      setTimeout(resolve, 20);
    });

    if (!isSocketOpen(robotSocket)) {
      throw new Error(
        'robot_socket_closed_during_send'
      );
    }
  }
}

function waitForEvent(
  emitter,
  eventName,
  timeoutMs
) {
  return new Promise((resolve, reject) => {
    let settled = false;
    let timer = null;

    function cleanup() {
      emitter.removeListener(
        eventName,
        onEvent
      );

      if (timer !== null) {
        clearTimeout(timer);
        timer = null;
      }
    }

    function onEvent(payload) {
      if (settled) {
        return;
      }

      settled = true;
      cleanup();
      resolve(payload);
    }

    emitter.once(eventName, onEvent);

    /*
     * Infinity 表示不设置超时。
     * 不能使用 Number.MAX_SAFE_INTEGER，
     * 因为它超过 Node.js setTimeout 的有效范围。
     */
    if (
      timeoutMs !== undefined &&
      timeoutMs !== Infinity
    ) {
      timer = setTimeout(() => {
        if (settled) {
          return;
        }

        settled = true;
        cleanup();

        reject(
          new Error(
            `timeout_waiting_for_${eventName}`
          )
        );
      }, timeoutMs);
    }
  });
}

function sendJson(robotSocket, payload) {
  if (!isSocketOpen(robotSocket)) {
    throw new Error(
      'robot_socket_closed_during_send'
    );
  }

  robotSocket.send(
    JSON.stringify(payload)
  );
}

/**
 * 立即中断当前音频。
 *
 * 此函数不加队列锁，确保 stop 操作可以
 * 立即唤醒正在等待 audio_ready 或 ACK 的播放任务。
 */
async function stopRobotAudioNow(
  robotSocket,
  robotId,
  reason = 'stopped_by_tool_call'
) {
  const state =
    activeStreamsByRobot.get(robotId);

  if (!state) {
    return;
  }

  console.log(
    `[audio] stopping stream ${state.streamId}: ${reason}`
  );

  if (isSocketOpen(robotSocket)) {
    try {
      robotSocket.send(
        JSON.stringify({
          type: 'audio_stop',
          stream_id: state.streamId
        })
      );
    } catch (error) {
      console.warn(
        '[audio] failed to send audio_stop:',
        error.message
      );
    }
  }

  /*
   * 先通知等待者，再删除状态。
   */
  state.emitter.emit(
    'audio_abort',
    reason
  );

  if (
    activeStreamsByRobot.get(robotId) === state
  ) {
    activeStreamsByRobot.delete(robotId);
  }

  await new Promise((resolve) => {
    setTimeout(resolve, 50);
  });
}

/**
 * 对外停止接口。
 *
 * 停止必须立即执行，不能等待播放队列。
 */
async function stopRobotAudio(
  robotSocket,
  robotId,
  reason = 'stopped_by_tool_call'
) {
  return stopRobotAudioNow(
    robotSocket,
    robotId,
    reason
  );
}

/**
 * 实际播放逻辑。
 *
 * 该函数只能由加锁后的 playRobotAudio 调用。
 */
async function playRobotAudioInner(
  robotSocket,
  robotId,
  audioUrl
) {
  if (!isSocketOpen(robotSocket)) {
    throw new Error('robot_not_connected');
  }

  if (
    typeof audioUrl !== 'string' ||
    audioUrl.length === 0
  ) {
    throw new Error('invalid_audio_url');
  }

  /*
   * 如果存在旧流，立即停止。
   * 由于当前函数已经在队列中执行，
   * 不会与另一个 playRobotAudio 并发。
   */
  await stopRobotAudioNow(
    robotSocket,
    robotId,
    'interrupted_by_new_audio'
  );

  const streamId = makeStreamId();
  const emitter = new EventEmitter();

  emitter.setMaxListeners(20);

  const state = {
    streamId,
    emitter
  };

  activeStreamsByRobot.set(
    robotId,
    state
  );

  let ffmpeg = null;
  let response = null;
  let downloadController = null;

  try {
    /*
     * 下载音频。
     */
    downloadController =
      new AbortController();

    const downloadTimer = setTimeout(() => {
      downloadController.abort();
    }, DOWNLOAD_TIMEOUT_MS);

    try {
      response = await fetch(audioUrl, {
        signal: downloadController.signal
      });
    } finally {
      clearTimeout(downloadTimer);
    }

    if (!response.ok || !response.body) {
      throw new Error(
        `audio_download_failed_${response.status}`
      );
    }

    /*
     * 启动 ffmpeg。
     */
    ffmpeg = spawn(ffmpegPath, [
      '-hide_banner',
      '-loglevel',
      'error',
      '-i',
      'pipe:0',
      '-f',
      's16le',
      '-acodec',
      'pcm_s16le',
      '-ar',
      String(SAMPLE_RATE),
      '-ac',
      String(CHANNELS),
      'pipe:1'
    ]);

    let ffmpegStderr = '';

    ffmpeg.stderr.on('data', (chunk) => {
      ffmpegStderr += chunk.toString();
    });

    const ffmpegExitPromise = once(
      ffmpeg,
      'close'
    );

    /*
     * 告知机器人音频开始。
     */
    sendJson(robotSocket, {
      type: 'audio_start',
      stream_id: streamId,
      format: 'pcm_s16le',
      sample_rate: SAMPLE_RATE,
      channels: CHANNELS,
      bits_per_sample: BITS_PER_SAMPLE
    });

    console.log(
      `[audio] audio_start sent: ${streamId}`
    );

    await waitForEvent(
      emitter,
      'audio_ready',
      AUDIO_READY_TIMEOUT_MS
    );

    console.log(
      `[audio] audio_ready received: ${streamId}`
    );

    /*
     * HTTP body -> ffmpeg stdin。
     */
    const feedFfmpegStdin = (async () => {
      try {
        for await (const chunk of response.body) {
          if (!ffmpeg.stdin.write(chunk)) {
            await once(
              ffmpeg.stdin,
              'drain'
            );
          }
        }
      } catch (error) {
        if (!ffmpeg.stdin.destroyed) {
          ffmpeg.stdin.destroy(error);
        }

        throw error;
      } finally {
        if (
          !ffmpeg.stdin.destroyed &&
          !ffmpeg.stdin.writableEnded
        ) {
          ffmpeg.stdin.end();
        }
      }
    })();

    let leftover = Buffer.alloc(0);
    let bytesSent = 0;

    async function sendPcmChunk(chunk) {
      await waitForSendBufferDrain(
        robotSocket
      );

      if (!isSocketOpen(robotSocket)) {
        throw new Error(
          'robot_socket_closed_during_send'
        );
      }

      console.log(
        `[audio] sending PCM chunk: ` +
        `${chunk.length} bytes, ` +
        `stream_id=${streamId}`
      );

      robotSocket.send(chunk);
      bytesSent += chunk.length;

      const ackBytes = await waitForEvent(
        emitter,
        'audio_chunk_ack',
        CHUNK_ACK_TIMEOUT_MS
      );

      console.log(
        `[audio] received PCM ACK: ` +
        `${ackBytes} bytes, ` +
        `stream_id=${streamId}`
      );
    }

    /*
     * 监听机器人主动中止。
     */
    const abortWatcher = waitForEvent(
      emitter,
      'audio_abort',
      Infinity
    ).then((reason) => {
      throw new Error(
        `robot_aborted_stream:${reason}`
      );
    });

    /*
     * PCM 分块发送。
     */
    const streamPcm = (async () => {
      for await (const pcmChunk of ffmpeg.stdout) {
        leftover = Buffer.concat([
          leftover,
          pcmChunk
        ]);

        while (
          leftover.length >= CHUNK_SIZE
        ) {
          const chunk = leftover.subarray(
            0,
            CHUNK_SIZE
          );

          leftover = leftover.subarray(
            CHUNK_SIZE
          );

          await sendPcmChunk(chunk);
        }
      }

      /*
       * 发送尾部，保持 16-bit PCM 偶数字节。
       */
      if (leftover.length > 0) {
        const usableLength =
          leftover.length -
          (leftover.length % 2);

        if (usableLength > 0) {
          await sendPcmChunk(
            leftover.subarray(
              0,
              usableLength
            )
          );
        }
      }
    })();

    await Promise.race([
      Promise.all([
        feedFfmpegStdin,
        streamPcm
      ]),
      abortWatcher
    ]);

    const [exitCode] =
      await ffmpegExitPromise;

    if (exitCode !== 0) {
      throw new Error(
        `ffmpeg_exit_${exitCode}: ` +
        ffmpegStderr.slice(-500)
      );
    }

    sendJson(robotSocket, {
      type: 'audio_end',
      stream_id: streamId
    });

    console.log(
      `[audio] stream ${streamId} done, ` +
      `${bytesSent} PCM bytes sent`
    );
  } catch (error) {
    console.error(
      `[audio] stream ${streamId} failed:`,
      error.message
    );

    if (
      downloadController &&
      !downloadController.signal.aborted
    ) {
      downloadController.abort();
    }

    if (ffmpeg && !ffmpeg.killed) {
      ffmpeg.kill('SIGKILL');
    }

    /*
     * 只有当前状态仍然属于本流时，
     * 才发送 audio_stop。
     */
    if (
      activeStreamsByRobot.get(robotId) ===
      state &&
      isSocketOpen(robotSocket)
    ) {
      try {
        robotSocket.send(
          JSON.stringify({
            type: 'audio_stop',
            stream_id: streamId
          })
        );
      } catch {
        // WebSocket 正在关闭时忽略错误。
      }
    }

    throw error;
  } finally {
    if (
      activeStreamsByRobot.get(robotId) ===
      state
    ) {
      activeStreamsByRobot.delete(robotId);
    }
  }
}

/**
 * 对外播放接口。
 *
 * 同一机器人上的多个播放请求会串行执行。
 */
async function playRobotAudio(
  robotSocket,
  robotId,
  audioUrl
) {
  return withRobotAudioLock(
    robotId,
    () =>
      playRobotAudioInner(
        robotSocket,
        robotId,
        audioUrl
      )
  );
}

module.exports = {
  playRobotAudio,
  stopRobotAudio,
  attachAudioProtocolHandlers
};
