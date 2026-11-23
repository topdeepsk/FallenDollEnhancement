#include "Dll/UeRuntime.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

#include "Common/UeOffsets.h"
#include "Common/Win32Util.h"

namespace ge::ue {

namespace {

uintptr_t g_namePool = 0;
bool g_ready = false;

uintptr_t ModuleBase() {
    return reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
}

// FName 池里一条记录：头 2 字节编码了长度（bits 6..14）与"宽字符"标志（bit 15），
// 低位 6 bit 是名字哈希，与解析无关。这个编码在 5.4 之后才有，本工程只面向该范围。
bool ReadNameEntry(uint32_t index, std::string& out) {
    out.clear();
    const uint32_t block = index >> 16;
    const uint32_t offset = (index & 0xFFFFu) * 2u;

    uintptr_t blockPtr = 0;
    if (!Read(g_namePool + kNamePoolBlocks + static_cast<uintptr_t>(block) * 8, blockPtr) ||
        !IsUserPointer(blockPtr)) {
        return false;
    }

    uint16_t header = 0;
    if (!Read(blockPtr + offset, header)) {
        return false;
    }
    const uint32_t length = (header >> kNameEntryLenShift) & 0x1FFu;
    const bool wide = (header & (1u << kNameEntryWideBit)) != 0;
    if (length == 0 || length > kNameEntryMaxLen) {
        return false;
    }

    if (!wide) {
        out.resize(length);
        if (!ReadMemory(blockPtr + offset + 2, out.data(), length)) {
            out.clear();
            return false;
        }
        // 允许 UTF-8（>=0x80）但去掉控制字符；正常类名/属性名都是 ASCII。
        for (unsigned char ch : out) {
            if (ch < 0x20) {
                out.clear();
                return false;
            }
        }
        return true;
    }

    std::wstring buffer(length, L'\0');
    if (!ReadMemory(blockPtr + offset + 2, buffer.data(), length * sizeof(wchar_t))) {
        return false;
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), static_cast<int>(length),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return false;
    }
    out.resize(static_cast<size_t>(bytes));
    ::WideCharToMultiByte(CP_UTF8, 0, buffer.c_str(), static_cast<int>(length), out.data(), bytes,
                          nullptr, nullptr);
    return true;
}

// 类对象自身也是对象：读出它的类名，用来判断"这是个 UClass 吗"。
bool ReadClassName(uintptr_t cls, std::string& out) {
    uintptr_t meta = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!ReadObjectHeader(cls, meta, index, number, outer)) {
        return false;
    }
    return ReadNameEntry(index, out);
}

bool IsClassLike(const std::string& className) {
    return className == "Class" || className == "ASClass" ||
           className == "BlueprintGeneratedClass" || className == "WidgetBlueprintGeneratedClass" ||
           className == "UserDefinedClass" || className == "DynamicClass";
}

}  // namespace

bool InitRuntime() {
    if (g_ready) {
        return true;
    }
    const uintptr_t base = ModuleBase();
    if (base == 0) {
        return false;
    }
    g_namePool = base + kGNamesRva;

    // 自检：索引 0 必须是 "None"，否则说明 GNamesRVA 已随版本失效。
    std::string none;
    if (!ReadNameEntry(0, none) || none != "None") {
        LogLine("反射：GNames 自检失败（索引 0 = \"%s\"），kGNamesRva 可能已过期",
                none.c_str());
        g_namePool = 0;
        return false;
    }

    uintptr_t table = 0;
    if (!Read(base + kGObjectsRva, table) || !IsUserPointer(table)) {
        LogLine("反射：GObjects 读取失败，kGObjectsRva 可能已过期");
        g_namePool = 0;
        return false;
    }

    g_ready = true;
    LogLine("反射：对象表就绪（GObjects=0x%llX GNames=0x%llX）",
            static_cast<unsigned long long>(table), static_cast<unsigned long long>(g_namePool));
    return true;
}

