#pragma once
#include "ZenFoundation/Utils/Errors.h"

#define LOGT              ZEN_LOG_TRACE
#define LOGI              ZEN_LOG_INFO
#define LOGD              ZEN_LOG_DEBUG
#define LOGE              ZEN_LOG_ERROR
#define LOGW              ZEN_LOG_WARN
#define VERIFY_EXPR       ZEN_VERIFY
#define VERIFY_EXPR_MSG   ZEN_VERIFY_MSG
#define VERIFY_EXPR_MSG_F ZEN_VERIFY_MSG_F
#define ASSERT            ZEN_ASSERT
#define ASSERT_SIZEOF     ZEN_ASSERT_SIZEOF
#define ASSERT_SIZEOF64   ZEN_ASSERT_SIZEOF64
#define LOG_ERROR         ZEN_REPORT_ERROR
#define LOG_FATAL_ERROR   ZEN_REPORT_FATAL_ERROR
#define LOG_ERROR_ONCE    ZEN_REPORT_ERROR_ONCE

#define CHECK_VK_ERROR(err, ...)                                                             \
    {                                                                                        \
        if (err != VK_SUCCESS)                                                               \
            zen::LogError(/*IsFatal=*/false, __FUNCTION__, __FILE__, __LINE__, __VA_ARGS__); \
    }
