#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <windows.h>

namespace ge {

// 读写自身进程内存。
//
// 刻意用 ReadProcessMemory / WriteProcessMemory 而不是直接解引用 + __try：
// 传入非法指针时它们返回 FALSE，不会抛访问违例，因此不需要 SEH
// （也就不会被 /EHsc 与优化器对 __try 的限制拖累）。
bool ReadMemory(uintptr_t address, void* buffer, size_t size);
bool WriteMemory(uintptr_t address, const void* buffer, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
    return ReadMemory(address, &out, sizeof(T));
}

template <typename T>
bool Write(uintptr_t address, const T& value) {
    return WriteMemory(address, &value, sizeof(T));
}

// 用户态指针的基本合法性检查：非空、8 字节对齐、落在用户空间。
bool IsUserPointer(uintptr_t value);

// 宽字符串转 UTF-8。
// 日志里不要用 %ls：CRT 的窄字符 printf 按当前 C locale 做宽窄转换，
// 默认 "C" locale 遇到中文直接失败，整行会变成空串。
std::string Narrow(const std::wstring& text);

// 追加一行日志到日志文件（默认在 GameEnhancement.dll 同目录），同时输出到调试器。
void LogLine(const char* format, ...);

// 日志文件与 GameEnhancement.ini 放在同一目录（DLL 所在目录），便于一起取走。
// DLL 在 DllMain 里调 SetLogDirectory 显式指定；没指定时退化为"本模块所在目录"。
void SetLogDirectory(const std::wstring& directory);
std::wstring GetLogFilePath();

std::wstring GetModuleFilePath(HMODULE module);

// 取路径里的目录部分（含结尾分隔符）；没有分隔符时返回空串。
std::wstring GetDirectoryOf(const std::wstring& filePath);

}  // namespace ge
