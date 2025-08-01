/* module_hooks.c -- module load/unload hooks for libwolfssl.ko
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

#define WOLFSSL_LINUXKM_NEED_LINUX_CURRENT

#include <wolfssl/wolfcrypt/libwolfssl_sources.h>

#ifndef WOLFSSL_LICENSE
    #define WOLFSSL_LICENSE "GPL"
#endif

#ifdef WOLFCRYPT_ONLY
    #include <wolfssl/version.h>
#else
    #include <wolfssl/ssl.h>
#endif
#ifdef HAVE_FIPS
    #ifdef USE_CONTESTMUTEX
        #error USE_CONTESTMUTEX is incompatible with WOLFSSL_LINUXKM
    #endif
    #include <wolfssl/wolfcrypt/fips_test.h>
#endif
#if !defined(NO_CRYPT_TEST) || defined(LINUXKM_LKCAPI_REGISTER)
    #include <wolfcrypt/test/test.h>
#endif
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/sha256.h>

#ifdef WOLFSSL_DEBUG_TRACE_ERROR_CODES
    enum linux_errcodes {
        my_EINVAL = EINVAL,
        my_ENOMEM = ENOMEM,
        my_EBADMSG = EBADMSG
    };

    #undef EINVAL
    #undef ENOMEM
    #undef EBADMSG

    #define EINVAL WC_ERR_TRACE(my_EINVAL)
    #define ENOMEM WC_ERR_TRACE(my_ENOMEM)
    #define EBADMSG WC_ERR_TRACE(my_EBADMSG)
#endif

static int libwolfssl_cleanup(void) {
    int ret;
#ifdef WOLFCRYPT_ONLY
    ret = wolfCrypt_Cleanup();
    if (ret != 0)
        pr_err("ERROR: wolfCrypt_Cleanup() failed: %s\n", wc_GetErrorString(ret));
    else
        pr_info("wolfCrypt " LIBWOLFSSL_VERSION_STRING " cleanup complete.\n");
#else
    ret = wolfSSL_Cleanup();
    if (ret != WOLFSSL_SUCCESS)
        pr_err("ERROR: wolfSSL_Cleanup() failed: %s\n", wc_GetErrorString(ret));
    else
        pr_info("wolfSSL " LIBWOLFSSL_VERSION_STRING " cleanup complete.\n");
#endif

    return ret;
}

#ifdef HAVE_LINUXKM_PIE_SUPPORT

#ifdef DEBUG_LINUXKM_PIE_SUPPORT

extern int wolfCrypt_PIE_first_function(void);
extern int wolfCrypt_PIE_last_function(void);
extern const unsigned int wolfCrypt_PIE_rodata_start[];
extern const unsigned int wolfCrypt_PIE_rodata_end[];

/* cheap portable ad-hoc hash function to confirm bitwise stability of the PIE
 * binary image.
 */
static unsigned int hash_span(char *start, char *end) {
    unsigned int sum = 1;
    while (start < end) {
        unsigned int rotate_by;
        sum ^= *start++;
        rotate_by = (sum ^ (sum >> 5)) & 31;
        sum = (sum << rotate_by) | (sum >> (32 - rotate_by));
    }
    return sum;
}

#endif /* DEBUG_LINUXKM_PIE_SUPPORT */

#ifdef USE_WOLFSSL_LINUXKM_PIE_REDIRECT_TABLE
extern struct wolfssl_linuxkm_pie_redirect_table wolfssl_linuxkm_pie_redirect_table;
static int set_up_wolfssl_linuxkm_pie_redirect_table(void);
#endif /* USE_WOLFSSL_LINUXKM_PIE_REDIRECT_TABLE */

#endif /* HAVE_LINUXKM_PIE_SUPPORT */

#ifdef HAVE_FIPS
static void lkmFipsCb(int ok, int err, const char* hash)
{
    if ((! ok) || (err != 0))
        pr_err("ERROR: libwolfssl FIPS error: %s\n", wc_GetErrorString(err));
    if (err == WC_NO_ERR_TRACE(IN_CORE_FIPS_E)) {
        pr_err("In-core integrity hash check failure.\n"
               "Update verifyCore[] in fips_test.c with new hash \"%s\" and rebuild.\n",
               hash ? hash : "<null>");
    }
}
#endif

#ifdef WOLFCRYPT_FIPS_CORE_DYNAMIC_HASH_VALUE
static int updateFipsHash(void);
#endif

#ifdef WOLFSSL_LINUXKM_BENCHMARKS
extern int wolfcrypt_benchmark_main(int argc, char** argv);
#endif /* WOLFSSL_LINUXKM_BENCHMARKS */

#ifndef WOLFSSL_LINUXKM_USE_MUTEXES
int wc_lkm_LockMutex(wolfSSL_Mutex* m)
{
    unsigned long irq_flags;
    /* first, try the cheap way. */
    if (spin_trylock_irqsave(&m->lock, irq_flags)) {
        m->irq_flags = irq_flags;
        return 0;
    }
    if (irq_count() != 0) {
        /* Note, this catches calls while SAVE_VECTOR_REGISTERS()ed as
         * required, because in_softirq() is always true while saved,
         * even for WC_FPU_INHIBITED_FLAG contexts.
         */
        spin_lock_irqsave(&m->lock, irq_flags);
        m->irq_flags = irq_flags;
        return 0;
    }
    else {
        for (;;) {
            if (spin_trylock_irqsave(&m->lock, irq_flags)) {
                m->irq_flags = irq_flags;
                return 0;
            }
            cond_resched();
        }
    }
    __builtin_unreachable();
}
#endif

