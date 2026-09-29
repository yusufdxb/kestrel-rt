#include "kernel/khist.h"

khist_t g_hist_pendsv = {2u, 0u, 4u, 0u, 0u, 0u, {0u}};
khist_t g_hist_alarm = {3u, 1u, 8u, 0u, 0u, 0u, {0u}};
khist_t g_hist_svc = {2u, 2u, 4u, 0u, 0u, 0u, {0u}};
khist_t g_hist_irqlat = {1u, 3u, 2u, 0u, 0u, 0u, {0u}};
