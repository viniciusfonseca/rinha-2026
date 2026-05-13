#ifndef RINHA_TIME_UTILS_H
#define RINHA_TIME_UTILS_H

#include <stdint.h>

int rinha_weekday_monday0(int year, int month, int day);
int64_t rinha_epoch_minutes_utc(int year, int month, int day, int hour, int minute, int second);

#endif
