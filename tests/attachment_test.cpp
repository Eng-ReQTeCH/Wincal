#include "../src/attachment.h"
#include <iostream>
#include <thread>
#include <atomic>

namespace {
enum class Mode { Reject, Pending, Claim, ClaimThenReject, ClaimReleaseReject };
Mode mode{};
std::wstring lastData;
HANDLE adopted[3]{};
LONG64 adoptedToken{};
FlyoutState* adoptedView{};
HANDLE sentinel{};

bool Receive(const std::wstring& data) {
    unsigned long long mapping{}, controller{}, mutex{}, nonce0{}, nonce1{};
    long long token{};
    if (swscanf_s(data.c_str(), L"%llu %llu %llu %lld %llu %llu",
        &mapping, &controller, &mutex, &token, &nonce0, &nonce1) != 6) return false;
    const auto handle = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(mapping));
    auto view = static_cast<FlyoutState*>(MapViewOfFile(handle, FILE_MAP_WRITE, 0, 0, sizeof(FlyoutState)));
    if (!view) return false;
    if (!ClaimAttachment(*view, token, nonce0, nonce1)) { UnmapViewOfFile(view); return false; }
    adopted[0] = handle;
    adopted[1] = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(controller));
    adopted[2] = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(mutex));
    adoptedToken = token;
    adoptedView = view;
    return true;
}

void ReleaseReceiver() {
    if (!adoptedView) return;
    ReleaseAttachmentClaim(*adoptedView, adoptedToken, static_cast<LONG>(GetCurrentProcessId()));
    UnmapViewOfFile(adoptedView);
    adoptedView = nullptr;
    for (auto& handle : adopted) { CloseHandle(handle); handle = nullptr; }
}

HRESULT WINAPI Initialize(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR data) {
    lastData = data;
    if (mode == Mode::Reject) return E_ACCESSDENIED;
    if (mode == Mode::Pending) return S_OK;
    if (!Receive(lastData)) return E_FAIL;
    if (mode == Mode::ClaimReleaseReject) {
        ReleaseReceiver();
        sentinel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return E_FAIL;
    }
    return mode == Mode::ClaimThenReject ? E_FAIL : S_OK;
}
}

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { std::cerr << message << '\n'; ++failures; } };
    const auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FlyoutState), nullptr);
    const auto mutex = CreateMutexW(nullptr, FALSE, nullptr);
    auto state = static_cast<FlyoutState*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(FlyoutState)));
    if (!mapping || !mutex || !state) { std::cerr << "Could not create attachment fixture: " << GetLastError() << '\n'; return 1; }
    GUID nonce{};
    const auto nonceResult = CoCreateGuid(&nonce);
    if (FAILED(nonceResult)) { std::cerr << "Could not generate fixture nonce: " << std::hex << nonceResult << '\n'; return 1; }
    state->protocol = FlyoutProtocol;
    memcpy(state->nonce, &nonce, sizeof(nonce));
    LONG64 token = 0;
    const auto begin = [&](AttachmentAttempt& attempt) {
        return attempt.Begin(Initialize, GetCurrentProcessId(), *state, mapping, mutex, L"fixture.dll", 1, ++token, 100);
    };
    {
        AttachmentAttempt attempt;
        // Warm up allocations before measuring OS handle ownership.
        mode = Mode::Reject;
        begin(attempt);
        DWORD before{}, after{};
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int i = 0; i < 100; ++i) check(FAILED(begin(attempt)), "Rejected initialization should fail");
        GetProcessHandleCount(GetCurrentProcess(), &after);
        check(before == after && !attempt.Pending(), "Repeated failed initialization must reclaim every transferred handle");

        mode = Mode::Claim;
        check(SUCCEEDED(begin(attempt)) && !attempt.Pending() && ReadShared64(state->attachmentOffer) == -token,
            "A synchronous receiver should own the transfer and suppress retries before loadedPid is set");
        check(WaitForSingleObject(adopted[1], 0) == WAIT_TIMEOUT, "Claimed controller handle must remain valid");
        ReleaseReceiver();
        check(!ReadShared64(state->attachmentOffer) && !ReadShared(state->loadedPid), "Detach must release the retry gate");

        mode = Mode::ClaimThenReject;
        check(FAILED(begin(attempt)) && WaitForSingleObject(adopted[1], 0) == WAIT_TIMEOUT,
            "A claimed transfer remains receiver-owned even if initialize returns failure");
        ReleaseReceiver();

        mode = Mode::ClaimReleaseReject;
        check(FAILED(begin(attempt)) && WaitForSingleObject(sentinel, 0) == WAIT_TIMEOUT,
            "A receiver that releases before initialize returns must not have its reused handles closed by the caller");
        CloseHandle(sentinel);

        mode = Mode::Pending;
        check(SUCCEEDED(begin(attempt)) && attempt.Pending(), "Asynchronous initialization should retain a bounded pending offer");
        attempt.Poll(2000);
        check(attempt.Pending(), "An unexpired offer must not be reclaimed early");
        check(Receive(lastData), "A delayed receiver should be able to claim before timeout");
        attempt.Poll(2001);
        check(!attempt.Pending() && WaitForSingleObject(adopted[1], 0) == WAIT_TIMEOUT,
            "Polling a delayed accepted offer must preserve its receiver handles");
        ReleaseReceiver();

        begin(attempt);
        const auto expiredData = lastData;
        attempt.Poll(2100);
        check(!attempt.Pending() && !ReadShared64(state->attachmentOffer), "Timed-out unclaimed offers must be revoked");
        check(!Receive(expiredData), "A callback arriving after revocation must not adopt reclaimed handles");
        begin(attempt);
        check(!Receive(expiredData) && ReadShared64(state->attachmentOffer) == token,
            "A stale token must not claim a newer offer with reused handle numbers");
        check(!ClaimAttachment(*state, token, state->nonce[0] ^ 1, state->nonce[1]),
            "A reused mapping with a different launch nonce must not be accepted");
        attempt.Reset();
        std::thread([] {}).join();
        GetProcessHandleCount(GetCurrentProcess(), &before);
        for (int i = 0; i < 100; ++i) {
            mode = Mode::Pending;
            begin(attempt);
            const auto data = lastData;
            std::atomic_bool go{};
            std::thread receiver([&] { go.wait(false); Receive(data); });
            go = true;
            go.notify_one();
            attempt.Reset();
            receiver.join();
            ReleaseReceiver();
            check(!ReadShared64(state->attachmentOffer), "A claim/revocation race must leave no occupied offer after cleanup");
        }
        GetProcessHandleCount(GetCurrentProcess(), &after);
        check(before == after, "Concurrent claims and revocations must neither leak nor double-close OS handles");
        // Model a prior shell resuming its acknowledgement after the replacement
        // shell has already accepted a newer generation.
        InterlockedExchange64(&state->attachmentAccepted, token + 2);
        InterlockedExchange64(&state->attachmentOffer, token + 1);
        check(ClaimAttachment(*state, token + 1, state->nonce[0], state->nonce[1]) &&
            ReadShared64(state->attachmentAccepted) == token + 2,
            "An old shell callback must never overwrite a newer ownership acknowledgement");
        InterlockedExchange64(&state->attachmentOffer, 0);
    }
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    CloseHandle(mutex);
    if (!failures) std::cout << "Attachment ownership, timeout, delayed-callback and retry tests passed.\n";
    return failures != 0;
}
