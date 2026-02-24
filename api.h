#pragma once
#include "common.h"

std::string SendRequest(const std::wstring &path, const std::string &method, const std::string &body);

void ApiToggleTodo(int id, bool currentStatus);
void ApiAddTodo(const std::wstring &content);
void ApiDeleteTodo(int id);
void ApiAddCountdown(const std::wstring &title, const std::wstring &dateStr);
void ApiDeleteCountdown(int id);

void SyncData();
bool AttemptAutoLogin();