#pragma once
#include "agenda.h"

inline constexpr GUID FlyoutTap{0x144ada1e, 0x6065, 0x48b8, {0xa4, 0x45, 0x2e, 0xcb, 0x38, 0x27, 0x18, 0xf5}};

struct FlyoutState {
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
inline HWND ReadFlyout(FlyoutState& state) { return reinterpret_cast<HWND>(InterlockedCompareExchange64(&state.window, 0, 0)); }

struct AgendaLock {
    HANDLE mutex;
    DWORD result;

    AgendaLock(HANDLE value, DWORD wait) : mutex(value), result(WaitForSingleObject(value, wait)) {}
    ~AgendaLock() { if (OwnsMutex()) ReleaseMutex(mutex); }
    AgendaLock(const AgendaLock&) = delete;
    AgendaLock& operator=(const AgendaLock&) = delete;

    bool OwnsMutex() const { return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED; }
};
