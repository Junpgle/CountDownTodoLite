const WebSocket = require('ws');
const wss = new WebSocket.Server({ port: 8081 });

// 获取当前格式化时间的辅助函数，格式：[YYYY-MM-DD HH:mm:ss]
function getTime() {
  const d = new Date();
  // 根据服务器本地时区格式化，强制 24 小时制
  return `[${d.toLocaleString('zh-CN', { hour12: false }).replace(/\//g, '-')}]`;
}

// 结构：Map<userId, { clients: Map<deviceId, WebSocket>, focusState: Object|null }>
const activeRooms = new Map();

wss.on('connection', (ws, req) => {
  const url = new URL(req.url, `http://${req.headers.host}`);
  const userId = url.searchParams.get('userId');
  const deviceId = url.searchParams.get('deviceId');

  if (!userId || !deviceId) {
    ws.close(1008, 'Missing userId or deviceId');
    return;
  }

  // 1. 初始化房间（增加 focusState 记忆）
  if (!activeRooms.has(userId)) {
    activeRooms.set(userId, {
      clients: new Map(),
      focusState: null // 房间初始状态：无人在专注
    });
  }
  const room = activeRooms.get(userId);

  // 2. 顶号逻辑（踢掉旧连接）
  if (room.clients.has(deviceId)) {
    room.clients.get(deviceId).close();
  }

  // 3. 加入新连接
  room.clients.set(deviceId, ws);
  ws.deviceId = deviceId;
  console.log(`${getTime()} [上线] 用户 ${userId} 的设备 ${deviceId} 已连接。当前设备数: ${room.clients.size}`);

  // 🌟 4. 核心修复：新设备刚连上时，检查“黑板”。如果有人在专注，立刻告诉新设备！
  if (room.focusState !== null) {
    console.log(`${getTime()} [同步] 向刚上线的 ${deviceId} 推送历史状态 (发起端: ${room.focusState.sourceDevice})`);
    ws.send(JSON.stringify({
      action: 'SYNC', // 用 SYNC 表示这是历史状态同步，不是刚按下的
      ...room.focusState
    }));
  }

  // 5. 监听消息
  ws.on('message', (messageAsString) => {
    try {
      const data = JSON.parse(messageAsString);

      // 🚀 忽略心跳 PING，不广播
      if (data.action === 'PING') return;

      const payload = { sourceDevice: deviceId, timestamp: Date.now(), ...data };

      // 🌟 6. 更新“黑板”记忆
      if (data.action === 'START') {
        room.focusState = payload; // 记下：有人开始专注了
        console.log(`${getTime()} [记录] 设备 ${deviceId} 发起了专注。`);
      } else if (data.action === 'STOP' || data.action === 'INTERRUPT') {
        room.focusState = null;    // 擦除：专注结束了
        console.log(`${getTime()} [擦除] 设备 ${deviceId} 结束/中断了专注。`);
      }

      // 7. 广播给其他人
      for (const client of room.clients.values()) {
        if (client !== ws && client.readyState === WebSocket.OPEN) {
          client.send(JSON.stringify(payload));
        }
      }
    } catch (e) {
      console.error(`${getTime()} [错误] 解析消息失败:`, e);
    }
  });

  // 8. 断开清理
  ws.on('close', () => {
    if (room.clients.get(deviceId) === ws) {
      room.clients.delete(deviceId);
      console.log(`${getTime()} [下线] 用户 ${userId} 的设备 ${deviceId} 已断开。剩余设备数: ${room.clients.size}`);
    }
    // 当所有设备都下线时，摧毁房间释放内存
    if (room.clients.size === 0) {
      activeRooms.delete(userId);
      console.log(`${getTime()} [销毁] 用户 ${userId} 的所有设备已下线，房间已释放。`);
    }
  });
});

console.log(`${getTime()} 🚀 WebSocket 启动：已加入迟到同步机制`);