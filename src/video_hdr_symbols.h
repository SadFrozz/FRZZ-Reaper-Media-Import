#pragma once

#define FRZZ_HDR_API_JOIN(name, abi) name##_##abi
#define FRZZ_HDR_API_EXPAND(name, abi) FRZZ_HDR_API_JOIN(name, abi)
#ifdef FRZZ_WINDOWS_HDR
#define FRZZ_HDR_API(name) FRZZ_HDR_API_EXPAND(name, FRZZ_WINDOWS_HDR)
#else
#define FRZZ_HDR_API(name) FRZZ_HDR_API_EXPAND(name, FRZZ_MACOS_HDR)
#endif