WC_MAYBE_UNUSED static int linuxkm_lkcapi_sysfs_install_node(struct kobj_attribute *node, int *installed_flag)
{
    if ((installed_flag == NULL) || (! *installed_flag)) {
        int ret = sysfs_create_file(&THIS_MODULE->mkobj.kobj, &node->attr);
        if (ret) {
            pr_err("ERROR: sysfs_create_file failed for %s: %d\n", node->attr.name, ret);
            return ret;
        }
        if (installed_flag)
            *installed_flag = 1;
    }
    return 0;
}

WC_MAYBE_UNUSED static int linuxkm_lkcapi_sysfs_deinstall_node(struct kobj_attribute *node, int *installed_flag)
{
    if ((installed_flag == NULL) || *installed_flag) {
        sysfs_remove_file(&THIS_MODULE->mkobj.kobj, &node->attr);
        if (installed_flag)
            *installed_flag = 0;
    }
    return 0;
}

#ifdef HAVE_FIPS
    static ssize_t FIPS_rerun_self_test_handler(struct kobject *kobj, struct kobj_attribute *attr,
                                       const char *buf, size_t count);
    static struct kobj_attribute FIPS_rerun_self_test_attr = __ATTR(FIPS_rerun_self_test, 0220, NULL, FIPS_rerun_self_test_handler);
    static int installed_sysfs_FIPS_files = 0;
#endif

#ifdef LINUXKM_LKCAPI_REGISTER
    #include "linuxkm/lkcapi_glue.c"
#endif

int wc_linuxkm_check_for_intr_signals(void) {
    static const int intr_signals[] = WC_LINUXKM_INTR_SIGNALS;
    if (preempt_count() != 0)
        return 0;

#if defined(HAVE_FIPS) && defined(LINUXKM_LKCAPI_REGISTER)
    /* ignore signals during FIPS startup sequence -- failed alg tests cause
     * kernel panics on FIPS kernels.
     */
    if (linuxkm_lkcapi_registering_now)
        return 0;
#endif
    if (signal_pending(current)) {
        int i;
        for (i = 0;
             i < (int)sizeof(intr_signals) / (int)sizeof(intr_signals[0]);
             ++i)
        {
            if (sigismember(&current->pending.signal, intr_signals[i])) {
#ifdef WOLFSSL_LINUXKM_VERBOSE_DEBUG
                pr_err("INFO: wc_linuxkm_check_for_intr_signals returning "
                       "INTERRUPTED_E on signal %d\n", intr_signals[i]);
#endif
                return INTERRUPTED_E;
            }
        }
    }
    return 0;
}

void wc_linuxkm_relax_long_loop(void) {
    #if WC_LINUXKM_MAX_NS_WITHOUT_YIELD >= 0
    if (preempt_count() == 0) {
        #if (WC_LINUXKM_MAX_NS_WITHOUT_YIELD == 0) || !defined(CONFIG_SCHED_INFO)
        cond_resched();
        #else
        /* note that local_clock() wraps a local_clock_noinstr() in a
         * preempt_disable_notrace(), which sounds expensive but isn't --
         * preempt_disable_notrace() is actually just a nonlocking integer
         * increment of current_thread_info()->preempt.count, protected only by
         * various compiler optimizer barriers.
         */
        u64 now = local_clock();
        u64 current_last_arrival = current->sched_info.last_arrival;
        s64 delta = (s64)(now - current_last_arrival);
        if (delta > WC_LINUXKM_MAX_NS_WITHOUT_YIELD) {
            cond_resched();
            /* if nothing else is runnable, cond_resched() is a no-op and
             * doesn't even update .last_arrival.  we could force update by
             * sleeping, but there's no need.  we've been nice enough by just
             * cond_resched()ing, and it's actually preferable to call
             * cond_resched() frequently once computation has looped
             * continuously for longer than WC_LINUXKM_MAX_NS_WITHOUT_YIELD.
             */
        }
        #endif
    }
    #endif
}

