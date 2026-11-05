#include "ProcessRunner.h"
#include <array>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace NLS::Platform::Process
{
namespace
{
#ifdef _WIN32
std::wstring Wide(const std::string& text)
{
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if (!size) throw std::invalid_argument("process argument is not UTF-8");
    std::wstring result(size,0); MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size); return result;
}
std::wstring Quote(const std::wstring& text)
{
    std::wstring result = L"\""; size_t slashes = 0;
    for (auto c : text)
    {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'\"' ? 2 : 1),L'\\'); slashes = 0;
        if (c == L'\"') result += L'\\'; result += c;
    }
    result.append(slashes * 2,L'\\'); return result + L'\"';
}
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
#endif
}
ProcessRunResult Run(const ProcessRunOptions& options)
{
    ProcessRunResult result;
    try
    {
        if (!options.executable.is_absolute() || !std::filesystem::is_regular_file(options.executable)) throw std::invalid_argument("process executable must be an existing absolute file");
        for (const auto& argument : options.arguments) if (argument.find('\0') != std::string::npos) throw std::invalid_argument("process argument contains NUL");
        std::ofstream outputFile;
        if (!options.stdoutFile.empty())
        {
            outputFile.open(options.stdoutFile,std::ios::binary|std::ios::trunc);
            if (!outputFile) throw std::runtime_error("cannot open process output file");
        }
        uint64_t count = 0;
        const auto accept = [&](const char* bytes, size_t size, bool error) {
            if (size > options.maxOutputBytes - count) { result.outputLimitExceeded = true; return; }
            count += size;
            if (!error && outputFile.is_open()) { outputFile.write(bytes,size); if (!outputFile) throw std::runtime_error("process output write failed"); }
            else (error ? result.error : result.output).append(bytes,size);
        };
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        auto nextQuotaCheck = std::chrono::steady_clock::now();
        const auto checkQuota = [&] {
            const auto now = std::chrono::steady_clock::now();
            if (!options.maxDirectoryBytes || now < nextQuotaCheck) return;
            nextQuotaCheck = now + std::chrono::milliseconds(50);
            std::error_code error; uint64_t bytes = 0;
            if (!std::filesystem::exists(options.monitoredDirectory,error)) return;
            for (std::filesystem::recursive_directory_iterator it(options.monitoredDirectory,error), end; it != end; it.increment(error))
            {
                if (error) throw std::runtime_error("cannot monitor package staging quota");
                if (!it->is_regular_file(error)) continue;
                const auto size = it->file_size(error);
                if (error) continue;
                if (size > options.maxDirectoryBytes - bytes) { result.outputLimitExceeded = true; return; }
                bytes += size;
            }
        };
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        Handle readOut, writeOut, readError, writeError, input, job;
        if (!CreatePipe(&readOut.value,&writeOut.value,&security,0) || !CreatePipe(&readError.value,&writeError.value,&security,0)) throw std::runtime_error("cannot create process pipes");
        SetHandleInformation(readOut.value,HANDLE_FLAG_INHERIT,0); SetHandleInformation(readError.value,HANDLE_FLAG_INHERIT,0);
        input.value = CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open process input");
        job.value = CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("cannot create process job");
        std::wstring command = Quote(options.executable.native());
        for (const auto& argument : options.arguments) command += L" " + Quote(Wide(argument));
        std::vector<wchar_t> environment;
        for (const auto& [key,value] : options.environment)
        {
            if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos) throw std::invalid_argument("invalid process environment");
            const auto entry = Wide(key + "=" + value); environment.insert(environment.end(),entry.begin(),entry.end()); environment.push_back(0);
        }
        environment.push_back(0); if (environment.size() == 1) environment.push_back(0);
        SIZE_T attributeSize = 0; InitializeProcThreadAttributeList(nullptr,1,0,&attributeSize);
        std::vector<uint8_t> attributeStorage(attributeSize);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!InitializeProcThreadAttributeList(attributes,1,0,&attributeSize)) throw std::runtime_error("cannot initialise process handle list");
        struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributesGuard() { DeleteProcThreadAttributeList(value); } } attributeGuard{attributes};
        HANDLE inherited[]{input.value,writeOut.value,writeError.value};
        if (!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)) throw std::runtime_error("cannot constrain process handles");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES; startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = writeOut.value; startup.StartupInfo.hStdError = writeError.value;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(options.executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|EXTENDED_STARTUPINFO_PRESENT,
            environment.data(),options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str(),&startup.StartupInfo,&process))
            throw std::runtime_error("CreateProcess failed: " + std::to_string(GetLastError()));
        Handle processHandle{process.hProcess}, threadHandle{process.hThread};
        if (!AssignProcessToJobObject(job.value,process.hProcess)) { TerminateProcess(process.hProcess,1); throw std::runtime_error("cannot isolate child process lifetime"); }
        ResumeThread(process.hThread); CloseHandle(writeOut.value); writeOut.value = nullptr; CloseHandle(writeError.value); writeError.value = nullptr;
        auto drain = [&](HANDLE pipe, bool error) {
            std::array<char,65536> buffer{}; DWORD available = 0, received = 0;
            while (PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr) && available)
            {
                if (!ReadFile(pipe,buffer.data(),(std::min)(available,static_cast<DWORD>(buffer.size())),&received,nullptr) || !received) break;
                accept(buffer.data(),received,error); if (result.outputLimitExceeded) break;
            }
        };
        for (;;)
        {
            drain(readOut.value,false); drain(readError.value,true);
            checkQuota();
            DWORD status = 0; if (!GetExitCodeProcess(process.hProcess,&status)) throw std::runtime_error("cannot query child process");
            if (status != STILL_ACTIVE) { result.exitCode = static_cast<int>(status); drain(readOut.value,false); drain(readError.value,true); break; }
            result.cancelled = options.cancel && options.cancel->load(); result.timedOut = std::chrono::steady_clock::now() >= deadline;
            if (result.cancelled || result.timedOut || result.outputLimitExceeded)
            { TerminateJobObject(job.value,1); WaitForSingleObject(process.hProcess,5000); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#else
        std::vector<std::string> arguments{options.executable.string()}; arguments.insert(arguments.end(),options.arguments.begin(),options.arguments.end());
        std::vector<std::string> environment;
        for (const auto& [key,value] : options.environment) environment.push_back(key + "=" + value);
        std::vector<char*> argv, envp;
        for (auto& arg : arguments) argv.push_back(arg.data()); argv.push_back(nullptr);
        for (auto& env : environment) envp.push_back(env.data()); envp.push_back(nullptr);
        int outPipe[2], errorPipe[2];
        if (pipe(outPipe) != 0) throw std::runtime_error("cannot create child pipe");
        if (pipe(errorPipe) != 0) { close(outPipe[0]); close(outPipe[1]); throw std::runtime_error("cannot create child pipe"); }
        const long maxFd = sysconf(_SC_OPEN_MAX);
        const auto child = fork();
        if (child == 0)
        {
            setpgid(0,0); dup2(outPipe[1],STDOUT_FILENO); dup2(errorPipe[1],STDERR_FILENO);
            const auto nullInput = open("/dev/null",O_RDONLY); if (nullInput >= 0) dup2(nullInput,STDIN_FILENO);
            for (int fd = 3; fd < maxFd; ++fd) close(fd);
            if (!options.workingDirectory.empty() && chdir(options.workingDirectory.c_str()) != 0) _exit(126);
            execve(options.executable.c_str(),argv.data(),envp.data()); _exit(127);
        }
        close(outPipe[1]); close(errorPipe[1]);
        if (child < 0) { close(outPipe[0]); close(errorPipe[0]); throw std::runtime_error("fork failed"); }
        setpgid(child,child); fcntl(outPipe[0],F_SETFL,O_NONBLOCK); fcntl(errorPipe[0],F_SETFL,O_NONBLOCK);
        struct ChildGuard
        {
            pid_t child; int out, error; bool reaped = false;
            ~ChildGuard() { if (!reaped) { kill(-child,SIGKILL); int status; while (waitpid(child,&status,0) < 0 && errno == EINTR) {} } close(out); close(error); }
        } guard{child,outPipe[0],errorPipe[0]};
        auto drain = [&](int pipe, bool error) { std::array<char,65536> buffer{}; ssize_t size; while ((size = read(pipe,buffer.data(),buffer.size())) > 0) { accept(buffer.data(),size,error); if (result.outputLimitExceeded) break; } };
        for (;;)
        {
            drain(outPipe[0],false); drain(errorPipe[0],true); int status = 0;
            checkQuota();
            const auto done = waitpid(child,&status,WNOHANG);
            if (done == child) { guard.reaped = true; result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1; drain(outPipe[0],false); drain(errorPipe[0],true); break; }
            if (done < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
            result.cancelled = options.cancel && options.cancel->load(); result.timedOut = std::chrono::steady_clock::now() >= deadline;
            if (result.cancelled || result.timedOut || result.outputLimitExceeded) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#endif
        if (outputFile.is_open()) { outputFile.flush(); if (!outputFile) throw std::runtime_error("process output flush failed"); }
    }
    catch (const std::exception& e) { result.error += e.what(); result.exitCode = -1; }
    return result;
}
}
