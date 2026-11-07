#include "Editor/Services/AssetPreprocessor.h"
#include "ZenAsset/Format/Utf8.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cerrno>
#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <spawn.h>
#    include <sys/wait.h>
#    include <signal.h>
#    include <fcntl.h>
#    include <unistd.h>
extern char** environ;
#endif

namespace zen::editor
{
namespace
{
std::string PathText(const std::filesystem::path& path)
{
    const std::u8string value = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool ValidText(const std::string& text)
{
    return !text.empty() && text.find('\0') == std::string::npos && zenasset::IsValidUtf8(text);
}

void JsonString(std::ostream& stream, const std::string& text)
{
    constexpr char hex[] = "0123456789abcdef";
    stream << '"';
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            stream << '\\' << static_cast<char>(c);
        }
        else if (c < 32)
        {
            stream << "\\u00" << hex[c >> 4] << hex[c & 15];
        }
        else
        {
            stream << static_cast<char>(c);
        }
    }
    stream << '"';
}

bool SimpleName(const std::string& name)
{
    bool valid = !name.empty() && name.size() <= 128;
    for (unsigned char c : name)
    {
        valid &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    return valid;
}

constexpr size_t   kLogTailBytes = 64 * 1024;
constexpr uint64_t kLogReadBytes = 4ull << 20;

struct CompilerLog
{
    std::filesystem::path path;
    uint64_t              offset{0};
    std::string           partial; // Bytes after the last complete line.
    std::string           lines;   // Complete lines from the latest read.
    std::string           tail;    // The last kLogTailBytes, shown in the editor.
};

// The compiler admits each job against this budget, so it must not exceed physical memory:
// an oversized model then fails cleanly instead of paging. 48 GiB is the tested ceiling.
uint64_t CompilerMemoryBudget()
{
    uint64_t physical = 0;
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) != 0)
    {
        physical = memory.ullTotalPhys;
    }
#else
    const long pages    = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0)
    {
        physical = uint64_t(pages) * uint64_t(pageSize);
    }
#endif
    constexpr uint64_t ceiling = 48ull << 30;
    return physical == 0 ? ceiling : std::clamp(physical / 4 * 3, uint64_t(1) << 30, ceiling);
}

// A copied or installed editor carries the compiler beside its executable; development
// builds fall back to the compiler target in the build tree.
std::filesystem::path FindCompiler()
{
    const std::filesystem::path built = std::filesystem::u8path(ZEN_ASSET_COMPILER_PATH);
    std::filesystem::path       executable;
#ifdef _WIN32
    HeapVector<wchar_t> buffer(32768);
    const DWORD         length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size())
    {
        executable = std::filesystem::path(std::wstring(buffer.data(), length));
    }
#elif defined(__linux__)
    std::error_code linkError;
    executable = std::filesystem::read_symlink("/proc/self/exe", linkError);
#endif
    std::error_code             error;
    const std::filesystem::path beside =
        executable.empty() ? std::filesystem::path() : executable.parent_path() / built.filename();
    const bool found = !beside.empty() && std::filesystem::is_regular_file(beside, error) && !error;
    return found ? beside : built;
}

// The log is already read into memory once the compiler has exited, so its control directory goes.
void RemoveJob(std::filesystem::path& job)
{
    if (!job.empty())
    {
        std::error_code error;
        std::filesystem::remove_all(job, error);
        job.clear();
    }
}

// Appends this file's output since the previous read. A partial last line waits for the next read.
void ReadCompilerLog(CompilerLog& log, bool finished)
{
    std::ifstream file(log.path, std::ios::binary | std::ios::ate);
    if (file)
    {
        const std::streamoff size = file.tellg();
        if (size > static_cast<std::streamoff>(log.offset))
        {
            if (finished && uint64_t(size) - log.offset > kLogReadBytes)
            {
                // After exit only the visible tail matters; progress is already final.
                log.offset = uint64_t(size) - kLogReadBytes;
                log.partial.clear();
            }
            const uint64_t bytes = std::min<uint64_t>(uint64_t(size) - log.offset, kLogReadBytes);
            const size_t   start = log.partial.size();
            log.partial.resize(start + static_cast<size_t>(bytes));
            file.seekg(static_cast<std::streamoff>(log.offset));
            file.read(log.partial.data() + start, static_cast<std::streamsize>(bytes));
            log.partial.resize(start + static_cast<size_t>(file.gcount()));
            log.offset += static_cast<uint64_t>(file.gcount());
        }
    }
    const size_t end = log.partial.rfind('\n');
    // Progress lines are short; flush an unterminated run so it cannot grow without bound.
    const size_t complete = finished                           ? log.partial.size()
                          : end != std::string::npos           ? end + 1
                          : log.partial.size() > kLogTailBytes ? log.partial.size()
                                                               : 0;
    log.lines.assign(log.partial, 0, complete);
    log.partial.erase(0, complete);
    log.tail += log.lines;
    if (log.tail.size() > kLogTailBytes)
    {
        log.tail.erase(0, log.tail.size() - kLogTailBytes);
    }
}