#if defined(WOLFSSL_LINUXKM_USE_SAVE_VECTOR_REGISTERS) && defined(CONFIG_X86)
    #include "linuxkm/x86_vector_register_glue.c"
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static int __init wolfssl_init(void)
#else
static int wolfssl_init(void)
#endif
{
    int ret;

#ifdef WOLFCRYPT_FIPS_CORE_DYNAMIC_HASH_VALUE
#ifdef CONFIG_MODULE_SIG
    if (THIS_MODULE->sig_ok == false) {
        pr_err("ERROR: wolfSSL module load aborted -- bad or missing module signature with FIPS dynamic hash.\n");
        return -ECANCELED;
    }
#endif
    ret = updateFipsHash();
    if (ret < 0) {
        pr_err("ERROR: wolfSSL module load aborted -- updateFipsHash: %s\n",wc_GetErrorString(ret));
        return -ECANCELED;
    }
#endif

#ifdef USE_WOLFSSL_LINUXKM_PIE_REDIRECT_TABLE
    ret = set_up_wolfssl_linuxkm_pie_redirect_table();
    if (ret < 0)
        return ret;
#endif

#if defined(HAVE_LINUXKM_PIE_SUPPORT) && defined(DEBUG_LINUXKM_PIE_SUPPORT)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    /* see linux commit ac3b432839 */
    #define THIS_MODULE_TEXT_BASE (THIS_MODULE->mem[MOD_TEXT].base)
    #define THIS_MODULE_TEXT_SIZE (THIS_MODULE->mem[MOD_TEXT].size)
    #define THIS_MODULE_RO_BASE (THIS_MODULE->mem[MOD_RODATA].base)
    #define THIS_MODULE_RO_SIZE (THIS_MODULE->mem[MOD_RODATA].size)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)
    #define THIS_MODULE_TEXT_BASE (THIS_MODULE->core_layout.base)
    #define THIS_MODULE_TEXT_SIZE (THIS_MODULE->core_layout.text_size)
    #define THIS_MODULE_RO_BASE ((char *)THIS_MODULE->core_layout.base + THIS_MODULE->core_layout.text_size)
    #define THIS_MODULE_RO_SIZE (THIS_MODULE->core_layout.ro_size)
#else
    #define THIS_MODULE_TEXT_BASE (THIS_MODULE->module_core)
    #define THIS_MODULE_TEXT_SIZE (THIS_MODULE->core_text_size)
    #define THIS_MODULE_RO_BASE ((char *)THIS_MODULE->module_core + THIS_MODULE->core_ro_size)
    #define THIS_MODULE_RO_SIZE (THIS_MODULE->core_ro_size)
#endif

    {
        char *pie_text_start = (char *)wolfCrypt_PIE_first_function;
        char *pie_text_end = (char *)wolfCrypt_PIE_last_function;
        char *pie_rodata_start = (char *)wolfCrypt_PIE_rodata_start;
        char *pie_rodata_end = (char *)wolfCrypt_PIE_rodata_end;
        unsigned int text_hash, rodata_hash;

        text_hash = hash_span(pie_text_start, pie_text_end);
        rodata_hash = hash_span(pie_rodata_start, pie_rodata_end);

        /* note, "%pK" conceals the actual layout information.  "%px" exposes
         * the true module start address, which is potentially useful to an
         * attacker.
         */
        pr_info("wolfCrypt section hashes (spans): text 0x%x (%lu), rodata 0x%x (%lu), offset %c0x%lx\n",
                text_hash, pie_text_end-pie_text_start,
                rodata_hash, pie_rodata_end-pie_rodata_start,
                pie_text_start < pie_rodata_start ? '+' : '-',
                pie_text_start < pie_rodata_start ? pie_rodata_start - pie_text_start : pie_text_start - pie_rodata_start);
    }

#endif /* HAVE_LINUXKM_PIE_SUPPORT && DEBUG_LINUXKM_PIE_SUPPORT */

#ifdef HAVE_FIPS
    ret = wolfCrypt_SetCb_fips(lkmFipsCb);
    if (ret != 0) {
        pr_err("ERROR: wolfCrypt_SetCb_fips() failed: %s\n", wc_GetErrorString(ret));
        return -ECANCELED;
    }
    fipsEntry();
    ret = wolfCrypt_GetStatus_fips();
    if (ret != 0) {
        pr_err("ERROR: wolfCrypt_GetStatus_fips() failed with code %d: %s\n", ret, wc_GetErrorString(ret));
        if (ret == WC_NO_ERR_TRACE(IN_CORE_FIPS_E)) {
            const char *newhash = wolfCrypt_GetCoreHash_fips();
            pr_err("Update verifyCore[] in fips_test.c with new hash \"%s\" and rebuild.\n",
                   newhash ? newhash : "<null>");
        }
        return -ECANCELED;
    }
#endif /* HAVE_FIPS */

#ifdef WC_RNG_SEED_CB
    ret = wc_SetSeed_Cb(wc_GenerateSeed);
    if (ret < 0) {
        pr_err("ERROR: wc_SetSeed_Cb() failed with return code %d.\n", ret);
        (void)libwolfssl_cleanup();
        msleep(10);
        return -ECANCELED;
    }
#endif

#ifdef WOLFCRYPT_ONLY
    ret = wolfCrypt_Init();
    if (ret != 0) {
        pr_err("ERROR: wolfCrypt_Init() failed: %s\n", wc_GetErrorString(ret));
        return -ECANCELED;
    }
#else
    ret = wolfSSL_Init();
    if (ret != WOLFSSL_SUCCESS) {
        pr_err("ERROR: wolfSSL_Init() failed: %s\n", wc_GetErrorString(ret));
        return -ECANCELED;
    }
#endif

#if defined(HAVE_FIPS) && FIPS_VERSION3_GT(5,2,0)
    ret = wc_RunAllCast_fips();
    if (ret != 0) {
        pr_err("ERROR: wc_RunAllCast_fips() failed with return value %d\n", ret);
        return -ECANCELED;
    }

    pr_info("FIPS 140-3 wolfCrypt-fips v%d.%d.%d%s%s startup "
            "self-test succeeded.\n",
#ifdef HAVE_FIPS_VERSION_MAJOR
            HAVE_FIPS_VERSION_MAJOR,
#else
            HAVE_FIPS_VERSION,
#endif
#ifdef HAVE_FIPS_VERSION_MINOR
            HAVE_FIPS_VERSION_MINOR,
#else
            0,
#endif
#ifdef HAVE_FIPS_VERSION_PATCH
            HAVE_FIPS_VERSION_PATCH,
#else
            0,
#endif
#ifdef HAVE_FIPS_VERSION_PORT
            "-",
            HAVE_FIPS_VERSION_PORT
#else
            "",
            ""
#endif
        );
#endif /* HAVE_FIPS && FIPS_VERSION3_GT(5,2,0) */

#ifndef NO_CRYPT_TEST
    ret = wolfcrypt_test(NULL);
    if (ret < 0) {
        pr_err("ERROR: wolfcrypt self-test failed with return code %d.\n", ret);
        (void)libwolfssl_cleanup();
        msleep(10);
        return -ECANCELED;
    }
    pr_info("wolfCrypt self-test passed.\n");
#else
#if !defined(HAVE_FIPS) || FIPS_VERSION3_LE(5,2,0)
    pr_info("skipping full wolfcrypt_test() "
            "(configure with --enable-crypttests to enable).\n");
#endif
#endif

#ifdef LINUXKM_LKCAPI_REGISTER
#ifdef LINUXKM_LKCAPI_REGISTER_ONLY_ON_COMMAND
    ret = linuxkm_lkcapi_sysfs_install();

    if (ret) {
        pr_err("ERROR: linuxkm_lkcapi_sysfs_install() failed with return code %d.\n", ret);
        (void)libwolfssl_cleanup();
        msleep(10);
        return -ECANCELED;
    }
#else /* !LINUXKM_LKCAPI_REGISTER_ONLY_ON_COMMAND */
    ret = linuxkm_lkcapi_register();

    if (ret) {
        pr_err("ERROR: linuxkm_lkcapi_register() failed with return code %d.\n", ret);
        linuxkm_lkcapi_unregister();
        (void)libwolfssl_cleanup();
        msleep(10);
        return -ECANCELED;
    }
#endif /* !LINUXKM_LKCAPI_REGISTER_ONLY_ON_COMMAND */
#endif /* LINUXKM_LKCAPI_REGISTER */

#ifdef HAVE_FIPS
    (void)linuxkm_lkcapi_sysfs_install_node(&FIPS_rerun_self_test_attr, &installed_sysfs_FIPS_files);
#endif

#ifdef WOLFSSL_LINUXKM_BENCHMARKS
    wolfcrypt_benchmark_main(0, (char**)NULL);
#endif

#ifdef WOLFCRYPT_ONLY
    pr_info("wolfCrypt " LIBWOLFSSL_VERSION_STRING " loaded%s"
            ".\nSee https://www.wolfssl.com/ for more information.\n"
            "wolfCrypt Copyright (C) 2006-present wolfSSL Inc.  Licensed under " WOLFSSL_LICENSE ".\n",
#ifdef CONFIG_MODULE_SIG
            THIS_MODULE->sig_ok ? " with valid module signature" : " without valid module signature"
#else
            ""
#endif
        );
#else
    pr_info("wolfSSL " LIBWOLFSSL_VERSION_STRING " loaded%s"
            ".\nSee https://www.wolfssl.com/ for more information.\n"
            "wolfSSL Copyright (C) 2006-present wolfSSL Inc.  Licensed under " WOLFSSL_LICENSE ".\n",
#ifdef CONFIG_MODULE_SIG
            THIS_MODULE->sig_ok ? " with valid module signature" : " without valid module signature"
#else
            ""
#endif
        );
#endif

    return 0;
}

