#include <filesystem>
#include <system_error>
#include <unordered_map>

#include "Havk.h"

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#endif

namespace havk {

#ifdef _WIN32
struct FileWatcher::Impl {
    HANDLE _fileHandle;
    OVERLAPPED _overlapped = {};
    uint8_t _eventBuffer[4096];

    Impl(const std::filesystem::path& path) {
        _fileHandle = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        ReadChangesAsync();
    };
    ~Impl() {
        CloseHandle(_fileHandle);
        CloseHandle(_overlapped.hEvent);
    }
    void ReadChangesAsync() {
        _overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        ReadDirectoryChangesW(_fileHandle, _eventBuffer, sizeof(_eventBuffer), true, FILE_NOTIFY_CHANGE_LAST_WRITE,
                              NULL, &_overlapped, NULL);
    }
};

void FileWatcher::PollChanges(std::vector<std::filesystem::path>& changedFiles) {
    DWORD numBytesReceived;
    while (GetOverlappedResult(_impl->_fileHandle, &_impl->_overlapped, &numBytesReceived, false)) {
        uint8_t* eventPtr = _impl->_eventBuffer;
        while (true) {
            auto event = (FILE_NOTIFY_INFORMATION*)eventPtr;
            auto fileName = std::filesystem::path(event->FileName, event->FileName + event->FileNameLength / 2);

            if (event->Action == FILE_ACTION_MODIFIED &&
                // Some apps (VSCode) will cause multiple events to be fired for the same file.
                (changedFiles.size() == 0 || changedFiles[changedFiles.size() - 1] != fileName)) {
                changedFiles.push_back(fileName);
            }
            if (event->NextEntryOffset == 0) {
                break;
            }
            eventPtr += event->NextEntryOffset;
        }
        _impl->ReadChangesAsync();
    }
}

#elif __linux__

#include <sys/inotify.h>

struct FileWatcher::Impl {
    int _fd;
    std::unordered_map<int, std::filesystem::path> _subdirs;

    Impl(const std::filesystem::path& path) {
        _fd = inotify_init1(IN_NONBLOCK);

        const auto WatchDir = [&](const std::filesystem::path& path) {
            int wd = inotify_add_watch(_fd, path.c_str(), IN_MODIFY);
            if (wd < 0) {
                throw std::system_error(errno, std::generic_category(), "Failed to setup inotify");
            }
            _subdirs.insert_or_assign(wd, path);
        };

        WatchDir(path);

        for (auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            if (entry.is_directory()) {
                WatchDir(entry.path());
            }
        }
    }
    ~Impl() {
        close(_fd);
    }
};

void FileWatcher::PollChanges(std::vector<std::filesystem::path>& changedFiles) {
    char buffer[4096];
    int len;

    while ((len = read(_impl->_fd, buffer, sizeof(buffer))) > 0) {
        for (char* ptr = buffer; ptr < buffer + len; ) {
            auto event = (inotify_event*)ptr;
            auto fileName = _impl->_subdirs[event->wd] / event->name;

            if ((event->mask & IN_MODIFY) &&
                // Some apps (VSCode) will cause multiple events to be fired for the same file.
                (changedFiles.size() == 0 || changedFiles[changedFiles.size() - 1] != fileName)) {
                changedFiles.push_back(fileName);
            }
            ptr += sizeof(inotify_event) + event->len;
        }
    }
}

#else

#warning "FileWatcher is not implemented on this platform"

struct FileWatcher::Impl {
    Impl(const std::filesystem::path& path) { }
};
void FileWatcher::PollChanges(std::vector<std::filesystem::path>& changedFiles) {}

#endif

FileWatcher::FileWatcher(const std::filesystem::path& path) {
    _impl = std::make_unique<Impl>(path);
};
FileWatcher::~FileWatcher() = default;


};  // namespace havk