void ReadBuildProgress(const std::string& log, HeapVector<uint32_t>& steps, std::string& description)
{
    struct Stage
    {
        const char* name;
        const char* label;
        uint32_t    steps;
    };
    constexpr Stage    stages[] = {{"preflight", "Checking source", 0}, {"importing", "Importing", 1},
                                   {"cooking", "Optimizing", 2},        {"writing", "Writing native model", 3},
                                   {"publishing", "Publishing", 4},     {"reused", "Reusing cached model", 4},
                                   {"complete", "Model processed", 4}};
    std::istringstream input(log);
    std::string        line;
    while (std::getline(input, line))
    {
        std::istringstream record(line);
        std::string        marker, stage;
        uint32_t           job = UINT32_MAX;
        if (record >> marker >> job >> stage && marker == "zenasset-progress-v1")
        {
            if (job == UINT32_MAX && stage == "publishing")
            {
                description = "Publishing native scene...";
            }
            else if (job < steps.size())
            {
                for (const Stage& known : stages)
                {
                    if (stage == known.name)
                    {
                        steps[job]  = std::max(steps[job], known.steps);
                        description = std::string(known.label) + " - model " + std::to_string(job + 1) + " of "
                                    + std::to_string(steps.size());
                    }
                }
            }
        }
    }
}

void WriteManifest(std::ostream& stream, const AssetPreprocessor& build, const std::string& directory)
{
    stream << "{\"version\":2,\"outputDirectory\":";
    JsonString(stream, directory);
    stream << ",\"assembly\":";
    JsonString(stream, build.name + ".zenscene");
    stream << ",\"jobs\":[";
    for (size_t index = 0; index < build.sources.size(); ++index)
    {
        stream << (index == 0 ? "" : ",") << "{\"key\":\"model_" << index << "\",\"source\":";
        JsonString(stream, build.sources[index]);
        stream
            << ",\"profile\":\"desktop\",\"compression\":{\"transport\":\"zstd\",\"zstandardLevel\":3,\"meshoptimizer\":true}"
               ",\"processing\":{\"geometry\":{\"optimizeVertexFetch\":true}";
        if (build.compressTextures)
        {
            stream << ",\"textures\":{\"generateMipmaps\":true,\"compressBC7\":true,\"compressBC5Normals\":true,"
                      "\"compressBC4Occlusion\":true,\"bc7Quality\":0}";
        }
        stream << "}}";
    }
    stream << "],\"instances\":[";
    for (size_t index = 0; index < build.sources.size(); ++index)
    {
        stream << (index == 0 ? "" : ",") << "{\"key\":\"instance_" << index << "\",\"model\":\"model_" << index << "\"}";
    }
    stream << "]}\n";
}
} // namespace

struct AssetPreprocessor::State
{
    std::string          message, output;
    std::string          progressText;
    HeapVector<uint32_t> steps;
    CompilerLog          log;
    // Holds the manifest and log of the current run; removed once the compiler has exited.
    std::filesystem::path                 job;
    bool                                  running{false}, succeeded{false};
    std::chrono::steady_clock::time_point nextLogRead;
#ifdef _WIN32
    HANDLE process{nullptr};
#else
    pid_t process{-1};
#endif
};

AssetPreprocessor::AssetPreprocessor() : m_state(MakeUnique<State>()) {}
AssetPreprocessor::~AssetPreprocessor()
{
    Cancel();
}

