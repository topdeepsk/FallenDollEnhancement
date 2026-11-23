#include "Injector/PeExport.h"

#include <cstring>
#include <fstream>
#include <vector>

#include <windows.h>

namespace ge {

namespace {

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        error = "无法打开 DLL 文件";
        return false;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0) {
        error = "DLL 文件为空";
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(out.data()), size)) {
        error = "读取 DLL 文件失败";
        return false;
    }
    return true;
}

// RVA -> 文件偏移：遍历节表，找到包含该 RVA 的节。
bool RvaToOffset(const std::vector<uint8_t>& image, uint32_t rva, uint32_t& outOffset) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const uint32_t start = section->VirtualAddress;
        const uint32_t span = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                             : section->SizeOfRawData;
        if (rva >= start && rva < start + span) {
            outOffset = section->PointerToRawData + (rva - start);
            return true;
        }
    }
    return false;
}

}  // namespace

bool FindExportRva(const std::wstring& dllPath, const char* exportName,
                   uint32_t& outRva, std::string& error) {
    outRva = 0;

    std::vector<uint8_t> image;
    if (!ReadWholeFile(dllPath, image, error)) {
        return false;
    }

    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        error = "文件太小，不是 PE";
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        error = "缺少 MZ 头";
        return false;
    }
    if (dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.size()) {
        error = "PE 头越界";
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        error = "缺少 PE 签名";
        return false;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        error = "DLL 不是 x64";
        return false;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "不是 PE32+ 可选头";
        return false;
    }

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) {
        error = "DLL 没有导出表";
        return false;
    }

    uint32_t exportOffset = 0;
    if (!RvaToOffset(image, dir.VirtualAddress, exportOffset) ||
        exportOffset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        error = "导出表偏移无效";
        return false;
    }

    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(image.data() + exportOffset);

    uint32_t namesOffset = 0;
    uint32_t ordinalsOffset = 0;
    uint32_t functionsOffset = 0;
    if (!RvaToOffset(image, exports->AddressOfNames, namesOffset) ||
        !RvaToOffset(image, exports->AddressOfNameOrdinals, ordinalsOffset) ||
        !RvaToOffset(image, exports->AddressOfFunctions, functionsOffset)) {
        error = "导出表子表偏移无效";
        return false;
    }

    const auto* names = reinterpret_cast<const uint32_t*>(image.data() + namesOffset);
    const auto* ordinals = reinterpret_cast<const WORD*>(image.data() + ordinalsOffset);
    const auto* functions = reinterpret_cast<const uint32_t*>(image.data() + functionsOffset);

    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        uint32_t nameOffset = 0;
        if (!RvaToOffset(image, names[i], nameOffset) || nameOffset >= image.size()) {
            continue;
        }
        const char* candidate = reinterpret_cast<const char*>(image.data() + nameOffset);
        const size_t remaining = image.size() - nameOffset;
        if (std::strncmp(candidate, exportName, remaining) != 0) {
            continue;
        }

        const WORD ordinal = ordinals[i];
        if (ordinal >= exports->NumberOfFunctions) {
            continue;
        }
        const uint32_t rva = functions[ordinal];

        // 落在导出目录范围内的 RVA 是转发导出（指向别的 DLL），这里用不了。
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
            error = "该导出是转发导出，不支持";
            return false;
        }

        outRva = rva;
        return true;
    }

    error = "DLL 里找不到导出 ";
    error += exportName;
    return false;
}

}  // namespace ge
