/*
 * lpst_platform.h — Compile-time platform detection for LinchpinST.
 *
 * Defines exactly one of LPST_PLATFORM_ATARI, LPST_PLATFORM_AMIGA, or
 * LPST_PLATFORM_POSIX as 1 (with the others set to 0), so the rest of the
 * code can use simple compile-time conditionals rather than raw predefined
 * macros.
 */
#ifndef LPST_PLATFORM_H
#define LPST_PLATFORM_H

#if defined(LPST_TARGET_ATARI) || defined(__MINT__)
#define LPST_PLATFORM_ATARI 1
#else
#define LPST_PLATFORM_ATARI 0
#endif

#if defined(LPST_TARGET_AMIGA) || defined(__amigaos__) || defined(__AMIGA__) || defined(AMIGA)
#define LPST_PLATFORM_AMIGA 1
#else
#define LPST_PLATFORM_AMIGA 0
#endif

#if !LPST_PLATFORM_ATARI && !LPST_PLATFORM_AMIGA
#define LPST_PLATFORM_POSIX 1
#else
#define LPST_PLATFORM_POSIX 0
#endif

#endif