#pragma once
#include "ESPixelStick.h"

class c_FileMgr
{
public:
    using FileId = uint32_t;
    static const FileId INVALID_FILE_HANDLE = 0;
    enum FileMode { FileRead = 0, FileWrite, FileAppend };
    struct SdInfo { uint64_t MaxSize = 64 * 1024 * 1024, Available = 64 * 1024 * 1024, Used = 0; };
    struct Handle { String name; FileMode mode; };
    std::map<String, std::vector<uint8_t>> files;
    std::map<FileId, Handle> handles;
    FileId nextHandle = 1;
    uint64_t available = 64 * 1024 * 1024;
    bool installed = true;
    bool uploading = false;
    bool shortWrite = false;
    bool failRename = false;
    bool failRead = false;
    uint64_t bytesWritten = 0;
    uint64_t largestWrite = 0;
    unsigned rebuilds = 0;
    unsigned opens = 0;

    bool SdCardIsInstalled() const { return installed; }
    bool IsUploadInProgress() const { return uploading; }
    bool UploadInProgress() const { return uploading; }
    bool IsSdFileUploadInProgress() const { return uploading; }
    bool OpenSdFile(const String& name, FileMode mode, FileId& handle, int) {
        handle = INVALID_FILE_HANDLE;
        if (!installed || (mode == FileRead && !files.count(name))) return false;
        handle = nextHandle++;
        handles[handle] = {name, mode};
        if (mode == FileWrite) files[name].clear();
        ++opens;
        return true;
    }
    void CloseSdFile(FileId& handle) { handles.erase(handle); handle = INVALID_FILE_HANDLE; }
    uint64_t GetSdFileSize(const FileId& handle) const {
        auto found = handles.find(handle);
        return found == handles.end() ? 0 : GetSdFileSize(found->second.name);
    }
    uint64_t GetSdFileSize(const String& name) const { auto found = files.find(name); return found == files.end() ? 0 : found->second.size(); }
    bool SdFileExists(const String& name) const { return files.count(name) != 0; }
    uint64_t ReadSdFile(const FileId& handle, byte* data, uint64_t length, uint64_t position) {
        if (failRead || !handles.count(handle)) return 0;
        auto& content = files[handles.at(handle).name];
        if (position > content.size()) return 0;
        length = std::min<uint64_t>(length, content.size() - position);
        if (length) std::memcpy(data, content.data() + position, size_t(length));
        return length;
    }
    uint64_t WriteSdFile(const FileId& handle, byte* data, uint64_t length) {
        if (length > 16 * 1024 * 1024) throw std::runtime_error("Unzip passed a negative read result as an unsigned write size");
        if (!handles.count(handle)) return 0;
        largestWrite = std::max(largestWrite, length);
        uint64_t written = shortWrite && length ? length - 1 : length;
        auto& content = files[handles.at(handle).name];
        content.insert(content.end(), data, data + written);
        bytesWritten += written;
        return written;
    }
    bool DeleteSdFile(const String& name) { return files.erase(name) != 0; }
    bool RenameSdFile(const String& before, const String& after) {
        if (failRename || !files.count(before) || files.count(after) || before == after) return false;
        files[after] = std::move(files[before]);
        files.erase(before);
        return true;
    }
    bool ReplaceSdFile(const String& temporary, const String& target) {
        if (failRename || !files.count(temporary)) return false;
        files[target] = std::move(files[temporary]);
        files.erase(temporary);
        return true;
    }
    void GetSdInfo(SdInfo& info) { info.Available = available; }
    void BuildFseqList(bool) { ++rebuilds; }
    void GetListOfSdFiles(std::vector<String>& response) const { for (auto& file : files) response.push_back(file.first); }
    void FindFirstZipFile(String& name) const {
        name.clear();
        for (auto& file : files) {
            String lower = file.first;
            lower.toLowerCase();
            if (lower.endsWith(".xlz") || lower.endsWith(".zip")) { name = file.first; return; }
        }
    }
};
inline c_FileMgr FileMgr;
