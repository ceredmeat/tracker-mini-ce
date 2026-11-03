#ifndef INTERFACE_LOOPER_H_
#define INTERFACE_LOOPER_H_

// tracker-mini-ce: the Looper screen. A four-track looper in the style of Retrack
// (github.com/math0ne/retrack-tracker), rebuilt for the Mini inside this firmware: lanes
// with waveforms, the mode bar, the grain menu, the mixer, two send effects, performance
// mode, a loop editor, the input / tempo page, settings and projects.

#include "ceLooperConfig.h"
#include "core/modulesBase.h"

#ifdef CE_LOOPER

#include "ceLooper/lp_engine.h"
#include "looperView.h"

class cLooper : public ModuleBase
{
  public:
    virtual void update();
    virtual void start(uint32_t options);
    virtual void stop();
    virtual void initDisplayControls();
    virtual void destroyDisplayControls();

    enum Page : uint8_t
    {
        kHome,
        kMixer,
        kFx,
        kInput,
        kConfig,
        kFile,
        kEditor,
        kManual,
    };

    enum KeyBar : uint8_t
    {
        kBarMain,
        kBarModes,
        kBarGrain,
        kBarFxType,
    };

    enum Overlay : uint8_t
    {
        kOvNone,
        kOvTempo,
        kOvVolume,
        kOvGain,
        kOvPerf,
        kOvThreshold,
        kOvLength,
    };

    // ---- state ----
    Page page = kHome;
    KeyBar bar = kBarMain;
    uint8_t sel = 0;            // selected track
    bool shift = false;
    bool perform = false;
    bool performHeld = false;   // the Perform key is held (momentary)
    uint8_t modePage = 0;
    uint8_t grainKnob = 0;
    uint8_t fxSlot = 0;
    uint8_t fxKnob = 1;         // 0..7
    uint8_t fxTypePage = 0;
    uint8_t mixStrip = 0;       // 0..6
    uint8_t mixRow = 3;         // 0 send 1, 1 send 2 / chain, 2 filter, 3 level
    uint8_t inRow = 0, inCol = 0;
    uint8_t cfgRow = 0;
    uint8_t fileRow = 0;
    uint8_t manualPage = 0;
    Overlay overlay = kOvNone;
    int8_t perfKnobFx = -1;     // overlay: which perf effect's knob
    int8_t heldKey = -1;        // F key held
    uint32_t heldSince = 0;
    bool heldTurned = false;
    bool heldShift = false;
    int8_t heldPad[lp::kTracks];   // perf: pad held per track (fx index) for knob turns
    bool showStats = false;
    char message[48];
    uint32_t messageUntil = 0;

    // editor
    uint8_t edTrack = 0;
    uint32_t edA = 0, edB = 0;
    uint8_t edMarker = 0;
    uint8_t edSnap = 0;         // bar, beat, 1/16, free
    int8_t edConfirm = -1;      // 5 = cut, 6 = erase pending
    uint32_t edVersion = 0xffffffff;

    // tap tempo
    uint32_t taps[4];
    uint8_t tapCount = 0;

    // project names
    char projectName[24];
    uint8_t projectIdx = 0;
    uint8_t projectCount = 0;
    char (*projectList)[24] = nullptr;   // 16 names, in the SDRAM

    // waveform cache
    uint32_t laneVersion[lp::kTracks];
    uint32_t laneSpan[lp::kTracks];
    uint32_t laneDrawnMs[lp::kTracks];
    bool laneValid[lp::kTracks];
    uint8_t laneNext = 0;
    uint32_t lastFrameMs = 0;

    // controls
    hControl bgCtl = -1, laneCtl[lp::kTracks] = {-1, -1, -1, -1}, keysCtl = -1, overlayCtl = -1;
    cLooperView::Painter bgPainter, lanePainter[lp::kTracks], keysPainter, overlayPainter;

    // the Mini's Fn keys on the Looper's screen: their own bindings (Fn1-4, Shift + Fn1-4),
    // saved in /Looper/keys.cfg; the tracker's bindings are untouched
    enum FnAction : uint8_t
    {
        kFnLoops,
        kFnMixer,
        kFnFx1,
        kFnFx2,
        kFnPerform,
        kFnEditor,
        kFnInput,
        kFnProjects,
        kFnSettings,
        kFnManual,
        kFnModes,
        kFnUndo,
        kFnTap,
        kFnMetronome,
        kFnPlayAll,
        kFnTracker,
        kFnActions
    };
    static const char* const kFnActionNames[kFnActions];
    static const uint8_t kFnDefaults[8];
    uint8_t fnBinding[8];
    bool fnLoaded = false;
    void fnAction(int action, bool press);
    void loadFnKeys();
    void saveFnKeys();

    // ---- helpers ----
    void setFunctions();
    void showPage(Page p);
    void refreshAll();
    void refreshKeys();
    void flash(const char* text);
    uint32_t now();

    void keyPress(int k);
    void keyRelease(int k);
    void encoder(int16_t v);
    void pad(uint8_t pad, uint8_t state);
    void selectTrack(int t);
    void tapTempo();

    void homeKey(int k, bool press);
    void mixerKey(int k, bool press);
    void fxKey(int k, bool press);
    void inputKey(int k, bool press);
    void configKey(int k, bool press);
    void fileKey(int k, bool press);
    void editorKey(int k, bool press);
    void editorSnapMarkers();
    uint32_t editorStep() const;

    void inputChange(int row, int col, int delta);
    void configChange(int row, int delta);
    void mixerChange(int delta, bool fine);

    // drawing (gLooper.cpp)
    void renderLane(int t, uint8_t* buf, int w, int h, uint32_t span);
    void uploadLanes();
    void uploadEditor();
    void keyText(int k, const char*& name, char* value, int valueLen, uint8_t& flags);

    // files (looperFiles.cpp)
    void scanProjects();
    bool saveProject();
    bool loadProject(const char* name);
    bool exportTracks();
    bool loadWav(int track, const char* path);
    void newProjectName();
};

extern cLooper looper;

#endif
#endif
