/* psprecomp shim: decoder diagnostics go to stderr only when PSPRECOMP_ATRAC_LOG is set. */
#pragma once
#include <cstdio>
#include <cstdlib>
#define PSPRECOMP_AT3_LOG(...) do { if (getenv("PSPRECOMP_ATRAC_LOG")) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define DEBUG_LOG(cat, ...) PSPRECOMP_AT3_LOG(__VA_ARGS__)
#define INFO_LOG(cat, ...)  PSPRECOMP_AT3_LOG(__VA_ARGS__)
#define WARN_LOG(cat, ...)  PSPRECOMP_AT3_LOG(__VA_ARGS__)
#define ERROR_LOG(cat, ...) PSPRECOMP_AT3_LOG(__VA_ARGS__)