module_init(wolfssl_init);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
static void __exit wolfssl_exit(void)
#else
static void wolfssl_exit(void)
#endif
{
#ifdef HAVE_FIPS
    int ret;

    (void)linuxkm_lkcapi_sysfs_deinstall_node(&FIPS_rerun_self_test_attr, &installed_sysfs_FIPS_files);
#endif

#ifdef LINUXKM_LKCAPI_REGISTER
    (void)linuxkm_lkcapi_unregister();
    (void)linuxkm_lkcapi_sysfs_deinstall();
#endif

#ifdef HAVE_FIPS
    ret = wc_RunAllCast_fips();
    if (ret != 0) {
        pr_err("ERROR: wc_RunAllCast_fips() failed at shutdown with return value %d\n", ret);
    }
    else
        pr_info("wolfCrypt FIPS re-self-test succeeded at unload: all algorithms re-verified.");
#endif

    (void)libwolfssl_cleanup();

    return;
}

module_exit(wolfssl_exit);

#if defined(LINUXKM_LKCAPI_REGISTER) || !defined(WOLFSSL_NO_ASM)
    /* When registering algorithms with crypto_register_skcipher() and friends,
     * or using kernel_fpu_begin_mask() and _end() to wrap vector register
     * usage, we use a "GPL" license unconditionally here to meet the GPL-only
     * requirements for those calls, satisfying license_is_gpl_compatible() (see
     * /usr/src/linux/include/linux/license.h).
     */
    MODULE_LICENSE("GPL");
#else
    MODULE_LICENSE(WOLFSSL_LICENSE);
#endif

MODULE_AUTHOR("https://www.wolfssl.com/");
MODULE_DESCRIPTION("libwolfssl cryptographic and protocol facilities");
MODULE_VERSION(LIBWOLFSSL_VERSION_STRING);

#ifdef USE_WOLFSSL_LINUXKM_PIE_REDIRECT_TABLE

/* get_current() is an inline or macro, depending on the target -- sidestep the whole issue with a wrapper func. */
static struct task_struct *my_get_current_thread(void) {
    return get_current();
}

/* preempt_count() is an inline function in arch/x86/include/asm/preempt.h that
 * accesses __preempt_count, which is an int array declared with
 * DECLARE_PER_CPU_CACHE_HOT.
 */
static int my_preempt_count(void) {
    return preempt_count();
}