bool AssetPreprocessor::AddSource(const std::string& path)
{
    std::error_code       error;
    bool                  valid = !IsRunning() && sources.size() < 1024 && ValidText(path);
    std::filesystem::path resolved;
    if (valid)
    {
        resolved              = std::filesystem::absolute(std::filesystem::u8path(path), error).lexically_normal();
        std::string extension = resolved.extension().string();
        for (char& c : extension)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        valid = !error && std::filesystem::is_regular_file(resolved, error) && !error
             && (extension == ".gltf" || extension == ".glb" || extension == ".fbx" || extension == ".obj");
    }
    if (valid)
    {
        const std::string source = PathText(resolved);
        valid                    = std::find(sources.begin(), sources.end(), source) == sources.end();
        if (valid)
        {
            sources.push_back(source);
            m_state->message.clear();
        }
        else
        {
            m_state->message = "This model is already in the list.";
        }
    }
    else
    {
        m_state->message = "Choose an existing glTF, GLB, FBX or OBJ model.";
    }
    return valid;
}

bool AssetPreprocessor::Start()
{
    const bool idle = !IsRunning();
    if (idle)
    {
        m_state->steps.clear();
        m_state->progressText.clear();
    }
    bool valid = idle && !sources.empty() && SimpleName(name) && ValidText(outputDirectory) && workers > 0 && workers <= 64;
    std::error_code       error;
    std::filesystem::path output, job, manifest;
    if (valid)
    {
        output = std::filesystem::absolute(std::filesystem::u8path(outputDirectory), error).lexically_normal();
        valid  = !error;
    }
    if (valid)
    {
        // A unique control directory keeps manifests/logs separate from published assets.
        const int64_t               stamp     = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
        job                                   = temporary / ("zenasset-job-" + std::to_string(stamp));
        valid                                 = !error && std::filesystem::create_directory(job, error) && !error;
        manifest                              = job / "build.json";
        if (valid)
        {
            RemoveJob(m_state->job);
            m_state->job = job;
            m_state->log = CompilerLog{job / "compiler.log"};
        }
    }
    if (valid)
    {
        std::ofstream file(manifest, std::ios::binary);
        WriteManifest(file, *this, PathText(output));
        file.close();
        valid = bool(file);
    }
    const std::filesystem::path compiler = FindCompiler();
    if (valid)
    {
        valid = std::filesystem::is_regular_file(compiler, error) && !error;
    }
    if (valid)
    {
        m_state->succeeded       = false;
        m_state->output          = PathText(output / (name + ".zenscene"));
        m_state->steps           = HeapVector<uint32_t>(sources.size(), 0);
        m_state->progressText    = "Starting model processing...";
        m_state->nextLogRead     = {};
        const std::string budget = std::to_string(CompilerMemoryBudget());
#ifdef _WIN32
        // Paths are generated/validated; neither may contain a quote on Windows.
        const std::wstring  wideBudget(budget.begin(), budget.end());
        const std::wstring  command = L"\"" + compiler.wstring() + L"\" build \"" + manifest.wstring() + L"\" --import-workers "
                                    + std::to_wstring(workers) + L" --job-memory " + wideBudget + L" --memory-budget "
                                    + wideBudget + L" --progress on";
        HeapVector<wchar_t> arguments(command.data(), command.size());
        arguments.push_back(L'\0');
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE              log = CreateFileW(m_state->log.path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW        startup{};
        startup.cb         = sizeof(startup);
        startup.dwFlags    = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log;
        PROCESS_INFORMATION process{};
        valid = log != INVALID_HANDLE_VALUE
             && CreateProcessW(compiler.c_str(), arguments.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                               &startup, &process)
                    != 0;
        if (log != INVALID_HANDLE_VALUE)
        {
            CloseHandle(log);
        }
        if (valid)
        {
            CloseHandle(process.hThread);
            m_state->process = process.hProcess;
        }
#else
        std::string                executable = compiler.string(), input = manifest.string(), count = std::to_string(workers);
        std::string                jobBudget = budget, reservationBudget = budget;
        char*                      arguments[] = {executable.data(),
                                                  const_cast<char*>("build"),
                                                  input.data(),
                                                  const_cast<char*>("--import-workers"),
                                                  count.data(),
                                                  const_cast<char*>("--job-memory"),
                                                  jobBudget.data(),
                                                  const_cast<char*>("--memory-budget"),
                                                  reservationBudget.data(),
                                                  const_cast<char*>("--progress"),
                                                  const_cast<char*>("on"),
                                                  nullptr};
        posix_spawn_file_actions_t actions;
        const int                  initialized = posix_spawn_file_actions_init(&actions);
        valid                                  = initialized == 0;
        if (valid)
        {
            valid = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, m_state->log.path.c_str(),
                                                     O_WRONLY | O_CREAT | O_EXCL, 0600)
                     == 0
                 && posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO) == 0
                 && posix_spawn(&m_state->process, executable.c_str(), &actions, nullptr, arguments, environ) == 0;
            posix_spawn_file_actions_destroy(&actions);
        }
#endif
    }
    if (idle)
    {
        m_state->running   = valid;
        m_state->succeeded = false;
        if (!valid)
        {
            m_state->progressText = "Could not start processing";
            RemoveJob(m_state->job);
        }
        m_state->message =
            valid ? "Processing models. You can keep working in the editor."
                  : "Could not start. Check the source list, output directory, simple scene name and compiler installation.";
    }
    return valid;
}

