#include "sentinel/monitoring/client.h"

#include <windows.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <thread>

namespace sentinel::monitoring {
namespace {

constexpr DWORD IO_TIMEOUT_MS = 2000;
constexpr DWORD SERVER_COUNT = 4;
constexpr std::uint32_t MAX_RESPONSE_BYTES = 4 * 1024 * 1024;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~Handle() { if (isValid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (isValid()) CloseHandle(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool isValid() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

struct LocalDeleter {
    void operator()(void* value) const noexcept { if (value) LocalFree(value); }
};
using LocalMemory = std::unique_ptr<void, LocalDeleter>;

[[nodiscard]] std::wstring getUserSid() {
    HANDLE rawToken{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        throw ConnectionError("Cannot read current Windows user token");
    }
    const Handle token(rawToken);
    DWORD size{};
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) {
        throw ConnectionError("Cannot read current Windows user SID");
    }
    LPWSTR text{};
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        throw ConnectionError("Cannot format current Windows user SID");
    }
    const LocalMemory memory(text);
    return text;
}

[[nodiscard]] std::wstring getEndpoint(const std::wstring& channel) {
    if (channel.empty() || channel.size() > 80 || channel.find_first_not_of(
            L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::wstring::npos) {
        throw std::invalid_argument("Invalid monitoring channel");
    }
    return L"Sentinel-" + getUserSid() + L"-" + channel;
}

[[nodiscard]] Handle createEvent() {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.isValid()) throw ConnectionError("Cannot create pipe I/O event");
    return event;
}

[[nodiscard]] bool finishIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD& transferred) {
    const HANDLE events[]{operation.hEvent, stop};
    const DWORD result = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, IO_TIMEOUT_MS);
    if (result == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &operation, &transferred, FALSE)) return true;
    CancelIoEx(pipe, &operation);
    // The OVERLAPPED and buffer must remain alive until cancellation completes.
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

[[nodiscard]] bool transfer(HANDLE pipe, void* data, DWORD size, bool isWrite, HANDLE stop = nullptr) {
    const Handle event = createEvent();
    DWORD position = 0;
    while (position < size) {
        ResetEvent(event.get());
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD transferred{};
        std::byte* buffer = static_cast<std::byte*>(data) + position;
        const BOOL completed = isWrite ? WriteFile(pipe, buffer, size - position, &transferred, &operation) :
            ReadFile(pipe, buffer, size - position, &transferred, &operation);
        if (!completed && (GetLastError() != ERROR_IO_PENDING || !finishIo(pipe, operation, stop, transferred))) return false;
        if (transferred == 0) return false;
        position += transferred;
    }
    return true;
}

[[nodiscard]] bool acceptClient(HANDLE pipe, HANDLE stop) {
    const Handle event = createEvent();
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    if (ConnectNamedPipe(pipe, &operation)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    const HANDLE events[]{event.get(), stop};
    const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    DWORD transferred{};
    if (result == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &operation, &transferred, FALSE)) return true;
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

}  // namespace

struct MonitoringOwner::Impl {
    std::function<MonitoringStatus()> read;
    Handle ownership;
    Handle stop{createEvent()};
    std::array<Handle, SERVER_COUNT> pipes;
    std::vector<std::jthread> servers;

    Impl(std::function<MonitoringStatus()> reader, const std::wstring& channel) : read(std::move(reader)) {
        const std::wstring endpoint = getEndpoint(channel);
        const std::wstring policy = L"D:P(A;;GA;;;" + getUserSid() + L")";
        PSECURITY_DESCRIPTOR rawDescriptor{};
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(policy.c_str(), SDDL_REVISION_1,
                &rawDescriptor, nullptr)) throw ConnectionError("Cannot construct same-user pipe policy");
        const LocalMemory descriptor(rawDescriptor);
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), rawDescriptor, FALSE};
        ownership = Handle(CreateMutexW(&security, FALSE, (L"Global\\" + endpoint).c_str()));
        const DWORD ownershipError = GetLastError();
        if (!ownership.isValid()) throw ConnectionError("Cannot acquire monitoring ownership");
        if (ownershipError == ERROR_ALREADY_EXISTS) throw std::runtime_error("Sentinel monitoring owner is already running");
        for (DWORD index = 0; index < SERVER_COUNT; ++index) {
            pipes[index] = Handle(CreateNamedPipeW((L"\\\\.\\pipe\\" + endpoint).c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (index == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                SERVER_COUNT, 64 * 1024, 0, IO_TIMEOUT_MS, &security));
            if (!pipes[index].isValid()) throw ConnectionError("Cannot create same-user monitoring pipe");
        }
        try {
            for (const Handle& pipe : pipes) servers.emplace_back([this, handle = pipe.get()] { serve(handle); });
        } catch (...) {
            SetEvent(stop.get());
            servers.clear();
            throw;
        }
    }

    ~Impl() { SetEvent(stop.get()); servers.clear(); }

    void serve(HANDLE pipe) {
        while (WaitForSingleObject(stop.get(), 0) == WAIT_TIMEOUT) {
            try {
                if (!acceptClient(pipe, stop.get())) continue;
                std::string response = serializeStatus(read());
                if (response.size() <= MAX_RESPONSE_BYTES) {
                    std::uint32_t size = static_cast<std::uint32_t>(response.size());
                    if (transfer(pipe, &size, sizeof(size), true, stop.get())) {
                        if (transfer(pipe, response.data(), size, true, stop.get())) {
                            std::byte acknowledgement{};
                            static_cast<void>(transfer(pipe, &acknowledgement, 1, false, stop.get()));
                        }
                    }
                }
            } catch (const std::exception&) {
                // A disconnected/broken reader must not stop collection or other readers.
            }
            DisconnectNamedPipe(pipe);
        }
    }
};

MonitoringOwner::MonitoringOwner(std::function<MonitoringStatus()> read, std::wstring channel)
    : impl_(std::make_unique<Impl>(std::move(read), channel)) {}
MonitoringOwner::~MonitoringOwner() = default;

MonitoringClient::MonitoringClient(std::wstring channel) : channel_(std::move(channel)) {}

MonitoringStatus MonitoringClient::read() const {
    const std::wstring name = L"\\\\.\\pipe\\" + getEndpoint(channel_);
    const ULONGLONG deadline = GetTickCount64() + IO_TIMEOUT_MS;
    Handle pipe;
    do {
        pipe = Handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (pipe.isValid()) break;
        if (GetLastError() != ERROR_PIPE_BUSY) throw ConnectionError("Sentinel monitoring owner is unreachable");
        WaitNamedPipeW(name.c_str(), 50);
    } while (GetTickCount64() < deadline);
    if (!pipe.isValid()) throw ConnectionError("Sentinel monitoring owner connection timed out");
    std::uint32_t size{};
    if (!transfer(pipe.get(), &size, sizeof(size), false) || size == 0 || size > MAX_RESPONSE_BYTES) {
        throw ConnectionError("Invalid or interrupted monitoring response");
    }
    std::string response(size, '\0');
    if (!transfer(pipe.get(), response.data(), size, false)) throw ConnectionError("Monitoring response timed out or disconnected");
    std::byte acknowledgement{};
    static_cast<void>(transfer(pipe.get(), &acknowledgement, 1, true));
    try { return deserializeStatus(response); }
    catch (const std::exception& error) { throw ConnectionError(std::string("Invalid monitoring response: ") + error.what()); }
}

std::future<MonitoringStatus> MonitoringClient::readAsync() const {
    return std::async(std::launch::async, [client = *this] { return client.read(); });
}

}  // namespace sentinel::monitoring