static int set_up_wolfssl_linuxkm_pie_redirect_table(void) {
    memset(
        &wolfssl_linuxkm_pie_redirect_table,
        0,
        sizeof wolfssl_linuxkm_pie_redirect_table);

#ifndef __ARCH_MEMCMP_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.memcmp = memcmp;
#endif
#ifndef CONFIG_FORTIFY_SOURCE
#ifndef __ARCH_MEMCPY_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.memcpy = memcpy;
#endif
#ifndef __ARCH_MEMSET_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.memset = memset;
#endif
#ifndef __ARCH_MEMMOVE_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.memmove = memmove;
#endif
#endif /* !CONFIG_FORTIFY_SOURCE */
#ifndef __ARCH_STRCMP_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strcmp = strcmp;
#endif
#ifndef __ARCH_STRNCMP_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strncmp = strncmp;
#endif
#ifndef __ARCH_STRCASECMP_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strcasecmp = strcasecmp;
#endif
#ifndef __ARCH_STRNCASECMP_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strncasecmp = strncasecmp;
#endif
#ifndef __ARCH_STRLEN_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strlen = strlen;
#endif
#ifndef __ARCH_STRSTR_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strstr = strstr;
#endif
#ifndef __ARCH_STRNCPY_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strncpy = strncpy;
#endif
#ifndef __ARCH_STRNCAT_NO_REDIRECT
    wolfssl_linuxkm_pie_redirect_table.strncat = strncat;
#endif
    wolfssl_linuxkm_pie_redirect_table.kstrtoll = kstrtoll;

    #if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)) || \
        (defined(RHEL_MAJOR) && \
         ((RHEL_MAJOR > 9) || ((RHEL_MAJOR == 9) && (RHEL_MINOR >= 5))))
        wolfssl_linuxkm_pie_redirect_table._printk = _printk;
    #else
        wolfssl_linuxkm_pie_redirect_table.printk = printk;
    #endif

#ifdef CONFIG_FORTIFY_SOURCE
    wolfssl_linuxkm_pie_redirect_table.__warn_printk = __warn_printk;
#endif

    wolfssl_linuxkm_pie_redirect_table.snprintf = snprintf;

    wolfssl_linuxkm_pie_redirect_table._ctype = _ctype;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
    wolfssl_linuxkm_pie_redirect_table.kmalloc_noprof = kmalloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.krealloc_noprof = krealloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.kzalloc_noprof = kzalloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.__kvmalloc_node_noprof = __kvmalloc_node_noprof;
    wolfssl_linuxkm_pie_redirect_table.__kmalloc_cache_noprof = __kmalloc_cache_noprof;
#ifdef HAVE_KVREALLOC
    wolfssl_linuxkm_pie_redirect_table.kvrealloc_noprof = kvrealloc_noprof;
#endif
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    wolfssl_linuxkm_pie_redirect_table.kmalloc_noprof = kmalloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.krealloc_noprof = krealloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.kzalloc_noprof = kzalloc_noprof;
    wolfssl_linuxkm_pie_redirect_table.kvmalloc_node_noprof = kvmalloc_node_noprof;
    wolfssl_linuxkm_pie_redirect_table.kmalloc_trace_noprof = kmalloc_trace_noprof;
#ifdef HAVE_KVREALLOC
    wolfssl_linuxkm_pie_redirect_table.kvrealloc_noprof = kvrealloc_noprof;
#endif
#else
    wolfssl_linuxkm_pie_redirect_table.kmalloc = kmalloc;
    wolfssl_linuxkm_pie_redirect_table.krealloc = krealloc;
#ifdef HAVE_KVMALLOC
    wolfssl_linuxkm_pie_redirect_table.kvmalloc_node = kvmalloc_node;
#endif
#ifdef HAVE_KVREALLOC
    wolfssl_linuxkm_pie_redirect_table.kvrealloc = kvrealloc;
#endif
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0) || \
        (defined(RHEL_MAJOR) &&                                    \
         ((RHEL_MAJOR > 9) || ((RHEL_MAJOR == 9) && (RHEL_MINOR >= 5))))
        wolfssl_linuxkm_pie_redirect_table.kmalloc_trace =
            kmalloc_trace;
    #else
        wolfssl_linuxkm_pie_redirect_table.kmem_cache_alloc_trace =
            kmem_cache_alloc_trace;
        wolfssl_linuxkm_pie_redirect_table.kmalloc_order_trace =
            kmalloc_order_trace;
    #endif
#endif

    wolfssl_linuxkm_pie_redirect_table.kfree = kfree;
    wolfssl_linuxkm_pie_redirect_table.ksize = ksize;
#ifdef HAVE_KVMALLOC
    wolfssl_linuxkm_pie_redirect_table.kvfree = kvfree;
#endif

    wolfssl_linuxkm_pie_redirect_table.get_random_bytes = get_random_bytes;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
        wolfssl_linuxkm_pie_redirect_table.getnstimeofday =
            getnstimeofday;
    #elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
        wolfssl_linuxkm_pie_redirect_table.current_kernel_time64 =
            current_kernel_time64;
    #else
        wolfssl_linuxkm_pie_redirect_table.ktime_get_coarse_real_ts64 =
            ktime_get_coarse_real_ts64;
    #endif

    wolfssl_linuxkm_pie_redirect_table.get_current = my_get_current_thread;

#if defined(WOLFSSL_LINUXKM_USE_SAVE_VECTOR_REGISTERS) && defined(CONFIG_X86)
    wolfssl_linuxkm_pie_redirect_table.allocate_wolfcrypt_linuxkm_fpu_states = allocate_wolfcrypt_linuxkm_fpu_states;
    wolfssl_linuxkm_pie_redirect_table.wc_can_save_vector_registers_x86 = wc_can_save_vector_registers_x86;
    wolfssl_linuxkm_pie_redirect_table.free_wolfcrypt_linuxkm_fpu_states = free_wolfcrypt_linuxkm_fpu_states;
    wolfssl_linuxkm_pie_redirect_table.wc_restore_vector_registers_x86 = wc_restore_vector_registers_x86;
    wolfssl_linuxkm_pie_redirect_table.wc_save_vector_registers_x86 = wc_save_vector_registers_x86;
#elif defined(WOLFSSL_LINUXKM_USE_SAVE_VECTOR_REGISTERS)
    #error WOLFSSL_LINUXKM_USE_SAVE_VECTOR_REGISTERS is set for an unsupported architecture.
#endif /* WOLFSSL_LINUXKM_USE_SAVE_VECTOR_REGISTERS */

    wolfssl_linuxkm_pie_redirect_table.__mutex_init = __mutex_init;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
        wolfssl_linuxkm_pie_redirect_table.mutex_lock_nested = mutex_lock_nested;
    #else
        wolfssl_linuxkm_pie_redirect_table.mutex_lock = mutex_lock;
    #endif
    wolfssl_linuxkm_pie_redirect_table.mutex_unlock = mutex_unlock;
    #if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
        wolfssl_linuxkm_pie_redirect_table.mutex_destroy = mutex_destroy;
    #endif