void AssetPreprocessor::Poll()
{
    if (m_state->running)
    {
        bool complete  = false;
        bool succeeded = false;
#ifdef _WIN32
        // Exit code 259 equals STILL_ACTIVE, so completion comes from the process handle.
        DWORD code = 1;
        complete   = WaitForSingleObject(m_state->process, 0) == WAIT_OBJECT_0;
        succeeded  = complete && GetExitCodeProcess(m_state->process, &code) != 0 && code == 0;
        if (complete)
        {
            CloseHandle(m_state->process);
            m_state->process = nullptr;
        }
#else
        int status = 0;
        complete   = waitpid(m_state->process, &status, WNOHANG) == m_state->process;
        succeeded  = complete && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
        if (complete)
        {
            m_state->running   = false;
            m_state->succeeded = succeeded;
            m_state->message   = succeeded ? "Processing complete. The native scene is ready to open."
                                           : "Processing failed. See the compiler log.";
        }
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (complete || now >= m_state->nextLogRead)
        {
            ReadCompilerLog(m_state->log, complete);
            ReadBuildProgress(m_state->log.lines, m_state->steps, m_state->progressText);
            if (complete)
            {
                m_state->progressText = succeeded ? "Complete" : "Processing failed";
                RemoveJob(m_state->job);
            }
            m_state->nextLogRead = now + std::chrono::milliseconds(500);
        }
    }
}

void AssetPreprocessor::Cancel()
{
    Poll();
    if (m_state->running)
    {
        bool stopped = false;
#ifdef _WIN32
        stopped = TerminateProcess(m_state->process, 1) != 0 && WaitForSingleObject(m_state->process, 5000) == WAIT_OBJECT_0;
        if (stopped)
        {
            CloseHandle(m_state->process);
            m_state->process = nullptr;
        }
#else
        if (kill(m_state->process, SIGKILL) == 0 || errno == ESRCH)
        {
            int   status = 0;
            pid_t waited = -1;
            do
            {
                waited = waitpid(m_state->process, &status, 0);
            } while (waited < 0 && errno == EINTR);
            stopped = waited == m_state->process || (waited < 0 && errno == ECHILD);
        }
#endif
        if (!stopped)
        {
            Poll();
        }
        if (stopped)
        {
            m_state->running   = false;
            m_state->succeeded = false;
            ReadCompilerLog(m_state->log, true);
            RemoveJob(m_state->job);
            m_state->message      = "Processing cancelled. Completed publications remain available.";
            m_state->progressText = "Cancelled";
        }
        else if (m_state->running)
        {
            m_state->message = "The compiler could not be stopped. It is still running.";
        }
    }
}

bool AssetPreprocessor::IsRunning() const
{
    return m_state->running;
}
bool AssetPreprocessor::Succeeded() const
{
    return m_state->succeeded;
}
const std::string& AssetPreprocessor::GetStatusText() const
{
    return m_state->message;
}
const std::string& AssetPreprocessor::GetLog() const
{
    return m_state->log.tail;
}
const std::string& AssetPreprocessor::GetOutput() const
{
    return m_state->output;
}

float AssetPreprocessor::GetProgress() const
{
    uint32_t completed = 0;
    for (uint32_t step : m_state->steps)
    {
        completed += step;
    }
    // The final unit is successful publication and compiler exit, never a timer.
    return m_state->succeeded ? 1.0f : float(completed) / float(m_state->steps.size() * 4 + 1);
}

const std::string& AssetPreprocessor::GetProgressText() const
{
    return m_state->progressText;
}
} // namespace zen::editor
