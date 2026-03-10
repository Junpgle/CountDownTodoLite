#pragma once
#include "common.h"

// ============================================================
// ws_pomodoro.h
// 番茄钟跨端 WebSocket 实时感知
// ============================================================

// 连接（登录成功后调用；已连接则无操作）
void WsPomodoroConnect();

// 断开（退出登录时调用）
void WsPomodoroDisconnect();

// 发送 START 消息（本机开始专注时调用）
// targetEndMs   : 专注结束 UTC ms
// plannedSecs   : 计划专注秒数
// todoContent   : 绑定待办内容（可为空）
// todoUuid      : 绑定待办 UUID（可为空），供对端直接展示标题
// isRest        : 是否休息阶段
// tagNames      : 当前选中标签的「名字」字符串列表（明文，直接放 JSON 数组）
void WsPomodoroSendStart(long long targetEndMs, int plannedSecs,
                          const std::wstring& todoContent,
                          const std::wstring& todoUuid = L"",
                          bool isRest = false,
                          const std::vector<std::wstring>& tagNames = {});

// 仅更新标签（不改变倒计时）
void WsPomodoroSendUpdateTags(const std::vector<std::wstring>& tagNames);

// 发送 STOP / INTERRUPT 消息（本机停止或中断时调用）
void WsPomodoroSendStop();

void WsPomodoroSendReconnectSync(long long targetEndMs,
                                 int plannedSecs,
                                 const std::wstring& todoContent,
                                 const std::wstring& todoUuid,
                                 bool isRest,
                                 const std::vector<std::wstring>& tagNames);