bool ReadObjectHeader(uintptr_t object, uintptr_t& cls, uint32_t& nameIndex, uint32_t& number,
                      uintptr_t& outer) {
    uint8_t buffer[0x28] = {};
    if (!ReadMemory(object, buffer, sizeof(buffer))) {
        return false;
    }
    memcpy(&cls, buffer + kUObjectClass, sizeof(cls));
    memcpy(&nameIndex, buffer + kUObjectName, sizeof(nameIndex));
    memcpy(&number, buffer + kUObjectName + 4, sizeof(number));
    memcpy(&outer, buffer + kUObjectOuter, sizeof(outer));
    return true;
}

std::string ObjectName(uintptr_t object) {
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!g_namePool || !ReadObjectHeader(object, cls, index, number, outer)) {
        return {};
    }
    std::string name;
    if (!ReadNameEntry(index, name)) {
        return {};
    }
    if (number != 0) {
        name += "_" + std::to_string(number);
    }
    return name;
}

std::string ClassNameOf(uintptr_t object) {
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!g_namePool || !ReadObjectHeader(object, cls, index, number, outer) || cls == 0) {
        return {};
    }
    std::string name;
    ReadClassName(cls, name);
    return name;
}

void ScanObjects(bool (*visit)(uintptr_t object, void* user), void* user) {
    if (!g_ready || visit == nullptr) {
        return;
    }

    uintptr_t table = 0;
    if (!Read(ModuleBase() + kGObjectsRva, table) || !IsUserPointer(table)) {
        return;
    }

    std::vector<uint8_t> buffer(static_cast<size_t>(kObjectChunkSize) * kObjectItemStride);
    for (uint32_t chunkIndex = 0; chunkIndex < 32; ++chunkIndex) {
        uintptr_t chunk = 0;
        if (!Read(table + static_cast<uintptr_t>(chunkIndex) * 8, chunk) || chunk == 0) {
            break;
        }

        // 最后一块可能没提交满，按 512KB 分段读，读到失败为止。
        size_t done = 0;
        while (done < buffer.size()) {
            const size_t piece = std::min<size_t>(0x80000, buffer.size() - done);
            if (!ReadMemory(chunk + done, buffer.data() + done, piece)) {
                break;
            }
            done += piece;
        }
        if (done < kObjectItemStride) {
            continue;
        }

        const uint32_t count = static_cast<uint32_t>(done / kObjectItemStride);
        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t object = 0;
            memcpy(&object, buffer.data() + static_cast<size_t>(i) * kObjectItemStride +
                                kObjectItemObject,
                   sizeof(object));
            if (object != 0 && !visit(object, user)) {
                return;
            }
        }
    }
}

bool FindClassByName(const char* className, uintptr_t& outClass) {
    outClass = 0;
    if (!g_ready || className == nullptr) {
        return false;
    }

    struct Search {
        const char* wanted;
        uintptr_t found;
        std::unordered_map<uintptr_t, std::string> classNames;
    } search{className, 0, {}};

    auto visit = [](uintptr_t object, void* user) -> bool {
        auto* state = static_cast<Search*>(user);
        uintptr_t cls = 0;
        uint32_t index = 0;
        uint32_t number = 0;
        uintptr_t outer = 0;
        if (!ReadObjectHeader(object, cls, index, number, outer) || cls == 0) {
            return true;
        }
        auto it = state->classNames.find(cls);
        if (it == state->classNames.end()) {
            std::string clsName;
            ReadClassName(cls, clsName);
            it = state->classNames.emplace(cls, std::move(clsName)).first;
        }
        if (!IsClassLike(it->second)) {
            return true;
        }
        if (ObjectName(object) == state->wanted) {
            state->found = object;
            return false;
        }
        return true;
    };

    ScanObjects(visit, &search);
    outClass = search.found;
    return outClass != 0;
}

bool FindInstanceOfClass(uintptr_t cls, const char* exactName, uintptr_t& outInstance) {
    outInstance = 0;
    if (!g_ready || cls == 0) {
        return false;
    }

    struct Search {
        uintptr_t cls;
        const char* exactName;
        uintptr_t found;
    } search{cls, exactName, 0};

    auto visit = [](uintptr_t object, void* user) -> bool {
        auto* state = static_cast<Search*>(user);
        uintptr_t objClass = 0;
        uint32_t index = 0;
        uint32_t number = 0;
        uintptr_t outer = 0;
        if (!ReadObjectHeader(object, objClass, index, number, outer) || objClass != state->cls) {
            return true;
        }
        const std::string name = ObjectName(object);
        if (state->exactName != nullptr) {
            if (name == state->exactName) {
                state->found = object;
                return false;
            }
            return true;
        }
        // 没指定名字时跳过 CDO（Default__Xxx），那只是类默认对象。
        if (name.rfind("Default__", 0) == 0) {
            return true;
        }
        state->found = object;
        return false;
    };

    ScanObjects(visit, &search);
    outInstance = search.found;
    return outInstance != 0;
}

