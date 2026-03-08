#pragma once
#include "common.h"

// ============================================================
// ws_pomodoro.h
// 番茄钟跨端 WebSocket 实时感知
// 职责：
//   - 登录后连接 WS 服务器（ws://host:8081?userId=X&deviceId=Y）
//   - 发起端：开始/停止/中断时发送 START/STOP/INTERRUPT
//   - 接收端：收到 START/SYNC/STOP 后更新 g_RemoteFocus，触发窗口重绘
// ============================================================

// 连接（登录成功后调用；已连接则无操作）
void WsPomodoroConnect();

// 断开（退出登录时调用）
void WsPomodoroDisconnect();

// 发送 START 消息（本机开始专注时调用）
// targetEndMs   : 专注结束 UTC ms
// plannedSecs   : 计划专注秒数
// todoContent   : 绑定待办内容（可为空）
// isRest        : 是否休息阶段
void WsPomodoroSendStart(long long targetEndMs, int plannedSecs,
                          const std::wstring& todoContent, bool isRest = false);

// 发送 STOP / INTERRUPT 消息（本机停止或中断时调用）
void WsPomodoroSendStop();

