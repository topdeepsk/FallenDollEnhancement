#pragma once

#include <cstdint>
#include <string>

namespace ge {

// 从磁盘上的 DLL 文件里解析导出表，取出某个导出函数的 RVA。
//
// 不 LoadLibrary 本地那份 DLL：那会执行它自己的 DllMain，
// 在注入器进程里装一遍钩子，纯属副作用。只读文件、只看导出表，干净。
bool FindExportRva(const std::wstring& dllPath, const char* exportName,
                   uint32_t& outRva, std::string& error);

}  // namespace ge
