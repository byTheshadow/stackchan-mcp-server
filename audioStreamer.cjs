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
const CHUNK_ACK_TIMEOUT_MS = 5000;
const AUDIO_READY_TIMEOUT_MS = 5000;
const MAX_BUFFERED_AMOUNT_BYTES = 256 * 1024;

const activeStreamsByRobot = new Map();


function makeStreamId() {
  return (
    'audio_' +
    Date.now().toString(36) +
    '_' +
    Math.random().toString(36).slice(2, 8)
  );
}

function attachAudioProtocolHandlers(robotId, msg) {
  const state = activeStreamsByRobot.get(robotId);

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
    state.emitter.emit(
      'audio_chunk_ack',
      msg.bytes_written
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

    if (
      !robotSocket ||
      robotSocket.readyState !== OPEN_STATE
    ) {
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
     * Infinity 或 undefined 表示永不超时。
     *
     * 不要使用 Number.MAX_SAFE_INTEGER，
     * Node.js 的 setTimeout 不支持这么大的延迟。
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
  if (
    !robotSocket ||
    robotSocket.readyState !== OPEN_STATE
  ) {
    throw new Error(
      'robot_socket_closed_during_send'
    );
  }

  robotSocket.send(JSON.stringify(payload));
}

async function playRobotAudio(
  robotSocket,
  robotId,
  audioUrl
) {
  if (
    !robotSocket ||
    robotSocket.readyState !== OPEN_STATE
  ) {
    throw new Error('robot_not_connected');
  }

  if (
    typeof audioUrl !== 'string' ||
    audioUrl.length === 0
  ) {
    throw new Error('invalid_audio_url');
  }

  await stopRobotAudio(
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

  activeStreamsByRobot.set(robotId, state);

  let ffmpeg = null;
  let response = null;
  let downloadController = null;

  try {
    /*
     * 下载音频。
     */
    downloadController = new AbortController();

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
     * ffmpeg：输入任意音频，输出 16kHz、单声道、
     * signed 16-bit little-endian PCM。
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
     * 告知固件音频格式。
     */
    sendJson(robotSocket, {
      type: 'audio_start',
      stream_id: streamId,
      format: 'pcm_s16le',
      sample_rate: SAMPLE_RATE,
      channels: CHANNELS,
      bits_per_sample: BITS_PER_SAMPLE
    });

    await waitForEvent(
      emitter,
      'audio_ready',
      AUDIO_READY_TIMEOUT_MS
    );

    /*
     * HTTP body -> ffmpeg stdin。
     */
    const feedFfmpegStdin = (async () => {
      try {
        for await (const chunk of response.body) {
          if (!ffmpeg.stdin.write(chunk)) {
            await once(ffmpeg.stdin, 'drain');
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
      await waitForSendBufferDrain(robotSocket);

      if (
        !robotSocket ||
        robotSocket.readyState !== OPEN_STATE
      ) {
        throw new Error(
          'robot_socket_closed_during_send'
        );
      }

      robotSocket.send(chunk);
      bytesSent += chunk.length;

      await waitForEvent(
        emitter,
        'audio_chunk_ack',
        CHUNK_ACK_TIMEOUT_MS
      );
    }
const abortWatcher = waitForEvent(
  emitter,
  'audio_abort',
  Infinity
).then((reason) => {
  throw new Error(
    `robot_aborted_stream:${reason}`
  );
});


    const streamPcm = (async () => {
      for await (const pcmChunk of ffmpeg.stdout) {
        leftover = Buffer.concat([
          leftover,
          pcmChunk
        ]);

        while (leftover.length >= CHUNK_SIZE) {
          const chunk = leftover.subarray(
            0,
            CHUNK_SIZE
          );

          leftover = leftover.subarray(CHUNK_SIZE);

          await sendPcmChunk(chunk);
        }
      }

      /*
       * PCM sample 为 16-bit，尾部必须保持偶数字节。
       */
      if (leftover.length > 0) {
        const usableLength =
          leftover.length -
          (leftover.length % 2);

        if (usableLength > 0) {
          await sendPcmChunk(
            leftover.subarray(0, usableLength)
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

    const [exitCode] = await ffmpegExitPromise;

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

    if (
      robotSocket &&
      robotSocket.readyState === OPEN_STATE
    ) {
      try {
        robotSocket.send(
          JSON.stringify({
            type: 'audio_stop',
            stream_id: streamId
          })
        );
      } catch {
        // WebSocket 可能正在关闭，忽略发送失败。
      }
    }

    throw error;
  } finally {
    if (
      activeStreamsByRobot.get(robotId) === state
    ) {
      activeStreamsByRobot.delete(robotId);
    }
  }
}

async function stopRobotAudio(
  robotSocket,
  robotId,
  reason = 'stopped_by_tool_call'
) {
  const state = activeStreamsByRobot.get(robotId);

  if (!state) {
    return;
  }

  if (
    robotSocket &&
    robotSocket.readyState === OPEN_STATE
  ) {
    try {
      robotSocket.send(
        JSON.stringify({
          type: 'audio_stop',
          stream_id: state.streamId
        })
      );
    } catch {
      // WebSocket 关闭时忽略发送失败。
    }
  }

  state.emitter.emit(
    'audio_abort',
    reason
  );

  activeStreamsByRobot.delete(robotId);

  await new Promise((resolve) => {
    setTimeout(resolve, 50);
  });
}

module.exports = {
  playRobotAudio,
  stopRobotAudio,
  attachAudioProtocolHandlers
};
