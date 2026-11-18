/* psprecomp shim: x86/x64 builds take the SSE2 path, everything else the scalar one. */
#pragma once
#if defined(_M_X64) || defined(__x86_64__)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 1
#elif defined(_M_IX86) || defined(__i386__)
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 1
#define PPSSPP_ARCH_SSE2 1
#else
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 0
#define PPSSPP_ARCH_SSE2 0
#endif
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64 0
#define PPSSPP_ARCH(x) (PPSSPP_ARCH_##x)
