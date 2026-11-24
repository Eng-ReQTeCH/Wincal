#pragma once
#include "flyout.h"
#include <xamlom.h>
#include <string>

// A diagnostics callback may arrive after InitializeXamlDiagnosticsEx returns.
// Offer each transfer with a unique token. The receiver must validate the launch
// nonce and atomically claim the offer before adopting any raw handle.
class AttachmentAttempt {
public:
    AttachmentAttempt() = default;
    AttachmentAttempt(const AttachmentAttempt&) = delete;
    AttachmentAttempt& operator=(const AttachmentAttempt&) = delete;
    ~AttachmentAttempt() { Reset(); }

    HRESULT Begin(decltype(&InitializeXamlDiagnosticsEx) initialize, DWORD pid, FlyoutState& shared,
        HANDLE mapping, HANDLE mutex, const wchar_t* dll, unsigned endpoint, LONG64 nextToken, ULONGLONG now) {
        Reset();
        state = &shared;
        token = nextToken;
        process = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
        if (!process) { const auto error = GetLastError(); Reset(); return HRESULT_FROM_WIN32(error); }
        if (!DuplicateHandle(GetCurrentProcess(), mapping, process, &remote[0], FILE_MAP_WRITE, FALSE, 0) ||
            !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), process, &remote[1], SYNCHRONIZE, FALSE, 0) ||
            !DuplicateHandle(GetCurrentProcess(), mutex, process, &remote[2], SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, 0)) {
            const auto error = GetLastError();
            Reset();
            return HRESULT_FROM_WIN32(error);
        }
        const auto data = std::to_wstring(reinterpret_cast<UINT_PTR>(remote[0])) + L" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(remote[1])) + L" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(remote[2])) + L" " + std::to_wstring(token) + L" " +
            std::to_wstring(state->nonce[0]) + L" " + std::to_wstring(state->nonce[1]);
        const auto name = L"VisualDiagConnection" + std::to_wstring(endpoint);
        expires = now + 2000;
        InterlockedExchange64(&state->attachmentOffer, token);
        const auto result = initialize(name.c_str(), pid, nullptr, dll, FlyoutTap, data.c_str());
        if (FAILED(result)) Reset();
        else Poll(now);
        return result;
    }

    void Poll(ULONGLONG now) {
        if (state && (ReadShared64(state->attachmentOffer) != token || now >= expires)) Reset();
    }
    bool Pending() const { return state != nullptr; }

    void Reset() {
        bool claimed = false;
        if (state) {
            const auto previous = InterlockedCompareExchange64(&state->attachmentOffer, 0, token);
            claimed = previous == -token || ReadShared64(state->attachmentAccepted) == token;
        }
        if (process) {
            if (!claimed) for (const auto handle : remote)
                if (handle) DuplicateHandle(process, handle, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
            CloseHandle(process);
        }
        state = nullptr;
        process = nullptr;
        for (auto& handle : remote) handle = nullptr;
    }

private:
    FlyoutState* state{};
    HANDLE process{}, remote[3]{};
    LONG64 token{};
    ULONGLONG expires{};
};
