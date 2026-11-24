#pragma once
#include "agenda.h"

inline constexpr GUID FlyoutTap{0x144ada1e, 0x6065, 0x48b8, {0xa4, 0x45, 0x2e, 0xcb, 0x38, 0x27, 0x18, 0xf5}};
inline constexpr ULONGLONG FlyoutProtocol = 0x57494E43414C3034ULL;

struct FlyoutState {
    ULONGLONG protocol, nonce[2];
    alignas(8) volatile LONG64 attachmentOffer, attachmentAccepted;
    alignas(8) volatile LONG64 window;
    volatile LONG loadedPid;
    volatile LONG stage;
    volatile LONG requestedDay;
    volatile LONG visibleDay;
    volatile LONG agendaRevision;
    volatile LONG error;
    volatile LONG cachedSelection;
    SharedAgenda agenda;
};

inline LONG ReadShared(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
inline LONG64 ReadShared64(volatile LONG64& value) { return InterlockedCompareExchange64(&value, 0, 0); }
inline HWND ReadFlyout(FlyoutState& state) { return reinterpret_cast<HWND>(InterlockedCompareExchange64(&state.window, 0, 0)); }

inline bool ClaimAttachment(FlyoutState& state, LONG64 token, ULONGLONG nonce0, ULONGLONG nonce1) {
    if (token <= 0 || state.protocol != FlyoutProtocol || state.nonce[0] != nonce0 || state.nonce[1] != nonce1 ||
        (!nonce0 && !nonce1) || InterlockedCompareExchange64(&state.attachmentOffer, -token, token) != token) return false;
    // A callback from the previous shell may resume after a new shell has
    // already accepted a later offer. Its acknowledgement must not overwrite
    // the newer receiver's ownership record.
    LONG64 acknowledged = ReadShared64(state.attachmentAccepted);
    while (acknowledged < token) {
        const auto previous = InterlockedCompareExchange64(&state.attachmentAccepted, token, acknowledged);
        if (previous == acknowledged) break;
        acknowledged = previous;
    }
    return true;
}

inline void ReleaseAttachmentClaim(FlyoutState& state, LONG64 token, LONG pid) {
    if (token > 0 && ReadShared64(state.attachmentOffer) == -token) {
        // Clear the PID before making the slot available to another session.
        InterlockedCompareExchange(&state.loadedPid, 0, pid);
        InterlockedCompareExchange64(&state.attachmentOffer, 0, -token);
    }
}

struct AgendaLock {
    HANDLE mutex;
    DWORD result;

    AgendaLock(HANDLE value, DWORD wait) : mutex(value), result(WaitForSingleObject(value, wait)) {}
    ~AgendaLock() { if (OwnsMutex()) ReleaseMutex(mutex); }
    AgendaLock(const AgendaLock&) = delete;
    AgendaLock& operator=(const AgendaLock&) = delete;

    bool OwnsMutex() const { return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED; }
};
