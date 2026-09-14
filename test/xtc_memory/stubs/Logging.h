#pragma once

template <typename... Args>
inline void xtcMemoryTestLog(Args...) {}

#define LOG_ERR(...) xtcMemoryTestLog(__VA_ARGS__)
#define LOG_INF(...) xtcMemoryTestLog(__VA_ARGS__)
#define LOG_DBG(...) xtcMemoryTestLog(__VA_ARGS__)
