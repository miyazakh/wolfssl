/* PlatformIO wolfssl_benchmark main.c
 *
 * Copyright (C) 2006-2025 wolfSSL Inc.
 *
 * This file is part of wolfSSL.
 *
 * wolfSSL is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * wolfSSL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */
#include "main.h"

#include <wolfssl/wolfcrypt/settings.h>
#ifdef WOLFSSL_ESPIDF
    #include <esp_log.h>
    #include <rtc_wdt.h>
    #include <wolfssl/wolfcrypt/port/Espressif/esp32-crypt.h>
#endif

#include <wolfssl/version.h>
#include <wolfssl/wolfcrypt/types.h>
#include <wolfcrypt/benchmark/benchmark.h>

#define TAG "wolfSSL demo"

void app_main() {
    int ret = 0;
#ifdef WOLFSSL_ESPIDF
    ESP_LOGI(TAG, "Found WOLFSSL_ESPIDF!");
#endif
    printf("\nHello World wolfSSL Version %s\n", LIBWOLFSSL_VERSION_STRING);

#if defined(HAVE_VERSION_EXTENDED_INFO) && defined(WOLFSSL_ESPIDF)
    esp_ShowExtendedSystemInfo();
#endif
    ret =  benchmark_test(NULL);
    printf("\nbenchmark_test complete! result code: %d\n", ret);
}
