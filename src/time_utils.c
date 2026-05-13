#include "time_utils.h"

int rinha_weekday_monday0(int year, int month, int day) {
    static const int month_offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year -= 1;
    }
    int weekday_sunday0 = (year + year / 4 - year / 100 + year / 400 + month_offsets[month - 1] + day) % 7;
    if (weekday_sunday0 == 0) {
        return 6;
    }
    return weekday_sunday0 - 1;
}

static int64_t rinha_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned) (year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? (unsigned) -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t) era * 146097 + (int64_t) doe - 719468;
}

int64_t rinha_epoch_minutes_utc(int year, int month, int day, int hour, int minute, int second) {
    int64_t days = rinha_days_from_civil(year, (unsigned) month, (unsigned) day);
    int64_t total_seconds = days * 86400 + (int64_t) hour * 3600 + (int64_t) minute * 60 + second;
    return total_seconds / 60;
}