#ifdef HAVE_FIPS
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_first =
        wolfCrypt_FIPS_first;
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_last =
        wolfCrypt_FIPS_last;
    #if FIPS_VERSION3_GE(6,0,0)
#ifndef NO_AES
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_AES_sanity =
        wolfCrypt_FIPS_AES_sanity;
#if defined(WOLFSSL_CMAC) && defined(WOLFSSL_AES_DIRECT)
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_CMAC_sanity =
        wolfCrypt_FIPS_CMAC_sanity;
#endif
#endif
#ifndef NO_DH
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_DH_sanity =
        wolfCrypt_FIPS_DH_sanity;
#endif
#ifdef HAVE_ECC
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_ECC_sanity =
        wolfCrypt_FIPS_ECC_sanity;
#endif
#ifdef HAVE_ED25519
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_ED25519_sanity =
        wolfCrypt_FIPS_ED25519_sanity;
#endif
#ifdef HAVE_ED448
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_ED448_sanity =
        wolfCrypt_FIPS_ED448_sanity;
#endif
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_HMAC_sanity =
        wolfCrypt_FIPS_HMAC_sanity;
#ifndef NO_KDF
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_KDF_sanity =
        wolfCrypt_FIPS_KDF_sanity;
#endif
#ifdef HAVE_PBKDF2
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_PBKDF_sanity =
        wolfCrypt_FIPS_PBKDF_sanity;
#endif
#ifdef HAVE_HASHDRBG
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_DRBG_sanity =
        wolfCrypt_FIPS_DRBG_sanity;
#endif
#ifndef NO_RSA
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_RSA_sanity =
        wolfCrypt_FIPS_RSA_sanity;
#endif
#ifndef NO_SHA
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_SHA_sanity =
        wolfCrypt_FIPS_SHA_sanity;
#endif
#ifndef NO_SHA256
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_SHA256_sanity =
        wolfCrypt_FIPS_SHA256_sanity;
#endif
#ifdef WOLFSSL_SHA512
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_SHA512_sanity =
        wolfCrypt_FIPS_SHA512_sanity;
#endif
#ifdef WOLFSSL_SHA3
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_SHA3_sanity =
        wolfCrypt_FIPS_SHA3_sanity;
#endif
    wolfssl_linuxkm_pie_redirect_table.wolfCrypt_FIPS_FT_sanity =
        wolfCrypt_FIPS_FT_sanity;
    wolfssl_linuxkm_pie_redirect_table.wc_RunAllCast_fips =
        wc_RunAllCast_fips;
    #endif
#endif

#if !defined(WOLFCRYPT_ONLY) && !defined(NO_CERTS)
    wolfssl_linuxkm_pie_redirect_table.GetCA = GetCA;
#ifndef NO_SKID
    wolfssl_linuxkm_pie_redirect_table.GetCAByName = GetCAByName;
#ifdef HAVE_OCSP
    wolfssl_linuxkm_pie_redirect_table.GetCAByKeyHash = GetCAByKeyHash;
#endif /* HAVE_OCSP */
#endif /* NO_SKID */
#ifdef WOLFSSL_AKID_NAME
    wolfssl_linuxkm_pie_redirect_table.GetCAByAKID = GetCAByAKID;
#endif /* WOLFSSL_AKID_NAME */
#if defined(OPENSSL_EXTRA) || defined(OPENSSL_EXTRA_X509_SMALL)
    wolfssl_linuxkm_pie_redirect_table.wolfSSL_X509_NAME_add_entry_by_NID = wolfSSL_X509_NAME_add_entry_by_NID;
    wolfssl_linuxkm_pie_redirect_table.wolfSSL_X509_NAME_free = wolfSSL_X509_NAME_free;
    wolfssl_linuxkm_pie_redirect_table.wolfSSL_X509_NAME_new_ex = wolfSSL_X509_NAME_new_ex;
#endif /* OPENSSL_EXTRA || OPENSSL_EXTRA_X509_SMALL */
#endif /* !WOLFCRYPT_ONLY && !NO_CERTS */

#ifdef WOLFSSL_DEBUG_BACKTRACE_ERROR_CODES
    wolfssl_linuxkm_pie_redirect_table.dump_stack = dump_stack;
#endif

    wolfssl_linuxkm_pie_redirect_table.preempt_count = my_preempt_count;
#ifndef _raw_spin_lock_irqsave
    wolfssl_linuxkm_pie_redirect_table._raw_spin_lock_irqsave = _raw_spin_lock_irqsave;
#endif
#ifndef _raw_spin_trylock
    wolfssl_linuxkm_pie_redirect_table._raw_spin_trylock = _raw_spin_trylock;
#endif
#ifndef _raw_spin_unlock_irqrestore
    wolfssl_linuxkm_pie_redirect_table._raw_spin_unlock_irqrestore = _raw_spin_unlock_irqrestore;
#endif
    wolfssl_linuxkm_pie_redirect_table._cond_resched = _cond_resched;

#ifndef WOLFSSL_LINUXKM_USE_MUTEXES
    wolfssl_linuxkm_pie_redirect_table.wc_lkm_LockMutex = wc_lkm_LockMutex;
#endif

#ifdef CONFIG_ARM64
    wolfssl_linuxkm_pie_redirect_table.alt_cb_patch_nops = alt_cb_patch_nops;
    wolfssl_linuxkm_pie_redirect_table.queued_spin_lock_slowpath = queued_spin_lock_slowpath;
