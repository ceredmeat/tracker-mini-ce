#ifndef CE_LOOPER_CONFIG_H_
#define CE_LOOPER_CONFIG_H_

// tracker-mini-ce Looper: on the Tracker+ and Mini (64 MB of SDRAM). Its memory is taken
// from the sample memory: 16 MB in the tracker firmware (about 18 s of stereo per track),
// 44 MB in the looper-only firmware (about 54 s per track).

#include <stdint.h>

#if defined(PLATFORM_IMX) && !defined(CE_NO_LOOPER)
#define CE_LOOPER 1
#ifdef CE_LOOPER_ONLY
constexpr uint32_t kLooperPoolBytes = 44u * 1024u * 1024u;
#else
constexpr uint32_t kLooperPoolBytes = 16u * 1024u * 1024u;
#endif
#else
constexpr uint32_t kLooperPoolBytes = 0;
#endif

#endif
