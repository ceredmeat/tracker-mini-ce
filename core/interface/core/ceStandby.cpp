// tracker-mini-ce: Standby screen (see ceStandby.h).
//
// While standby is on, the display draws a full-screen frame from paint() instead of the
// controls (cDisplay::setFullScreenPainter); the screens keep updating underneath, so on
// wake-up they come straight back. The picture is decoded by the FT812 (the same loader
// and file check as the startup picture) into RAM_G above every control in use. On the
// Looper and the image viewer, whose own pictures live in that area, standby goes black
// instead.

#include "ceStandby.h"

#ifdef PLATFORM_IMX

#include <stdio.h>
#include <string.h>

#include <memory>
#include <new>

#include "Platform.h"
#include "ThemeManager.h"
#include "TrackerImpl.h"
#include "configEditor/configMenuActions.h"
#include "core/interface.h"
#include "core/splashScreen.h"
#include "FT812.h"
#include "display.h"
#include "imageViewer/imageViewer.h"
#include "mtStructs.h"

#if __has_include("ceLooperConfig.h")
#include "ceLooperConfig.h"
#endif
#ifdef CE_LOOPER
#include "looper/looper.h"
#endif

namespace ceStandby
{
uint8_t timeoutSetting = 0;
uint8_t modeSetting = 0;

namespace
{
constexpr char kFolder[] = "/Images/Standby";
constexpr char kCfgPath[] = "/Images/Standby/standby.cfg";
constexpr uint32_t kMinutes[6] = { 0, 1, 2, 5, 10, 30 };
constexpr uint32_t kRamGLimit = 1000000;
constexpr uint32_t kSpareSlots = 3;
constexpr uint32_t kLoadTimeoutMs = 4000;
constexpr uint32_t kKnobSwallowMs = 400;
constexpr int kMaxFiles = 32;

enum class State : uint8_t
{
    kOff,     // normal use
    kLoading, // standby, picture on its way (black meanwhile)
    kPicture, // standby, picture drifting
    kBlack,   // standby, black
};

State state = State::kOff;
bool loaded = false;
uint32_t lastInputMs = 0;
uint32_t wokeMs = 0;
int swallowButton = -1;
int swallowPad = -1;
uint8_t nextPicture = 0;

uint32_t loadStartMs = 0;
uint32_t address = 0;
uint16_t picW = 0, picH = 0;
float posX = 0, posY = 0, velX = 0.35f, velY = 0.22f;

uint32_t now()
{
    return TrackerImpl::getInstance()->getPlatform()->getTimer()->millis();
}

FileSystem* fs()
{
    return TrackerImpl::getInstance()->getPlatform()->getFileSystem();
}

void loadSettings()
{
    loaded = true;
    if (!fs()->exists(kCfgPath)) return;
    auto [ok, h] = fs()->openFile(kCfgPath, FileSystem::kRead);
    if (!ok) return;
    uint8_t b[6];
    if (fs()->readFile(b, sizeof(b), h) == sizeof(b) && memcmp(b, "STB1", 4) == 0)
    {
        timeoutSetting = b[4] < 6 ? b[4] : 0;
        modeSetting = b[5] < 2 ? b[5] : 0;
    }
    fs()->closeFile(h);
}

void paint()
{
    if (state != State::kPicture) return; // black
    API_BITMAP_HANDLE(13);
    API_CMD_SETBITMAP(address, RGB565, picW, picH);
    API_COLOR(0xD0D0D0); // a little darker than full
    API_BEGIN(BITMAPS);
    API_VERTEX2F((int16_t)posX, (int16_t)posY);
    API_END();
}

// the n-th .jpg / .jpeg in the folder (name order), without its extension
bool pictureName(uint8_t n, char* out, int len, char* full, int fullLen)
{
    // the list is only needed here: on the heap for a moment, not in on-chip RAM for good
    struct Lists
    {
        char names[kMaxFiles * 40];
        char* list[kMaxFiles];
    };
    std::unique_ptr<Lists> L(new (std::nothrow) Lists);
    if (!L) return false;
    char* names = L->names;
    char** list = L->list;
    int count = kMaxFiles;
    fs()->listDirContents(
      kFolder,
      FileSystem::kFiles,
      [](const char* name) {
          int l = strlen(name);
          if (name[0] == '.') return false;
          return (l > 4 && strcasecmp(name + l - 4, ".jpg") == 0) ||
                 (l > 5 && strcasecmp(name + l - 5, ".jpeg") == 0);
      },
      names,
      sizeof(L->names),
      38,
      count,
      list);
    if (count <= 0) return false;
    const char* name = list[n % count];
    snprintf(full, fullLen, "%s/%s", kFolder, name);
    snprintf(out, len, "%s/%s", kFolder, name);
    char* dot = strrchr(out, '.');
    if (dot != nullptr) *dot = 0; // readImgFromSd adds the extension
    return true;
}

bool pictureAllowed()
{
    ModuleBase* m = mtInterface.getOnScreenModule();
    if (m == &imageViewer) return false;
#ifdef CE_LOOPER
    if (m == &looper) return false;
#endif
    return true;
}

void goBlack()
{
    state = State::kBlack;
    display.setBacklightBrightness(3);
}

void enter()
{
    display.setFullScreenPainter(paint);
    if (modeSetting != 0 || !pictureAllowed() || display.getImgLoadState() == 1 ||
        display.getImgLoadState() == 2)
    {
        goBlack();
        return;
    }

    // above the display lists of every control in use (plus a few spare)
    int slot = display.highestUsedControlSlot();
    address = controlsRamStartAddress + (uint32_t)(slot + 1 + kSpareSlots) * controlsRamAddressStep;
    address = (address + 15) & ~15u;
    if (address >= kRamGLimit)
    {
        goBlack();
        return;
    }
    char path[80], full[80];
    uint16_t w = 0, h = 0, padW = 0, padH = 0;
    bool found = false;
    for (int tries = 0; tries < 3 && !found; tries++)
    {
        if (!pictureName(nextPicture++, path, sizeof(path), full, sizeof(full))) break;
        // a decodable JPEG whose decoded size (in whole blocks) fits the screen and RAM_G
        found = splash::checkJpeg(full, 800, 480, w, h, padW, padH) &&
                address + (uint32_t)padW * padH * 2 <= kRamGLimit;
    }
    if (!found || display.readImgFromSd(path, address, 800, 480) != 0)
    {
        goBlack();
        return;
    }
    picW = w;
    picH = h;
    posX = (800 - picW) / 2.0f;
    posY = (480 - picH) / 2.0f;
    loadStartMs = now();
    state = State::kLoading;
}

void leave()
{
    if (state == State::kLoading)
    {
        display.abortImageLoad();
        ThemeManager::applyFont(mtConfig.general.font); // a co-processor reset forgets them
    }
    state = State::kOff;
    display.setFullScreenPainter(nullptr);
    setDisplayBrightness(); // back to Config's brightness
}

bool wake()
{
    lastInputMs = now();
    if (state == State::kOff) return false;
    leave();
    wokeMs = lastInputMs;
    return true;
}
} // namespace

void settingsChanged()
{
    FileSystem* f = fs();
    f->mkdir(0, kFolder);
    if (f->exists(kCfgPath)) f->remove(kCfgPath);
    auto [ok, h] = f->openFile(kCfgPath, FileSystem::kWriteCreateNew);
    if (ok)
    {
        uint8_t b[6] = { 'S', 'T', 'B', '1', timeoutSetting, modeSetting };
        f->writeFile(b, sizeof(b), h);
        f->closeFile(h);
    }
    lastInputMs = now();
}

bool isActive()
{
    return state != State::kOff;
}

void activity()
{
    wake();
}

bool buttonEvent(uint8_t button, uint8_t st)
{
    if (swallowButton == button)
    {
        if (st == 0) swallowButton = -1; // released
        lastInputMs = now();
        return true;
    }
    if (wake())
    {
        if (st != 0) swallowButton = button;
        return true;
    }
    return false;
}

bool padEvent(uint8_t pad, uint8_t st)
{
    if (swallowPad == pad)
    {
        if (st == 0) swallowPad = -1;
        lastInputMs = now();
        return true;
    }
    if (wake())
    {
        if (st != 0) swallowPad = pad;
        return true;
    }
    return false;
}

bool knobEvent()
{
    if (wake()) return true;
    return now() - wokeMs < kKnobSwallowMs; // the rest of the turn that woke it
}

void update()
{
    if (!loaded)
    {
        if (!mtInterface.isRunningNormally()) return; // the card is read once the tracker runs
        loadSettings();
        lastInputMs = now();
    }

    if (state == State::kOff)
    {
        if (timeoutSetting == 0 || timeoutSetting > 5 || !mtInterface.isRunningNormally())
        {
            lastInputMs = now();
            return;
        }
        if (now() - lastInputMs >= kMinutes[timeoutSetting] * 60000u) enter();
        return;
    }

    if (!mtInterface.isRunningNormally()) // e.g. USB storage, card out, power off
    {
        leave();
        lastInputMs = now();
        return;
    }

    if (state == State::kLoading)
    {
        uint8_t s = display.getImgLoadState();
        if (s == 3)
        {
            if (getProps.Width == picW && getProps.Height == picH &&
                getProps.End_Address > address && getProps.End_Address <= kRamGLimit)
                state = State::kPicture;
            else
                goBlack();
        }
        else if (s == 4 || s == 0)
        {
            if (s == 4) ThemeManager::applyFont(mtConfig.general.font);
            goBlack();
        }
        else if (now() - loadStartMs > kLoadTimeoutMs)
        {
            display.abortImageLoad();
            ThemeManager::applyFont(mtConfig.general.font);
            goBlack();
        }
        return;
    }

    if (state == State::kPicture)
    {
        // a slow drift (about 10 pixels a second), bouncing off the edges
        static uint32_t last = 0;
        uint32_t t = now();
        if (t - last < 40) return;
        last = t;
        float maxX = 800.0f - picW, maxY = 480.0f - picH;
        posX += velX;
        posY += velY;
        if (posX < 0 || posX > maxX)
        {
            velX = -velX;
            posX = posX < 0 ? 0 : maxX;
        }
        if (posY < 0 || posY > maxY)
        {
            velY = -velY;
            posY = posY < 0 ? 0 : maxY;
        }
    }
}
} // namespace ceStandby

#else

namespace ceStandby
{
uint8_t timeoutSetting = 0;
uint8_t modeSetting = 0;
void settingsChanged() {}
void update() {}
bool buttonEvent(uint8_t, uint8_t) { return false; }
bool padEvent(uint8_t, uint8_t) { return false; }
bool knobEvent() { return false; }
void activity() {}
bool isActive() { return false; }
} // namespace ceStandby

#endif