#endif

    wolfssl_linuxkm_pie_redirect_table.wc_linuxkm_check_for_intr_signals = wc_linuxkm_check_for_intr_signals;
    wolfssl_linuxkm_pie_redirect_table.wc_linuxkm_relax_long_loop = wc_linuxkm_relax_long_loop;

    /* runtime assert that the table has no null slots after initialization. */
    {
        unsigned long *i;
        static_assert(sizeof(unsigned long) == sizeof(void *),
                      "unexpected pointer size");
        for (i = (unsigned long *)&wolfssl_linuxkm_pie_redirect_table;
             i < (unsigned long *)&wolfssl_linuxkm_pie_redirect_table._last_slot;
             ++i)
            if (*i == 0) {
                pr_err("ERROR: wolfCrypt container redirect table initialization was "
                       "incomplete [%lu].\n",
                       i-(unsigned long *)&wolfssl_linuxkm_pie_redirect_table);
                return -EFAULT;
            }
    }

    return 0;
}

#endif /* USE_WOLFSSL_LINUXKM_PIE_REDIRECT_TABLE */


#ifdef WOLFCRYPT_FIPS_CORE_DYNAMIC_HASH_VALUE

#include <wolfssl/wolfcrypt/coding.h>

PRAGMA_GCC_DIAG_PUSH
PRAGMA_GCC("GCC diagnostic ignored \"-Wnested-externs\"")
PRAGMA_GCC("GCC diagnostic ignored \"-Wpointer-arith\"")
#include <crypto/hash.h>
PRAGMA_GCC_DIAG_POP

extern char verifyCore[WC_SHA256_DIGEST_SIZE*2 + 1];
extern const char coreKey[WC_SHA256_DIGEST_SIZE*2 + 1];
extern const unsigned int wolfCrypt_FIPS_ro_start[];
extern const unsigned int wolfCrypt_FIPS_ro_end[];

#define FIPS_IN_CORE_KEY_SZ 32
#define FIPS_IN_CORE_VERIFY_SZ FIPS_IN_CORE_KEY_SZ
typedef int (*fips_address_function)(void);
#define MAX_FIPS_DATA_SZ 10000000
#define MAX_FIPS_CODE_SZ 10000000
extern int GenBase16_Hash(const byte* in, int length, char* out, int outSz);

