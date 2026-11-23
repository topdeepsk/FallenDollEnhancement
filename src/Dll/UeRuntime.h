#pragma once

#include <cstdint>
#include <string>

namespace ge::ue {

// 运行时对象表访问（只读 + 属性定位）。
//
// 为什么不直接用 dump：这个游戏的玩法类（A_ClientHPreviewManager 等）是 AngelScript
// 声明的 ASClass，dump 里没有；而且 dump 里的 funcs[].addr 是 UFunction 对象地址，
// 不是代码地址，跨进程毫无用处。所以这里全部按"名字"在活进程里重新解析。
//
// 线程：Scan* 系列要遍历 20 多万个对象，耗时数百毫秒，只能在后台线程调用。
// 定位结果用偏移缓存下来，热路径（鼠标回调）只做常数次内存读写。

struct PropInfo {
    uint32_t offset = 0;   // 实例内字节偏移（FProperty::Offset_Internal）
    uint32_t size = 0;     // 元素大小
    bool     valid  = false;
};

// 读 GNames 并做一次自检（索引 0 应为 "None"）。只需调用一次。
bool InitRuntime();

// 单个对象的名字 / 类名。名字非法时返回空串。
std::string ObjectName(uintptr_t object);
std::string ClassNameOf(uintptr_t object);

// 把对象头一次读完：类指针、名字索引、Outer。
bool ReadObjectHeader(uintptr_t object, uintptr_t& cls, uint32_t& nameIndex,
                      uint32_t& number, uintptr_t& outer);

// 遍历全部对象。回调返回 false 提前结束。只在后台线程用。
void ScanObjects(bool (*visit)(uintptr_t object, void* user), void* user);

// 按类名找 UClass（"A_ClientHPreviewManager"）。名字匹配的是类对象自身的名字。
bool FindClassByName(const char* className, uintptr_t& outClass);

// 在该类的实例里找一个。exactName 为空时取第一个非 CDO（Default__）实例。
bool FindInstanceOfClass(uintptr_t cls, const char* exactName, uintptr_t& outInstance);

// 沿 ChildProperties 链找属性，返回它的实例内偏移。
bool FindProperty(uintptr_t cls, const char* name, PropInfo& out);

// 在类下找 UFunction/ASFunction 对象（Outer == cls，名字匹配）。
bool FindFunctionOfClass(uintptr_t cls, const char* name, uintptr_t& outFunc);

// UStruct::PropertiesSize，用于校验偏移没越界。
uint32_t PropertiesSize(uintptr_t cls);

// 指针是否仍然指向该类的一个活对象（防场景切换后写到野指针）。
bool IsAlive(uintptr_t object, uintptr_t expectedClass);

}  // namespace ge::ue
