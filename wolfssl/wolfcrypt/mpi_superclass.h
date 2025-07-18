/* mpi_superclass.h
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



/* super class file for PK algos */

/* default ... include all MPI */
#define LTM_ALL

/* RSA only (does not support DH/DSA/ECC) */
/* #define SC_RSA_1 */

/* For reference.... On an Athlon64 optimizing for speed...

   LTM's mpi.o with all functions [striped] is 142KiB in size.

*/

/* Works for RSA only, mpi.o is 68KiB */
#ifdef SC_RSA_1
   #define BN_MP_SHRINK_C
   #define BN_MP_LCM_C
   #define BN_MP_PRIME_RANDOM_EX_C
   #define BN_MP_INVMOD_C
   #define BN_MP_GCD_C
   #define BN_MP_MOD_C
   #define BN_MP_MULMOD_C
   #define BN_MP_ADDMOD_C
   #define BN_MP_EXPTMOD_C
   #define BN_MP_SET_INT_C
   #define BN_MP_INIT_MULTI_C
   #define BN_MP_CLEAR_MULTI_C
   #define BN_MP_UNSIGNED_BIN_SIZE_C
   #define BN_MP_TO_UNSIGNED_BIN_C
   #define BN_MP_MOD_D_C
   #define BN_MP_PRIME_RABIN_MILLER_TRIALS_C
   #define BN_REVERSE_C
   #define BN_PRIME_TAB_C

   /* other modifiers */
   #define BN_MP_DIV_SMALL                    /* Slower division, not critical */

   /* here we are on the last pass so we turn things off.  The functions classes are still there
    * but we remove them specifically from the build.  This also invokes tweaks in functions
    * like removing support for even moduli, etc...
    */
#ifdef LTM_LAST
   #undef  BN_MP_TOOM_MUL_C
   #undef  BN_MP_TOOM_SQR_C
   #undef  BN_MP_KARATSUBA_MUL_C
   #undef  BN_MP_KARATSUBA_SQR_C
   #undef  BN_MP_REDUCE_C
   #undef  BN_MP_REDUCE_SETUP_C
   #undef  BN_MP_DR_IS_MODULUS_C
   #undef  BN_MP_DR_SETUP_C
   #undef  BN_MP_DR_REDUCE_C
   #undef  BN_MP_REDUCE_IS_2K_C
   #undef  BN_MP_REDUCE_2K_SETUP_C
   #undef  BN_MP_REDUCE_2K_C
   #undef  BN_S_MP_EXPTMOD_C
   #undef  BN_MP_DIV_3_C
   #undef  BN_S_MP_MUL_HIGH_DIGS_C
   #undef  BN_FAST_S_MP_MUL_HIGH_DIGS_C
   #undef  BN_FAST_MP_INVMOD_C

   /* To safely undefine these you have to make sure your RSA key won't exceed the Comba threshold
    * which is roughly 255 digits [7140 bits for 32-bit machines, 15300 bits for 64-bit machines]
    * which means roughly speaking you can handle up to 2536-bit RSA keys with these defined without
    * trouble.
    */
   #undef  BN_S_MP_MUL_DIGS_C
   #undef  BN_S_MP_SQR_C
   #undef  BN_MP_MONTGOMERY_REDUCE_C
#endif

#endif