static int updateFipsHash(void)
{
    struct crypto_shash *tfm = NULL;
    struct shash_desc *desc = NULL;
    word32 verifySz  = FIPS_IN_CORE_VERIFY_SZ;
    word32 binCoreSz  = FIPS_IN_CORE_KEY_SZ;
    int ret;
    byte *hash = NULL;
    char *base16_hash = NULL;
    byte *binCoreKey = NULL;
    byte *binVerify = NULL;

    fips_address_function first = wolfCrypt_FIPS_first;
    fips_address_function last  = wolfCrypt_FIPS_last;

    char* start = (char*)wolfCrypt_FIPS_ro_start;
    char* end   = (char*)wolfCrypt_FIPS_ro_end;

    unsigned long code_sz = (unsigned long)last - (unsigned long)first;
    unsigned long data_sz = (unsigned long)end - (unsigned long)start;

    if (data_sz == 0 || data_sz > MAX_FIPS_DATA_SZ)
        return BAD_FUNC_ARG;  /* bad fips data size */

    if (code_sz == 0 || code_sz > MAX_FIPS_CODE_SZ)
        return BAD_FUNC_ARG;  /* bad fips code size */

    hash = XMALLOC(WC_SHA256_DIGEST_SIZE, 0, DYNAMIC_TYPE_TMP_BUFFER);
    if (hash == NULL) {
        ret = MEMORY_E;
        goto out;
    }
    base16_hash = XMALLOC(WC_SHA256_DIGEST_SIZE*2 + 1, 0, DYNAMIC_TYPE_TMP_BUFFER);
    if (base16_hash == NULL) {
        ret = MEMORY_E;
        goto out;
    }
    binCoreKey = XMALLOC(binCoreSz, 0, DYNAMIC_TYPE_TMP_BUFFER);
    if (binCoreKey == NULL) {
        ret = MEMORY_E;
        goto out;
    }
    binVerify = XMALLOC(verifySz, 0, DYNAMIC_TYPE_TMP_BUFFER);
    if (binVerify == NULL) {
        ret = MEMORY_E;
        goto out;
    }

    {
        word32 base16_out_len = binCoreSz;
        ret = Base16_Decode((const byte *)coreKey, sizeof coreKey - 1, binCoreKey, &base16_out_len);
        if (ret != 0) {
            pr_err("ERROR: Base16_Decode for coreKey: %s\n", wc_GetErrorString(ret));
            goto out;
        }
        if (base16_out_len != binCoreSz) {
            pr_err("ERROR: unexpected output length %u for coreKey from Base16_Decode.\n",base16_out_len);
            ret = BAD_STATE_E;
            goto out;
        }
    }

    tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
    if (IS_ERR(tfm)) {
        if (PTR_ERR(tfm) == -ENOMEM) {
            pr_err("ERROR: crypto_alloc_shash failed: out of memory\n");
            ret = MEMORY_E;
        } else if (PTR_ERR(tfm) == -ENOENT) {
            pr_err("ERROR: crypto_alloc_shash failed: kernel is missing hmac(sha256) implementation\n");
            pr_err("ERROR: check for CONFIG_CRYPTO_SHA256 and CONFIG_CRYPTO_HMAC.\n");
            ret = NOT_COMPILED_IN;
        } else {
            pr_err("ERROR: crypto_alloc_shash failed with ret %ld\n",PTR_ERR(tfm));
            ret = HASH_TYPE_E;
        }
        tfm = NULL;
        goto out;
    }

    {
        size_t desc_size = crypto_shash_descsize(tfm) + sizeof *desc;
        desc = XMALLOC(desc_size, NULL, DYNAMIC_TYPE_TMP_BUFFER);
        if (desc == NULL) {
            pr_err("ERROR: failed allocating desc.");
            ret = MEMORY_E;
            goto out;
        }
        XMEMSET(desc, 0, desc_size);
    }

    ret = crypto_shash_setkey(tfm, binCoreKey, binCoreSz);
    if (ret) {
        pr_err("ERROR: crypto_ahash_setkey failed: err %d\n", ret);
        ret = BAD_STATE_E;
        goto out;
    }

    desc->tfm = tfm;
    ret = crypto_shash_init(desc);
    if (ret) {
        pr_err("ERROR: crypto_shash_init failed: err %d\n", ret);
        ret = BAD_STATE_E;
        goto out;
    }

    WC_SANITIZE_DISABLE();

    ret = crypto_shash_update(desc, (byte *)(wc_ptr_t)first, (word32)code_sz);
    if (ret) {
        pr_err("ERROR: crypto_shash_update failed: err %d\n", ret);
        ret = BAD_STATE_E;
        goto out;
    }

    /* don't hash verifyCore or changing verifyCore will change hash */
    if (verifyCore >= start && verifyCore < end) {
        data_sz = (unsigned long)verifyCore - (unsigned long)start;
        ret = crypto_shash_update(desc, (byte*)start, (word32)data_sz);
        if (ret) {
                pr_err("ERROR: crypto_shash_update failed: err %d\n", ret);
                ret = BAD_STATE_E;
                goto out;
        }
        start   = (char*)verifyCore + sizeof(verifyCore);
        data_sz = (unsigned long)end - (unsigned long)start;
    }
    ret = crypto_shash_update(desc, (byte*)start, (word32)data_sz);
    if (ret) {
        pr_err("ERROR: crypto_shash_update failed: err %d\n", ret);
        ret = BAD_STATE_E;
        goto out;
    }

    WC_SANITIZE_ENABLE();

    ret = crypto_shash_final(desc, hash);
    if (ret) {
        pr_err("ERROR: crypto_shash_final failed: err %d\n", ret);
        ret = BAD_STATE_E;
        goto out;
    }

    ret = GenBase16_Hash(hash, WC_SHA256_DIGEST_SIZE, base16_hash, WC_SHA256_DIGEST_SIZE*2 + 1);
    if (ret != 0) {
        pr_err("ERROR: GenBase16_Hash failed: %s\n", wc_GetErrorString(ret));
        goto out;
    }

    {
        word32 base16_out_len = verifySz;
        ret = Base16_Decode((const byte *)verifyCore, sizeof verifyCore - 1, binVerify, &base16_out_len);
        if (ret != 0) {
            pr_err("ERROR: Base16_Decode for verifyCore: %s\n", wc_GetErrorString(ret));
            goto out;
        }
        if (base16_out_len != binCoreSz) {
            pr_err("ERROR: unexpected output length %u for verifyCore from Base16_Decode.\n",base16_out_len);
            ret = BAD_STATE_E;
            goto out;
        }
    }

    if (XMEMCMP(hash, binVerify, WC_SHA256_DIGEST_SIZE) == 0) {
#if defined(DEBUG_LINUXKM_PIE_SUPPORT) || defined(WOLFSSL_LINUXKM_VERBOSE_DEBUG)
        pr_info("updateFipsHash: verifyCore already matches [%s]\n", verifyCore);
#else
        pr_info("updateFipsHash: verifyCore already matches.\n");
#endif
    } else {
        XMEMCPY(verifyCore, base16_hash, WC_SHA256_DIGEST_SIZE*2 + 1);
#if defined(DEBUG_LINUXKM_PIE_SUPPORT) || defined(WOLFSSL_LINUXKM_VERBOSE_DEBUG)
        pr_info("updateFipsHash: verifyCore updated [%s].\n", base16_hash);
#else
        pr_info("updateFipsHash: verifyCore updated.\n");
#endif
    }

    ret = 0;

  out:

    if (tfm != NULL)
        crypto_free_shash(tfm);
    XFREE(desc, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(hash, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(base16_hash, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(binCoreKey, NULL, DYNAMIC_TYPE_TMP_BUFFER);
    XFREE(binVerify, NULL, DYNAMIC_TYPE_TMP_BUFFER);

    return ret;
}

#endif /* WOLFCRYPT_FIPS_CORE_DYNAMIC_HASH_VALUE */

#ifdef HAVE_FIPS

static ssize_t FIPS_rerun_self_test_handler(struct kobject *kobj, struct kobj_attribute *attr,
                                   const char *buf, size_t count)
{
    int arg;
    int ret;

    (void)kobj;
    (void)attr;

    if (kstrtoint(buf, 10, &arg) || arg != 1)
        return -EINVAL;

    pr_info("wolfCrypt: rerunning FIPS self-test on command.");

    ret = wolfCrypt_IntegrityTest_fips();
    if (ret != 0) {
        pr_err("ERROR: wolfCrypt_IntegrityTest_fips: error %d", ret);
        return -EINVAL;
    }

    ret = wolfCrypt_GetStatus_fips();
    if (ret != 0) {
        pr_err("ERROR: wolfCrypt_GetStatus_fips() failed with code %d: %s\n", ret, wc_GetErrorString(ret));
        if (ret == WC_NO_ERR_TRACE(IN_CORE_FIPS_E))
            return -ELIBBAD;
        else
            return -EINVAL;
    }

    ret = wc_RunAllCast_fips();
    if (ret != 0) {
        pr_err("ERROR: wc_RunAllCast_fips() failed with return value %d\n", ret);
        return -EINVAL;
    }

    pr_info("wolfCrypt FIPS re-self-test succeeded: all algorithms verified and available.");

    return count;
}

#endif /* HAVE_FIPS */
