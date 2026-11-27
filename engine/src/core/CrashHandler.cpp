#include <myengine/core/CrashHandler.h>

#include <myengine/core/AppPaths.h>
#include <myengine/core/Version.h>

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <random>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>

namespace myengine::core::crash
{
    namespace
    {
        constexpr const char* kMarkerName = "pending.txt";   // the path of the text of the last crash; removed when the game has told about it
        constexpr int kLogLines = 40;
        constexpr DWORD kLogBytes = 12 * 1024;

        // Everything the filter needs is prepared in Install: in a crashed process nothing may allocate more than it has to
        wchar_t g_directory[MAX_PATH]{};
        wchar_t g_logFile[MAX_PATH]{};
        wchar_t g_productName[128] = L"Rocket Arena";
        volatile LONG g_entered = 0;
        // LR5: the id of the report of this run, and the context of the crash (SetScene, SetLastAction)
        char g_reportId[17]{};
        char g_scene[256] = "(none yet)";
        char g_lastAction[256] = "(none yet)";
        DWORD g_mainThread = 0;

        using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                                  PMINIDUMP_CALLBACK_INFORMATION);

        // CreateDirectory makes one level: the folder of the game may not exist yet when the very first start crashes
        void MakeDirectories(const wchar_t* path)
        {
            wchar_t partial[MAX_PATH]{};
            for (size_t i = 0; path[i] != 0 && i + 1 < MAX_PATH; ++i)
            {
                partial[i] = path[i];
                if (path[i] == L'\\' && i > 2)
                {
                    CreateDirectoryW(partial, nullptr);
                }
            }
            CreateDirectoryW(path, nullptr);
        }

        void CopyPath(wchar_t* target, const std::filesystem::path& source)
        {
            wcsncpy_s(target, MAX_PATH, source.c_str(), _TRUNCATE);
        }

        // the marker is read as UTF-8 (the path may have letters of the name of the user)
        void ToUtf8(const wchar_t* text, char* out, const int size)
        {
            if (WideCharToMultiByte(CP_UTF8, 0, text, -1, out, size, nullptr, nullptr) == 0)
            {
                out[0] = 0;
            }
        }

        const char* ExceptionName(const DWORD code)
        {
            switch (code)
            {
            case EXCEPTION_ACCESS_VIOLATION: return "access violation";
            case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
            case EXCEPTION_IN_PAGE_ERROR: return "page error";
            case EXCEPTION_BREAKPOINT: return "breakpoint";
            case 0xE06D7363: return "unhandled C++ exception";
            case 0xE0000001: return "std::terminate / abort";
            case 0xE0000002: return "invalid CRT parameter or pure virtual call";
            default: return "exception";
            }
        }

        // "writing the address 0x0000000000000000" for an access violation (the first two parameters of the record say read / write / execute and where)
        void DescribeAccess(const EXCEPTION_RECORD* record, char* out, const size_t size)
        {
            out[0] = 0;
            if (record == nullptr || (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && record->ExceptionCode != EXCEPTION_IN_PAGE_ERROR) ||
                record->NumberParameters < 2)
            {
                return;
            }
            const ULONG_PTR kind = record->ExceptionInformation[0];
            const char* verb = kind == 0 ? "reading" : (kind == 1 ? "writing" : (kind == 8 ? "executing" : "accessing"));
            std::snprintf(out, size, " (%s the address 0x%016llx)", verb, static_cast<unsigned long long>(record->ExceptionInformation[1]));
        }

        // The copy of the log of the run (myengine.log is truncated by the next start) with one line at its end that ties it to the report
        void CopyLog(const wchar_t* target, const char* lastLine, const int lastLength)
        {
            if (g_logFile[0] == 0 || !CopyFileW(g_logFile, target, FALSE))
            {
                return;
            }
            HANDLE file = CreateFileW(target, FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(file, lastLine, lastLength > 0 ? static_cast<DWORD>(lastLength) : 0, &written, nullptr);
                CloseHandle(file);
            }
        }