bool FindProperty(uintptr_t cls, const char* name, PropInfo& out) {
    out = PropInfo{};
    if (!g_ready || cls == 0 || name == nullptr) {
        return false;
    }

    const uint32_t limit = PropertiesSize(cls);
    uintptr_t field = 0;
    if (!Read(cls + kUStructChildProperties, field) || field == 0) {
        return false;
    }

    for (int guard = 0; field != 0 && guard < 512; ++guard) {
        uint8_t buffer[0x58] = {};
        if (!ReadMemory(field, buffer, sizeof(buffer))) {
            return false;
        }

        uint32_t nameIndex = 0;
        memcpy(&nameIndex, buffer + kFFieldName, sizeof(nameIndex));

        int32_t offset = 0;
        int32_t elementSize = 0;
        memcpy(&offset, buffer + kFPropertyOffset, sizeof(offset));
        memcpy(&elementSize, buffer + kFPropertyElementSize, sizeof(elementSize));

        uintptr_t next = 0;
        memcpy(&next, buffer + kFFieldNext, sizeof(next));

        std::string fieldName;
        if (ReadNameEntry(nameIndex, fieldName) && fieldName == name) {
            if (offset < 0 || (limit != 0 && static_cast<uint32_t>(offset) >= limit) ||
                elementSize <= 0) {
                return false;
            }
            out.offset = static_cast<uint32_t>(offset);
            out.size = static_cast<uint32_t>(elementSize);
            out.valid = true;
            return true;
        }
        field = next;
    }
    return false;
}

bool FindFunctionOfClass(uintptr_t cls, const char* name, uintptr_t& outFunc) {
    outFunc = 0;
    if (!g_ready || cls == 0 || name == nullptr) {
        return false;
    }

    struct Search {
        uintptr_t cls;
        const char* name;
        uintptr_t found;
        std::unordered_map<uintptr_t, bool> isFunction;  // cls 指针 -> 是否 Function 家族
    } search{cls, name, 0, {}};

    auto visit = [](uintptr_t object, void* user) -> bool {
        auto* state = static_cast<Search*>(user);
        uintptr_t objClass = 0;
        uint32_t index = 0;
        uint32_t number = 0;
        uintptr_t outer = 0;
        if (!ReadObjectHeader(object, objClass, index, number, outer) || outer != state->cls) {
            return true;
        }

        auto it = state->isFunction.find(objClass);
        if (it == state->isFunction.end()) {
            std::string clsName;
            ReadClassName(objClass, clsName);
            const bool isFn = clsName == "Function" || clsName.rfind("ASFunction", 0) == 0 ||
                              clsName.rfind("DelegateFunction", 0) == 0;
            it = state->isFunction.emplace(objClass, isFn).first;
        }
        if (!it->second) {
            return true;
        }
        if (ObjectName(object) == state->name) {
            state->found = object;
            return false;
        }
        return true;
    };

    ScanObjects(visit, &search);
    outFunc = search.found;
    return outFunc != 0;
}

uint32_t PropertiesSize(uintptr_t cls) {    uint32_t size = 0;
    if (cls == 0 || !Read(cls + kUStructPropertiesSize, size)) {
        return 0;
    }
    return size;
}

bool IsAlive(uintptr_t object, uintptr_t expectedClass) {
    if (object == 0 || expectedClass == 0) {
        return false;
    }
    uintptr_t cls = 0;
    uint32_t index = 0;
    uint32_t number = 0;
    uintptr_t outer = 0;
    if (!ReadObjectHeader(object, cls, index, number, outer)) {
        return false;
    }
    return cls == expectedClass;
}

}  // namespace ge::ue
