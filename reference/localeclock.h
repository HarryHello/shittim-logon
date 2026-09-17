#pragma once

#include <windows.h>

namespace localeclock {

inline void Format(wchar_t *time, int timeCap, wchar_t *date, int dateCap,
                   bool showSeconds = false, bool force24 = false,
                   void (*log)(const wchar_t *, ...) = nullptr) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    if (time && timeCap > 0) {
        time[0] = L'\0';

        const wchar_t *picture = force24 ? (showSeconds ? L"HH:mm:ss" : L"HH:mm") : nullptr;
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,
                             (picture || showSeconds) ? 0 : TIME_NOSECONDS,
                             &st, picture, time, timeCap)) {

            wsprintfW(time, L"%02d:%02d", st.wHour, st.wMinute);
            if (log) log(L"clock: GetTimeFormatEx failed (%lu) -- 24-hour fallback",
                         GetLastError());
        }
    }

    if (date && dateCap > 0) {
        date[0] = L'\0';

        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, nullptr,
                             date, dateCap, nullptr)) {
            wsprintfW(date, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            if (log) log(L"clock: GetDateFormatEx failed (%lu) -- ISO fallback",
                         GetLastError());
        }
    }
}

}