        // "RocketArena.exe+0x1a2b3c" for the address, the raw address when it is in no module
        void DescribeAddress(const void* address, char* out, const size_t size)
        {
            HMODULE module = nullptr;
            if (address != nullptr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                         static_cast<LPCWSTR>(address), &module))
            {
                char name[MAX_PATH]{};
                GetModuleFileNameA(module, name, MAX_PATH);
                const char* shortName = std::strrchr(name, '\\');
                shortName = shortName != nullptr ? shortName + 1 : name;
                std::snprintf(out, size, "%s+0x%llx", shortName, static_cast<unsigned long long>(reinterpret_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
                return;
            }
            std::snprintf(out, size, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)));
        }

        // The last `kLogLines` lines of the log of this run (the next run truncates the file)
        void AppendLogTail(HANDLE out)
        {
            HANDLE log = CreateFileW(g_logFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (log == INVALID_HANDLE_VALUE)
            {
                return;
            }
            static char buffer[kLogBytes + 1];
            LARGE_INTEGER size{};
            GetFileSizeEx(log, &size);
            LARGE_INTEGER from{};
            from.QuadPart = size.QuadPart > kLogBytes ? size.QuadPart - kLogBytes : 0;
            SetFilePointerEx(log, from, nullptr, FILE_BEGIN);
            DWORD read = 0;
            if (!ReadFile(log, buffer, kLogBytes, &read, nullptr))
            {
                read = 0;
            }
            CloseHandle(log);
            buffer[read] = 0;

            // from the end: kLogLines line breaks back (the first line of a cut buffer is partial: it is dropped with the rest)
            const char* begin = buffer;
            int lines = 0;
            for (const char* p = buffer + read; p > buffer; --p)
            {
                if (p[-1] == '\n' && p != buffer + read && ++lines == kLogLines)
                {
                    begin = p;
                    break;
                }
            }
            if (begin == buffer && from.QuadPart > 0)
            {
                const char* firstBreak = std::strchr(buffer, '\n');
                begin = firstBreak != nullptr ? firstBreak + 1 : buffer;
            }
            DWORD written = 0;
            WriteFile(out, begin, static_cast<DWORD>(buffer + read - begin), &written, nullptr);
        }

        LONG WriteReport(EXCEPTION_POINTERS* pointers)
        {
            if (InterlockedExchange(&g_entered, 1) != 0)
            {
                return EXCEPTION_EXECUTE_HANDLER;   // a crash inside the handler: the process just ends
            }
            if (g_directory[0] == 0)
            {
                return EXCEPTION_EXECUTE_HANDLER;
            }

            MakeDirectories(g_directory);
            SYSTEMTIME now{};
            GetLocalTime(&now);
            SYSTEMTIME utc{};
            GetSystemTime(&utc);
            // crash-<date>-<time>-<reportId>: the name sorts by time, and two crashes never share it (the reportId is new in every run)
            wchar_t stem[MAX_PATH]{};
            _snwprintf_s(stem, _TRUNCATE, L"%s\\crash-%04d%02d%02d-%02d%02d%02d-%S", g_directory, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                         g_reportId);
            wchar_t dumpPath[MAX_PATH]{};
            wchar_t textPath[MAX_PATH]{};
            wchar_t logCopyPath[MAX_PATH]{};
            _snwprintf_s(dumpPath, _TRUNCATE, L"%s.dmp", stem);
            _snwprintf_s(textPath, _TRUNCATE, L"%s.txt", stem);
            _snwprintf_s(logCopyPath, _TRUNCATE, L"%s.log", stem);
            const DWORD thread = GetCurrentThreadId();

            // the dump: dbghelp from System32 (loaded now, not linked: a game that never crashes does not carry it)
            bool dumped = false;
            if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); dbghelp != nullptr)
            {
                const auto write = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
                HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (write != nullptr && file != INVALID_HANDLE_VALUE)
                {
                    MINIDUMP_EXCEPTION_INFORMATION info{};
                    info.ThreadId = thread;   // the filter runs on the thread that crashed: the dump marks it as the one of the exception
                    info.ExceptionPointers = pointers;
                    info.ClientPointers = FALSE;
                    // the stacks of all threads, the modules, the exception: some hundred KB (with the data segments of all the libraries the dump of this game is 40 MB)
                    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
                    dumped = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, pointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
                }
                if (file != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(file);
                }
            }

            const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
            const DWORD code = record != nullptr ? record->ExceptionCode : 0;
            const void* address = record != nullptr ? record->ExceptionAddress : nullptr;
            char where[MAX_PATH + 40]{};
            DescribeAddress(address, where, sizeof(where));
            char access[96]{};
            DescribeAccess(record, access, sizeof(access));

            // the log of the run next to the dump, with a last line that names the report
            static char logLine[512];
            const int logLineLength = std::snprintf(logLine, sizeof(logLine), "[CRASH] reportId=%s buildId=%s: %s 0x%08lX at %s%s\r\n", g_reportId,
                                                    MYENGINE_BUILD_ID, ExceptionName(code), static_cast<unsigned long>(code), where, access);
            CopyLog(logCopyPath, logLine, logLineLength);

            // what can be read without a debugger (LR5: reportId, the time and buildId are here before the dump is opened)
            static char head[4096];
            const int headLength = std::snprintf(
                head, sizeof(head),
                "%S crash report\r\n"
                "reportId: %s\r\n"
                "time: %04d-%02d-%02d %02d:%02d:%02d (local), %04d-%02d-%02dT%02d:%02d:%02dZ (UTC)\r\n"
                "buildId: %s\r\n"
                "version: %s\r\n"
                "commit: %s\r\n"
                "scene: %s\r\n"
                "last action: %s\r\n"
                "exception: %s 0x%08lX at %s%s\r\n"
                "thread: %lu (%s)\r\n"
                "process: %lu\r\n"
                "dump: %s\r\n"
                "log: the .log next to this file (the whole log of the run)\r\n"
                "\r\nThe end of the log:\r\n",
                g_productName, g_reportId, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                utc.wMinute, utc.wSecond, MYENGINE_BUILD_ID, MYENGINE_VERSION_STRING, MYENGINE_GIT_COMMIT_FULL, g_scene, g_lastAction, ExceptionName(code),
                static_cast<unsigned long>(code), where, access, static_cast<unsigned long>(thread), thread == g_mainThread ? "the main thread" : "not the main thread",
                static_cast<unsigned long>(GetCurrentProcessId()), dumped ? "written (the .dmp next to this file)" : "NOT written");
            HANDLE text = CreateFileW(textPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (text != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(text, head, headLength > 0 ? static_cast<DWORD>(std::min(headLength, static_cast<int>(sizeof(head)) - 1)) : 0, &written, nullptr);
                AppendLogTail(text);
                CloseHandle(text);

                // the marker is the last thing written: the next start tells about this crash only if the report is complete
                wchar_t markerPath[MAX_PATH]{};
                _snwprintf_s(markerPath, _TRUNCATE, L"%s\\%S", g_directory, kMarkerName);
                if (HANDLE marker = CreateFileW(markerPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); marker != INVALID_HANDLE_VALUE)
                {
                    char utf8Path[MAX_PATH * 4]{};
                    ToUtf8(textPath, utf8Path, sizeof(utf8Path));
                    char line[MAX_PATH * 4 + 256]{};
                    const int length = std::snprintf(line, sizeof(line), "%s\n%s 0x%08lX at %s\n", utf8Path, ExceptionName(code), static_cast<unsigned long>(code), where);
                    WriteFile(marker, line, length > 0 ? static_cast<DWORD>(length) : 0, &written, nullptr);
                    CloseHandle(marker);
                }
            }

            wchar_t message[MAX_PATH * 2 + 256]{};
            if (GetEnvironmentVariableW(L"MYENGINE_CRASH_NO_DIALOG", message, 8) == 0)
            {
                _snwprintf_s(message, _TRUNCATE,
                             L"%s has stopped working.\n\nReport %S (build %S) was saved to:\n%s\n\nPlease send its three files (.dmp, .txt, .log) to the person who gave you the game.",
                             g_productName, g_reportId, MYENGINE_BUILD_ID, stem);
                MessageBoxW(nullptr, message, g_productName, MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
            }
            return EXCEPTION_EXECUTE_HANDLER;
        }

        LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* pointers)
        {
            return WriteReport(pointers);
        }

        // std::terminate, abort(), a pure virtual call and a bad parameter of the CRT do not pass through the filter in a Release build:
        // they raise a made-up exception of their own with the context of the place they were called from
        [[noreturn]] void RaiseSynthetic(const DWORD code)
        {
            CONTEXT context{};
            RtlCaptureContext(&context);
            EXCEPTION_RECORD record{};
            record.ExceptionCode = code;
            record.ExceptionAddress = _ReturnAddress();
            EXCEPTION_POINTERS pointers{&record, &context};
            WriteReport(&pointers);
            TerminateProcess(GetCurrentProcess(), code);
            std::abort();
        }

        void TerminateHandler() { RaiseSynthetic(0xE0000001); }
        void AbortSignal(int) { RaiseSynthetic(0xE0000001); }
        void PureCall() { RaiseSynthetic(0xE0000002); }
        void InvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) { RaiseSynthetic(0xE0000002); }

        void SetHandlers()
        {
            SetUnhandledExceptionFilter(&UnhandledFilter);
            std::set_terminate(&TerminateHandler);
            std::signal(SIGABRT, &AbortSignal);
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);   // no second dialog of the CRT after ours
            _set_purecall_handler(&PureCall);
            _set_invalid_parameter_handler(&InvalidParameter);
        }

        bool EnvironmentIs(const wchar_t* name, const wchar_t expected)
        {
            wchar_t value[8]{};
            return GetEnvironmentVariableW(name, value, 8) != 0 && value[0] == expected;
        }

        void CrashNow()
        {
            volatile int* nothing = nullptr;
#pragma warning(suppress : 6011)  // on purpose: the crash that the crash handler is tested with
            *nothing = 1;
        }
    }

    void Install(const std::filesystem::path& crashDirectory, const std::filesystem::path& logFile, const wchar_t* productName)
    {
        CopyPath(g_directory, crashDirectory);
        CopyPath(g_logFile, logFile);
        wcsncpy_s(g_productName, productName != nullptr ? productName : L"myengine", _TRUNCATE);
        g_mainThread = GetCurrentThreadId();
        // the reportId: 64 random bits (std::random_device is rand_s on Windows), made now: the crashed process does not have to
        std::random_device random;
        const unsigned long long id = (static_cast<unsigned long long>(random()) << 32) | random();
        std::snprintf(g_reportId, sizeof(g_reportId), "%016llx", id);
        SetHandlers();
    }

    std::string ReportId()
    {
        return g_reportId;
    }

    void SetScene(const std::string_view scene)
    {
        const size_t length = std::min(scene.size(), sizeof(g_scene) - 1);
        std::memcpy(g_scene, scene.data(), length);
        g_scene[length] = 0;
    }

    void SetLastAction(const std::string_view action)
    {
        const size_t length = std::min(action.size(), sizeof(g_lastAction) - 1);
        std::memcpy(g_lastAction, action.data(), length);
        g_lastAction[length] = 0;
    }

    void Reassert()
    {
        if (g_directory[0] == 0)
        {
            return;
        }
        if (SetUnhandledExceptionFilter(&UnhandledFilter) != &UnhandledFilter)
        {
            SetHandlers();   // somebody (the Steam client library) took them: all of them are put back
        }
        static ULONGLONG first = 0;
        if (first == 0)
        {
            first = GetTickCount64();
        }
        else if (GetTickCount64() - first > 10000 && EnvironmentIs(L"MYENGINE_CRASH_TEST", L'2'))
        {
            CrashNow();
        }
    }

    std::optional<Report> TakePending(const std::filesystem::path& crashDirectory)
    {
        const std::filesystem::path marker = crashDirectory / kMarkerName;
        std::error_code error;
        if (!std::filesystem::exists(marker, error))
        {
            return std::nullopt;
        }

        Report report;
        {
            std::ifstream stream(marker, std::ios::binary);
            std::string line;
            std::getline(stream, line);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            report.text = std::filesystem::u8path(line);
            std::getline(stream, report.summary);
        }
        std::filesystem::remove(marker, error);
        if (report.text.empty())
        {
            return std::nullopt;
        }
        report.dump = report.text;
        report.dump.replace_extension(".dmp");
        return report;
    }

    std::string PlayerNote(const Report& report, const std::filesystem::path& crashDirectory)
    {
        const std::string folder = apppaths::IsShipping()
            ? "%LOCALAPPDATA%\\" + crashDirectory.parent_path().filename().u8string() + "\\" + crashDirectory.filename().u8string()
            : report.text.parent_path().generic_u8string();
        return "The game crashed last time. Send " + report.text.stem().generic_u8string() + ".dmp / .txt / .log from " + folder;
    }

    void CrashIfRequested()
    {
        if (EnvironmentIs(L"MYENGINE_CRASH_TEST", L'1'))
        {
            CrashNow();
        }
    }
}
