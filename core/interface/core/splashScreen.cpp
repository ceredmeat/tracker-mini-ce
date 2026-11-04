// tracker-mini-ce: custom startup picture. See splashScreen.h.

#include "splashScreen.h"

#ifdef PLATFORM_IMX

#include <stdint.h>
#include <string.h>

#include "Platform.h"
#include "ThemeManager.h"
#include "TrackerImpl.h"
#include "display.h"
#include "mtStructs.h"
#include "startScreenControl.h"

namespace splash
{

static constexpr char kPath[] = "/Images/Startup/splash.jpg";
static constexpr char kPathNoExt[] = "/Images/Startup/splash"; // readImgFromSd adds ".jpg"
static constexpr uint32_t kMaxFileBytes = 650u * 1024u;        // the image viewer's limit
static constexpr uint16_t kMaxWidth = 800;
static constexpr uint16_t kMaxHeight = 416;     // what fits in RAM_G during boot
static constexpr uint32_t kRamGLimit = 1000000; // CE keeps images below this (PNG scratch above)
static constexpr uint32_t kSpareSlots = 3;      // control slots kept free above those in use
static constexpr uint32_t kTimeoutMs = 3000;

enum class State : uint8_t
{
    kIdle,
    kLoading,
    kShown,
    kFailed,
};

static State state = State::kIdle;
static uint32_t startMs = 0;
static uint16_t width = 0, height = 0;
static uint32_t address = 0;

static uint32_t now()
{
    return TrackerImpl::getInstance()->getPlatform()->getTimer()->millis();
}

// Reads the whole file once and accepts only what the FT812 decodes for sure: a baseline
// (SOF0) 8-bit JPEG, grayscale or YCbCr 4:4:4 / 4:2:2 / 4:2:0, one scan, ending with EOI,
// no progressive / lossless / arithmetic coding. Fills the picture size and its size in
// whole JPEG blocks (what the decoder writes).
static bool checkFile(FileSystem* fs, const char* path, uint16_t maxW, uint16_t maxH, uint16_t& w,
                      uint16_t& h, uint16_t& padW, uint16_t& padH)
{
    if (!fs->exists(path))
    {
        return false;
    }
    auto [isOpen, file] = fs->openFile(path, FileSystem::FileOpenMode::kRead);
    if (!isOpen)
    {
        return false;
    }

    bool ok = false;
    uint32_t size = fs->getFileSize(file);
    uint8_t b[24];
    uint32_t pos = 0;
    bool sof = false, dht = false, dqt = false;
    uint8_t components = 0;

    auto read = [&](uint8_t* dst, uint32_t n)
    {
        if (pos + n > size || fs->readFile(dst, n, file) != n)
        {
            return false;
        }
        pos += n;
        return true;
    };

    do
    {
        if (size < 128 || size > kMaxFileBytes)
            break;
        if (!read(b, 2) || b[0] != 0xFF || b[1] != 0xD8)
            break; // not a JPEG

        bool header = true, bad = false;
        while (header && !bad)
        {
            // a marker: FF (fill FFs allowed), then its code
            if (!read(b, 2) || b[0] != 0xFF)
            {
                bad = true;
                break;
            }
            uint8_t m = b[1];
            while (m == 0xFF)
            {
                if (!read(&m, 1))
                {
                    bad = true;
                    break;
                }
            }
            if (bad)
                break;
            if (!read(b, 2))
            {
                bad = true;
                break;
            }
            uint16_t len = (uint16_t)(b[0] << 8 | b[1]);
            if (len < 2 || pos + len - 2 > size)
            {
                bad = true;
                break;
            }
            uint16_t body = len - 2;

            if ((m >= 0xE0 && m <= 0xEF) || m == 0xFE || m == 0xDD)
            {
                // APPn, comments, restart interval: skipped
                if (!fs->seekFileCur(body, file))
                {
                    bad = true;
                    break;
                }
                pos += body;
            }
            else if (m == 0xDB || m == 0xC4)
            {
                (m == 0xDB ? dqt : dht) = true;
                if (!fs->seekFileCur(body, file))
                {
                    bad = true;
                    break;
                }
                pos += body;
            }
            else if (m == 0xC0)
            {
                // baseline frame: precision, height, width, components
                if (sof || body < 6 || body > sizeof(b) || !read(b, body))
                {
                    bad = true;
                    break;
                }
                sof = true;
                components = b[5];
                h = (uint16_t)(b[1] << 8 | b[2]);
                w = (uint16_t)(b[3] << 8 | b[4]);
                if (b[0] != 8 || (components != 1 && components != 3) ||
                    body != 6 + 3 * components || w == 0 || h == 0)
                {
                    bad = true;
                    break;
                }
                uint8_t hmax = 1, vmax = 1;
                for (uint8_t c = 0; c < components; c++)
                {
                    uint8_t hs = b[7 + 3 * c] >> 4, vs = b[7 + 3 * c] & 15;
                    if (c == 0 && components == 3)
                    {
                        bool usual =
                          (hs == 1 && vs == 1) || (hs == 2 && vs == 1) || (hs == 2 && vs == 2);
                        if (!usual)
                            bad = true;
                        hmax = hs;
                        vmax = vs;
                    }
                    else if (components == 3 && (hs != 1 || vs != 1))
                        bad = true;
                }
                if (bad)
                    break;
                uint16_t bw = 8 * hmax, bh = 8 * vmax;
                padW = (uint16_t)((w + bw - 1) / bw * bw);
                padH = (uint16_t)((h + bh - 1) / bh * bh);
                if (padW > maxW || padH > maxH)
                {
                    bad = true;
                    break;
                }
            }
            else if (m == 0xDA)
            {
                // the scan: everything else must be in place, one scan with all components
                if (!sof || !dht || !dqt || body < 6 || body > sizeof(b) || !read(b, body) ||
                    b[0] != components)
                {
                    bad = true;
                    break;
                }
                header = false;
            }
            else
            {
                // progressive (C2), extended, lossless, arithmetic, hierarchical, or anything
                // else this check doesn't know: not for the decoder
                bad = true;
            }
        }
        if (bad || header)
            break;

        // the coded data up to the end: FF only as FF 00 (a stuffed byte) or a restart
        // marker; the file must end exactly with EOI (FF D9)
        uint8_t chunk[1024]; // on the stack: only used once, at boot
        bool prevFF = false, eoi = false;
        while (pos < size && !bad && !eoi)
        {
            uint32_t n = size - pos < sizeof(chunk) ? size - pos : sizeof(chunk);
            if (fs->readFile(chunk, n, file) != n)
            {
                bad = true;
                break;
            }
            for (uint32_t i = 0; i < n; i++)
            {
                uint8_t v = chunk[i];
                if (prevFF)
                {
                    if (v == 0xD9)
                    {
                        eoi = true;
                        if (pos + i + 1 != size)
                            bad = true; // something after the end
                        break;
                    }
                    if (v != 0x00 && !(v >= 0xD0 && v <= 0xD7) && v != 0xFF)
                    {
                        bad = true; // another marker: a second scan, tables mid-file, ...
                        break;
                    }
                    prevFF = (v == 0xFF);
                }
                else
                    prevFF = (v == 0xFF);
            }
            pos += n;
        }
        ok = !bad && eoi;
    } while (false);

    fs->closeFile(file);
    return ok;
}

static void fail(bool coProReset)
{
    state = State::kFailed;
    startScreenSplash.show = false;
    if (coProReset)
    {
        // a co-processor reset forgets the fonts set up through it: set them up again
        ThemeManager::applyFont(mtConfig.general.font);
    }
}

void begin()
{
    if (state != State::kIdle)
        return;
    startScreenSplash.show = false;

    FileSystem* fs = TrackerImpl::getInstance()->getPlatform()->getFileSystem();
    if (!fs->isSdCardInserted())
    {
        state = State::kFailed;
        return;
    }

    uint16_t w = 0, h = 0, padW = 0, padH = 0;
    if (!checkFile(fs, kPath, kMaxWidth, kMaxHeight, w, h, padW, padH))
    {
        state = State::kFailed; // no file, or one the decoder shouldn't see: the logo
        return;
    }

    // above the display lists of every control in use (plus a few spare)
    int slot = display.highestUsedControlSlot();
    uint32_t addr =
      controlsRamStartAddress + (uint32_t)(slot + 1 + kSpareSlots) * controlsRamAddressStep;
    addr = (addr + 15) & ~15u;
    if (addr + (uint32_t)padW * padH * 2 > kRamGLimit || display.getImgLoadState() == 1 ||
        display.getImgLoadState() == 2)
    {
        state = State::kFailed;
        return;
    }

    char path[40];
    strcpy(path, kPathNoExt);
    if (display.readImgFromSd(path, addr, kMaxWidth, kMaxHeight) != 0)
    {
        state = State::kFailed; // e.g. no JFIF header (CE's loader needs one)
        return;
    }
    width = w;
    height = h;
    address = addr;
    startMs = now();
    state = State::kLoading;
}

void update()
{
    if (state != State::kLoading)
        return;

    uint8_t s = display.getImgLoadState();
    if (s == 3)
    {
        // decoded: the FT812 reports the picture's size; anything else means trouble
        if (getProps.Width == width && getProps.Height == height &&
            getProps.End_Address > address && getProps.End_Address <= kRamGLimit)
        {
            startScreenSplash.address = address;
            startScreenSplash.width = width;
            startScreenSplash.height = height;
            startScreenSplash.show = true;
            state = State::kShown;
        }
        else
            fail(false);
        return;
    }
    if (s == 4 || s == 0)
    {
        fail(s == 4); // stopped by a co-processor reset, or the file ended early
        return;
    }
    if (now() - startMs > kTimeoutMs)
    {
        display.abortImageLoad();
        fail(true);
    }
}

bool busy()
{
    return state == State::kLoading;
}

bool checkJpeg(const char* path, uint16_t maxW, uint16_t maxH, uint16_t& w, uint16_t& h,
               uint16_t& padW, uint16_t& padH)
{
    FileSystem* fs = TrackerImpl::getInstance()->getPlatform()->getFileSystem();
    return checkFile(fs, path, maxW, maxH, w, h, padW, padH);
}

void end()
{
    if (state == State::kLoading)
    {
        display.abortImageLoad(); // e.g. the card was taken out
        fail(true);
    }
    startScreenSplash.show = false;
    // the picture's RAM_G area is free again for the screens that follow
}

} // namespace splash

#else

namespace splash
{
void begin()
{
}
void update()
{
}
bool busy()
{
    return false;
}
void end()
{
}
bool checkJpeg(const char*, uint16_t, uint16_t, uint16_t&, uint16_t&, uint16_t&, uint16_t&)
{
    return false;
}
} // namespace splash

#endif
