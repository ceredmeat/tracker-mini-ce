// tracker-mini-ce: the Looper screen, keys and logic. See looper.h.

#include "looper.h"

#ifdef CE_LOOPER

#include <stdio.h>
#include <string.h>

#include "AudioLooper.h"
#include "core/interfaceMenuPopup.h"
#include "core/songTimer.h"
#include "mtPadBoard.h"
#include "mtPadsBacklight.h"
#include "cr_section_macros.h"

using namespace lp;

cLooper looper;
static cLooper* LP = &looper;

static void functFKey(uint8_t button, uint8_t state);
static void functShift(uint8_t state);
static void functPlay(uint8_t state);
static void functRec(uint8_t state);
static void functInsert(uint8_t state);
static void functDelete(uint8_t state);
static void functCopy(uint8_t state);
static void functUp(uint8_t state);
static void functDown(uint8_t state);
static void functLeft(uint8_t state);
static void functRight(uint8_t state);
static void functEncoder(int16_t value);
static void functPads(uint8_t pad, uint8_t state, int16_t velo);
static void functMenu(uint8_t state);
static void functMiddle(uint8_t state);
static void functPageKey(uint8_t button, uint8_t state);

static constexpr uint32_t kHoldMs = 300;

uint32_t cLooper::now() { return platform->getTimer()->millis(); }

void cLooper::flash(const char* text)
{
    snprintf(message, sizeof(message), "%s", text);
    messageUntil = now() + 1300;
    display.refreshControl(overlayCtl);
}

//------------------------------------------------------------------------------

void cLooper::start(uint32_t options)
{
    (void)options;
    moduleRefresh = 1;
    AudioLooper::begin();
    AudioLooper::screenOpen = true;
    titleBar->hide();
    songTimer.hide();
    for (int t = 0; t < kTracks; t++)
    {
        laneValid[t] = false;
        heldPad[t] = -1;
    }
    if (projectName[0] == 0) newProjectName();
    edVersion = 0xffffffff;
    setFunctions();
    showPage(page);
}

void cLooper::stop()
{
    moduleRefresh = 0;
    AudioLooper::screenOpen = false;
    if (perform)
    {
        perform = false;
        engine.perfClear();
    }
    overlay = kOvNone;
    heldKey = -1;
    for (int t = 0; t < kTracks; t++)
        display.setControlHide(laneCtl[t]);
    display.setControlHide(keysCtl);
    display.setControlHide(overlayCtl);
    display.setControlHide(bgCtl);
    titleBar->show();
    songTimer.show();
    padsBacklight.clearGlobalPads(1, 1, 1);
}

void cLooper::setFunctions()
{
    ip->clearAll();
    for (int b = interfaceButton0; b <= interfaceButton7; b++)
        ip->setButtonObj(b, functFKey);
    ip->setButtonObj(interfaceButtonShift, functShift);
    ip->setButtonObj(interfaceButtonPlay, functPlay);
    ip->setButtonObj(interfaceButtonRec, functRec);
    ip->setButtonObj(interfaceButtonInsert, functInsert);
    ip->setButtonObj(interfaceButtonDelete, functDelete);
    ip->setButtonObj(interfaceButtonCopy, functCopy);
    ip->setButtonObj(interfaceButtonUp, functUp);
    ip->setButtonObj(interfaceButtonDown, functDown);
    ip->setButtonObj(interfaceButtonLeft, functLeft);
    ip->setButtonObj(interfaceButtonRight, functRight);
    ip->setPotObj(interfacePot0, functEncoder, false);
    ip->setPadsGlobal(functPads);
    ip->setButtonObj(interfaceButtonMiniMenu, functMenu);
    ip->setButtonObj(interfaceButtonMiniMiddle, functMiddle);

    static const uint8_t pageKeys[] = {
        interfaceButtonMaster,     interfaceButtonFx1,        interfaceButtonFx2,  interfaceButtonNote,
        interfaceButtonSampleEdit, interfaceButtonPerformance, interfaceButtonInstr, interfaceButtonParams,
        interfaceButtonPattern,    interfaceButtonConfig,      interfaceButtonFile, interfaceButtonSong,
        interfaceButtonSampleLoad, interfaceButtonMiniFn1,     interfaceButtonMiniFn2, interfaceButtonMiniFn3,
        interfaceButtonMiniFn4,    interfaceButtonSamplePlay,  interfaceButtonSampleRec,
    };
    for (uint8_t k : pageKeys)
        ip->setButtonObj(k, functPageKey);
}

void cLooper::showPage(Page p)
{
    page = p;
    if (p != kHome) bar = kBarMain;
    if (p == kFx) bar = kBarMain;
    overlay = kOvNone;
    heldKey = -1;
    bool lanes = (p == kHome);
    for (int t = 0; t < kTracks; t++)
    {
        if (lanes)
            display.setControlShow(laneCtl[t]);
        else
            display.setControlHide(laneCtl[t]);
        laneValid[t] = laneValid[t] && lanes;
    }
    display.setControlShow(bgCtl);
    display.setControlShow(keysCtl);
    display.setControlShow(overlayCtl);
    if (p == kEditor)
    {
        edTrack = sel;
        Track& t = engine.track[edTrack];
        edA = 0;
        edB = t.len;
        edConfirm = -1;
        edVersion = 0xffffffff;
    }
    if (p == kFile) scanProjects();
    refreshAll();
}

void cLooper::refreshAll()
{
    display.refreshControl(bgCtl);
    for (int t = 0; t < kTracks; t++)
        display.refreshControl(laneCtl[t]);
    display.refreshControl(keysCtl);
    display.refreshControl(overlayCtl);
    display.synchronizeRefresh();
}

void cLooper::refreshKeys()
{
    display.refreshControl(keysCtl);
    display.refreshControl(overlayCtl);
}

void cLooper::selectTrack(int t)
{
    t = clampi(t, 0, kTracks - 1);
    if (t == sel) return;
    sel = (uint8_t)t;
    engine.selected = sel;
    if (bar == kBarGrain && engine.track[sel].mode != kModeGranular) bar = kBarMain;
    if (bar == kBarModes) bar = kBarMain;
    for (int i = 0; i < kTracks; i++)
        laneValid[i] = laneValid[i];   // tint changes only: no new upload
    refreshAll();
}

//------------------------------------------------------------------------------
// the loop

void cLooper::update()
{
    uint32_t ms = now();

    // held F keys turn into knobs after a moment
    if (heldKey >= 0 && overlay == kOvNone && ms - heldSince >= kHoldMs)
    {
        if (page == kHome && !perform)
        {
            if (!heldShift)
            {
                if (heldKey == 3) overlay = kOvTempo;
                if (heldKey == 5) overlay = kOvGain;
                if (heldKey == 6) overlay = kOvVolume;
                if (heldKey == 4 && engine.track[sel].mode == kModeGranular)
                {
                    bar = kBarGrain;
                    heldKey = -1;   // the grain menu stays open
                    refreshKeys();
                }
            }
            else
            {
                if (heldKey == 4) overlay = kOvLength;
                if (heldKey == 5) overlay = kOvThreshold;
            }
        }
        else if (page == kHome && perform)
        {
            overlay = kOvPerf;
            perfKnobFx = heldKey;
        }
        if (overlay != kOvNone) display.refreshControl(overlayCtl);
    }

    if (messageUntil && ms > messageUntil)
    {
        messageUntil = 0;
        display.refreshControl(overlayCtl);
    }

    // about 25 frames a second: playheads, meters, grains
    if (ms - lastFrameMs >= 40)
    {
        lastFrameMs = ms;
        if (page == kHome)
        {
            uploadLanes();
            for (int t = 0; t < kTracks; t++)
                display.refreshControl(laneCtl[t]);
            display.refreshControl(keysCtl);
            if (overlay != kOvNone) display.refreshControl(overlayCtl);
        }
        else
        {
            if (page == kEditor) uploadEditor();
            display.refreshControl(bgCtl);
            display.refreshControl(keysCtl);
        }
    }
}

//------------------------------------------------------------------------------
// keys

void cLooper::keyPress(int k)
{
    heldKey = (int8_t)k;
    heldSince = now();
    heldTurned = false;
    heldShift = shift;
    switch (page)
    {
        case kHome: homeKey(k, true); break;
        case kMixer: mixerKey(k, true); break;
        case kFx: fxKey(k, true); break;
        case kInput: inputKey(k, true); break;
        case kConfig: configKey(k, true); break;
        case kFile: fileKey(k, true); break;
        case kEditor: editorKey(k, true); break;
        case kManual:
            if (k == 7) showPage(kHome);
            else if (k == 0 && manualPage > 0) manualPage--;
            else if (k == 1) manualPage++;
            break;
    }
    refreshKeys();
}

void cLooper::keyRelease(int k)
{
    if (page == kHome) homeKey(k, false);
    if (heldKey == k)
    {
        heldKey = -1;
        if (overlay != kOvNone)
        {
            overlay = kOvNone;
            perfKnobFx = -1;
        }
    }
    refreshKeys();
}

void cLooper::homeKey(int k, bool press)
{
    Track& t = engine.track[sel];

    // performance mode: the keys play the effects on every track
    if (perform && bar == kBarMain)
    {
        if (press)
        {
            if (shift)
            {
                for (int i = 0; i < kTracks; i++)
                    if (engine.track[i].len) engine.perfLatch(i, k);
            }
            else
                for (int i = 0; i < kTracks; i++)
                    if (engine.track[i].len) engine.perfHold(i, k, true);
        }
        else if (!heldShift)
            for (int i = 0; i < kTracks; i++)
                engine.perfHold(i, k, false);
        return;
    }

    if (bar == kBarModes)
    {
        if (!press) return;
        if (modePage == 0)
        {
            if (k < 7)
            {
                engine.setMode(sel, k);
                bar = kBarMain;
            }
            else
                modePage = 1;
        }
        else
        {
            if (k < 4)
            {
                engine.setMode(sel, kModeHalfTime + k);
                bar = kBarMain;
            }
            else if (k == 7)
                modePage = 0;
        }
        heldKey = -1;
        return;
    }

    if (bar == kBarGrain)
    {
        if (!press) return;
        static const int8_t knobOf[8] = {0, 1, 2, 3, -1, 4, 5, -1};
        if (k == 4)
            bar = kBarMain;
        else if (knobOf[k] >= 0)
            grainKnob = (uint8_t)knobOf[k];
        heldKey = -1;
        return;
    }

    if (!press)
    {
        // taps (released before turning into a knob)
        if (heldKey != k || heldTurned || overlay != kOvNone) return;
        if (heldShift) return;
        if (k == 4)
        {
            bar = kBarModes;
            modePage = t.mode >= kModeHalfTime ? 1 : 0;
        }
        else if (k == 5)
            showPage(kInput);
        else if (k == 6)
        {
            mixStrip = sel;
            mixRow = 3;
            showPage(kMixer);
        }
        return;
    }

    if (!shift)
    {
        switch (k)
        {
            case 0:
                if (t.len && t.mode > kModePingPong && t.state != TrackState::kOverdub)
                    flash("Overdub plays Forward, Reverse or Ping-pong");
                else if (t.len && t.tempoOpt == kTempoStretch && engine.tempoPct() != 100.0f)
                    flash("Stretch tracks overdub at 100 %");
                engine.pressRec(sel);
                break;
            case 1: engine.pressPlay(sel); break;
            case 2: engine.pressMute(sel); break;
            case 7:
                if (t.undoAvail && !t.undone)
                {
                    engine.undo(sel);
                    flash("Undo");
                }
                else if (t.undoAvail && t.undone)
                {
                    engine.redo(sel);
                    flash("Redo");
                }
                break;
            default: break;   // 3 tempo, 4 mode, 5 input, 6 volume: hold or tap
        }
    }
    else
    {
        switch (k)
        {
            case 0: engine.pressReplace(sel); break;
            case 1:
            {
                bool anyPlaying = false;
                for (int i = 0; i < kTracks; i++)
                    if (engine.track[i].state == TrackState::kPlaying) anyPlaying = true;
                if (anyPlaying)
                {
                    for (int i = 0; i < kTracks; i++)
                        if (engine.track[i].state == TrackState::kPlaying) engine.pressPlay(i);
                }
                else
                    engine.playAll();
                break;
            }
            case 2:
            {
                // solo: mute the others; again: unmute all
                bool soloed = !engine.track[sel].muted;
                for (int i = 0; i < kTracks; i++)
                    if (i != sel && !engine.track[i].muted) soloed = false;
                for (int i = 0; i < kTracks; i++)
                    engine.track[i].muted = soloed ? false : (i != sel);
                break;
            }
            case 3: engine.cfg.metronome = !engine.cfg.metronome; break;
            case 4:
                if (t.fixedBars)
                    t.fixedBars = 0;
                else
                    t.stopOpt = (uint8_t)((t.stopOpt + 1) % 3);
                break;
            case 5: t.startOpt = (uint8_t)((t.startOpt + 1) % 3); break;
            case 6: t.afterRec = (uint8_t)((t.afterRec + 1) % 3); break;
            case 7: t.tempoOpt = (uint8_t)((t.tempoOpt + 1) % 2); break;
        }
    }
}

void cLooper::mixerKey(int k, bool press)
{
    if (!press) return;
    if (k == 7)
    {
        showPage(kHome);
        return;
    }
    if (shift)
    {
        if (k < 4)
            engine.track[k].muted = !engine.track[k].muted;
        else if (k < 6)
            engine.fxMute[k - 4] = !engine.fxMute[k - 4];
        else if (k == 6)
            engine.cfg.masterMute = !engine.cfg.masterMute;
    }
    else
    {
        if ((k == 4 || k == 5) && mixStrip == k)
        {
            // the selected FX strip again: its effect page
            fxSlot = (uint8_t)(k - 4);
            fxKnob = 1;
            showPage(kFx);
            return;
        }
        mixStrip = (uint8_t)k;
        if (mixStrip == 6) mixRow = 3;
        if (mixStrip >= 4 && mixRow == 2) mixRow = 3;
    }
}

void cLooper::mixerChange(int d, bool fine)
{
    auto adj = [&](uint8_t& v, int lo, int hi) { v = (uint8_t)clampi(v + d, lo, hi); };
    if (mixStrip < 4)
    {
        Track& t = engine.track[mixStrip];
        switch (mixRow)
        {
            case 0: adj(t.send[0], 0, 100); break;
            case 1: adj(t.send[1], 0, 100); break;
            case 2:
            {
                int v = t.djFilter + (fine ? d : d * 2);
                t.djFilter = (int8_t)clampi(v, -50, 50);
                break;
            }
            case 3: adj(t.volume, 0, 100); break;
        }
    }
    else if (mixStrip < 6)
    {
        int s = mixStrip - 4;
        switch (mixRow)
        {
            case 0: adj(engine.fx[s].knob[s == 0 && engine.fx[s].type() == kFxClouds ? 0 : 0], 0, 100); break;
            case 1:
                adj(engine.chainSend[s], 0, 100);
                break;
            default: adj(engine.fxReturn[s], 0, 100); break;
        }
    }
    else
        adj(engine.cfg.master, 0, 100);
}

void cLooper::fxKey(int k, bool press)
{
    if (!press) return;
    FxSlot& f = engine.fx[fxSlot];
    if (bar == kBarFxType)
    {
        if (k == 7)
        {
            fxTypePage ^= 1;
            return;
        }
        int ty = fxTypePage * 7 + k;
        if (ty < kFxTypes)
        {
            f.setType(ty);
            bar = kBarMain;
            fxKnob = 1;
        }
        return;
    }
    switch (k)
    {
        case 0:
            bar = kBarFxType;
            fxTypePage = f.type() >= 7 ? 1 : 0;
            break;
        case 1:
        case 2:
        case 3:
        case 4:
        {
            int kn = k - 1 + ((shift && kFxDesc[f.type()].knobs > 4) ? 4 : 0);
            if (kn < kFxDesc[f.type()].knobs)
            {
                fxKnob = (uint8_t)kn;
                if (f.type() == kFxClouds && kn == 7) f.knob[7] = f.knob[7] >= 50 ? 0 : 100;   // freeze: a toggle
            }
            break;
        }
        case 5: fxKnob = 8; break;
        case 6:
            fxSlot ^= 1;
            fxKnob = 1;
            break;
        case 7: showPage(kHome); break;
    }
}

//------------------------------------------------------------------------------
// input / tempo page

void cLooper::inputChange(int row, int col, int d)
{
    Config& c = engine.cfg;
    Track& t = engine.track[sel];
    if (col == 0)
    {
        switch (row)
        {
            case 0: c.inputGain = (uint8_t)clampi(c.inputGain + d, 0, 100); break;
            case 1:
                if (t.state == TrackState::kEmpty)
                    t.channels = (uint8_t)clampi(t.channels + d, 0, 3);
                else
                    flash("Channels: only while the track is empty");
                break;
            case 2: c.monitor = d > 0; break;
            case 3: c.feedback = (uint8_t)clampi(c.feedback + d, 0, 100); break;
        }
    }
    else
    {
        switch (row)
        {
            case 0: engine.setBpm(engine.currentBpm() + d); break;
            case 1:
                if (!engine.hasLoops())
                {
                    c.beatsPerBar = (uint8_t)clampi(c.beatsPerBar + d, 1, 12);
                    engine.setBpm(c.bpm);
                }
                else
                    flash("Beats / bar: before the first loop");
                break;
            case 2: c.metronome = d > 0; break;
            case 3: c.clickLevel = (uint8_t)clampi(c.clickLevel + d, 0, 100); break;
            case 4: c.clickSound = (uint8_t)clampi(c.clickSound + d, 0, 3); break;
            case 5: c.accent = d > 0; break;
            case 6: c.clickOnlyRec = d > 0; break;
            case 7:
            {
                static const uint8_t ci[4] = {0, 1, 2, 4};
                int i = 0;
                while (i < 3 && ci[i] != c.countInBars) i++;
                c.countInBars = ci[clampi(i + d, 0, 3)];
                break;
            }
            case 8: c.thresholdDb = (int8_t)clampi(c.thresholdDb + d, -60, 0); break;
            case 9: c.stopFade = (uint8_t)clampi(c.stopFade + d, 0, 3); break;
        }
    }
}

void cLooper::inputKey(int k, bool press)
{
    if (!press) return;
    if (k == 7) showPage(kHome);
    if (k == 0) inCol = 0;
    if (k == 1) inCol = 1;
}

void cLooper::configChange(int row, int d)
{
    switch (row)
    {
        case 0: engine.cfg.latency = (int16_t)clampi(engine.cfg.latency + d * 22, 0, 2205); break;   // 0.5 ms steps
        case 1: showStats = d > 0; break;
    }
}

void cLooper::configKey(int k, bool press)
{
    if (!press) return;
    if (k == 7) showPage(kHome);
}

void cLooper::fileKey(int k, bool press)
{
    if (!press) return;
    switch (k)
    {
        case 0:
            flash(saveProject() ? "Saved" : "Could not save");
            scanProjects();
            break;
        case 1:
            if (projectCount > 0 && fileRow < projectCount)
                flash(loadProject(projectList[fileRow]) ? "Loaded" : "Could not load");
            break;
        case 2: flash(exportTracks() ? "Exported to /Looper/Export" : "Could not export"); break;
        case 3: newProjectName(); break;
        case 7: showPage(kHome); break;
    }
}

//------------------------------------------------------------------------------
// editor

uint32_t cLooper::editorStep() const
{
    uint32_t beat = (uint32_t)(engine.beatLen() >> kFrac);
    switch (edSnap)
    {
        case 0: return (uint32_t)(engine.barLen() >> kFrac);
        case 1: return beat;
        case 2: return beat / 4;
        default: return (uint32_t)(kSampleRate / 100);   // 10 ms
    }
}

void cLooper::editorKey(int k, bool press)
{
    if (!press) return;
    Track& t = engine.track[edTrack];
    if (k != 4 && k != 5) edConfirm = -1;
    switch (k)
    {
        case 0: edMarker = 0; break;
        case 1: edMarker = 1; break;
        case 2: edSnap = (uint8_t)((edSnap + 1) % 4); break;
        case 3:
            if (engine.trim(edTrack, edA, edB))
            {
                edA = 0;
                edB = t.len;
                flash("Trimmed");
            }
            break;
        case 4:
        case 5:
            if (edConfirm != k)
            {
                edConfirm = (int8_t)k;
                flash(k == 4 ? "Cut: press again" : "Erase: press again");
            }
            else
            {
                bool ok = (k == 4) ? engine.cut(edTrack, edA, edB) : engine.erase(edTrack, edA, edB);
                edConfirm = -1;
                if (ok && k == 4)
                {
                    edB = edA;
                    if (edB > t.len) edB = t.len;
                }
                flash(ok ? (k == 4 ? "Cut" : "Erased") : "Nothing to do");
            }
            break;
        case 6:
            if (engine.untrim(edTrack))
            {
                edA = 0;
                edB = t.len;
                flash("Trim undone");
            }
            break;
        case 7: showPage(kHome); break;
    }
}

//------------------------------------------------------------------------------
// encoder

void cLooper::encoder(int16_t v)
{
    if (v == 0) return;
    int d = v > 0 ? 1 : -1;
    Track& t = engine.track[sel];

    if (heldKey >= 0)
    {
        heldTurned = true;
        if (page == kHome && !perform && bar == kBarMain)
        {
            if (!heldShift)
            {
                if (heldKey == 3)
                {
                    overlay = kOvTempo;
                    if (!engine.hasLoops())
                        engine.setBpm(engine.currentBpm() + d);
                    else
                        engine.setTempoPct(engine.tempoPct() + d);
                }
                else if (heldKey == 5)
                {
                    overlay = kOvGain;
                    engine.cfg.inputGain = (uint8_t)clampi(engine.cfg.inputGain + d, 0, 100);
                }
                else if (heldKey == 6)
                {
                    overlay = kOvVolume;
                    t.volume = (uint8_t)clampi(t.volume + d, 0, 100);
                }
            }
            else
            {
                if (heldKey == 3)
                    engine.cfg.clickLevel = (uint8_t)clampi(engine.cfg.clickLevel + d, 0, 100);
                else if (heldKey == 4)
                {
                    overlay = kOvLength;
                    t.fixedBars = (uint8_t)clampi(t.fixedBars + d, 0, 64 / engine.cfg.beatsPerBar);
                }
                else if (heldKey == 5)
                {
                    overlay = kOvThreshold;
                    engine.cfg.thresholdDb = (int8_t)clampi(engine.cfg.thresholdDb + d, -60, 0);
                }
            }
            refreshKeys();
            return;
        }
        if (page == kHome && perform)
        {
            overlay = kOvPerf;
            perfKnobFx = heldKey;
            static const uint8_t maxK[kPerfFx] = {5, 5, 100, 100, 4, 4, 100, 3};
            int step = maxK[heldKey] > 10 ? 2 : 1;
            for (int i = 0; i < kTracks; i++)
                engine.track[i].perfKnob[heldKey] =
                  (uint8_t)clampi(engine.track[sel].perfKnob[heldKey] + d * step, 0, maxK[heldKey]);
            refreshKeys();
            return;
        }
    }

    switch (page)
    {
        case kHome:
            if (perform)
            {
                // a held pad: that track's own knob
                for (int i = 0; i < kTracks; i++)
                    if (heldPad[i] >= 0)
                    {
                        static const uint8_t maxK[kPerfFx] = {5, 5, 100, 100, 4, 4, 100, 3};
                        int f = heldPad[i];
                        int step = maxK[f] > 10 ? 2 : 1;
                        engine.track[i].perfKnob[f] = (uint8_t)clampi(engine.track[i].perfKnob[f] + d * step, 0, maxK[f]);
                        return;
                    }
            }
            if (bar == kBarGrain)
            {
                GrainParams& g = t.grain;
                switch (grainKnob)
                {
                    case 0: g.spread = (uint8_t)clampi(g.spread + d, 0, 100); break;
                    case 1: g.size = (uint8_t)clampi(g.size + d, 0, 100); break;
                    case 2: g.density = (uint8_t)clampi(g.density + d, 0, 100); break;
                    case 3: g.pitch = (int8_t)clampi(g.pitch + d, -24, 24); break;
                    case 4: g.speed = (uint8_t)clampi(g.speed + d, 0, 2); break;
                    case 5: g.direction = (uint8_t)clampi(g.direction + d, 0, 2); break;
                }
                refreshKeys();
                return;
            }
            if (shift)
                engine.cfg.inputGain = (uint8_t)clampi(engine.cfg.inputGain + d, 0, 100);
            else
                t.volume = (uint8_t)clampi(t.volume + d, 0, 100);
            break;
        case kMixer: mixerChange(d, shift); break;
        case kFx:
        {
            FxSlot& f = engine.fx[fxSlot];
            if (fxKnob == 8)
                engine.fxReturn[fxSlot] = (uint8_t)clampi(engine.fxReturn[fxSlot] + d, 0, 100);
            else if (fxKnob < kFxDesc[f.type()].knobs)
                f.knob[fxKnob] = (uint8_t)clampi(f.knob[fxKnob] + d * (shift ? 1 : 2), 0, 100);
            break;
        }
        case kInput: inputChange(inRow, inCol, d); break;
        case kConfig: configChange(cfgRow, d); break;
        case kFile:
            if (projectCount) fileRow = (uint8_t)clampi(fileRow + d, 0, projectCount - 1);
            break;
        case kEditor:
        {
            Track& et = engine.track[edTrack];
            int32_t step = (int32_t)editorStep();
            if (shift) step = edSnap == 3 ? (int32_t)(kSampleRate / 1000) : step / 4;
            if (step < 1) step = 1;
            uint32_t& m = edMarker ? edB : edA;
            int64_t nv = (int64_t)m + (int64_t)d * step;
            if (!shift && edSnap < 3) nv = (nv / step) * step;
            if (nv < 0) nv = 0;
            if (nv > (int64_t)et.len) nv = et.len;
            m = (uint32_t)nv;
            if (edA > edB)
            {
                uint32_t x = edA;
                edA = edB;
                edB = x;
                edMarker ^= 1;
            }
            edConfirm = -1;
            break;
        }
        case kManual: manualPage = (uint8_t)clampi(manualPage + d, 0, 7); break;
    }
    refreshKeys();
}

//------------------------------------------------------------------------------
// pads: one row per track

void cLooper::pad(uint8_t p, uint8_t state)
{
    int row = p / 12, col = p % 12;
    if (row >= kTracks) return;
    if (state == 1 && row != sel) selectTrack(row);
    if (page != kHome) return;

    if (perform && col >= 4)
    {
        int f = col - 4;
        if (state == 1)
        {
            if (shift)
                engine.perfLatch(row, f);
            else
            {
                engine.perfHold(row, f, true);
                heldPad[row] = (int8_t)f;
            }
        }
        else
        {
            engine.perfHold(row, f, false);
            if (heldPad[row] == f) heldPad[row] = -1;
        }
        return;
    }
    if (state != 1) return;
    switch (col)
    {
        case 0:
            if (shift)
                engine.pressReplace(row);
            else
                engine.pressRec(row);
            break;
        case 1: engine.pressPlay(row); break;
        case 2: engine.pressMute(row); break;
        case 3:
            if (perform)
            {
                // freeze a Clouds slot: row 1 = FX 1, row 2 = FX 2
                if (row < kFxSlots && engine.fx[row].type() == kFxClouds)
                    engine.fx[row].knob[7] = engine.fx[row].knob[7] >= 50 ? 0 : 100;
            }
            else
                engine.clear(row);
            break;
        default: engine.jump(row, col - 4); break;
    }
    refreshKeys();
}

void cLooper::tapTempo()
{
    uint32_t ms = now();
    if (tapCount && ms - taps[(tapCount - 1) & 3] > 2000) tapCount = 0;
    taps[tapCount & 3] = ms;
    tapCount++;
    if (tapCount >= 3)
    {
        int n = tapCount < 4 ? tapCount : 4;
        uint32_t first = taps[(tapCount - n) & 3], last = taps[(tapCount - 1) & 3];
        float interval = (float)(last - first) / (n - 1);
        if (interval > 200)
        {
            float bpm = 60000.0f / interval;
            engine.setBpm(bpm);
            char s[24];
            snprintf(s, sizeof(s), "%.1f bpm", engine.currentBpm());
            flash(s);
        }
    }
}

//==============================================================================
// callbacks

static void functFKey(uint8_t button, uint8_t state)
{
    if (state == buttonPress)
        LP->keyPress(button - interfaceButton0);
    else if (state == buttonRelease)
        LP->keyRelease(button - interfaceButton0);
}

static void functShift(uint8_t state)
{
    if (state == buttonPress || state == buttonRelease)
    {
        LP->shift = state == buttonPress;
        LP->refreshKeys();
        if (LP->page == cLooper::kMixer || LP->page == cLooper::kFx) display.refreshControl(LP->bgCtl);
    }
}

static void functPlay(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->shift)
    {
        engine.cfg.metronome = !engine.cfg.metronome;
        LP->flash(engine.cfg.metronome ? "Metronome on" : "Metronome off");
    }
    else
        engine.transport();
    LP->refreshKeys();
}

static void functRec(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->shift)
        engine.pressReplace(LP->sel);
    else
        engine.pressRec(LP->sel);
    LP->refreshKeys();
}

static void functInsert(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->page == cLooper::kEditor)
    {
        LP->flash(engine.paste(LP->edTrack, LP->edA, true) ? "Inserted" : "Nothing to insert");
        return;
    }
    if (LP->shift)
        engine.redo(LP->sel);
    else
        engine.undo(LP->sel);
    LP->refreshKeys();
}

static void functDelete(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->shift)
        engine.clearAll();
    else
        engine.clear(LP->sel);
    LP->refreshKeys();
}

static void functCopy(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->page == cLooper::kEditor)
    {
        if (LP->shift)
            LP->flash(engine.paste(LP->edTrack, LP->edA, false) ? "Pasted" : "Nothing to paste");
        else
            LP->flash(engine.copySel(LP->edTrack, LP->edA, LP->edB) ? "Copied" : "Nothing selected");
        return;
    }
    int to = (LP->sel + 1) % kTracks;
    LP->flash(engine.duplicate(LP->sel, to) ? "Duplicated" : "The next track is not empty");
}

static void functUp(uint8_t state)
{
    if (state != buttonPress) return;
    switch (LP->page)
    {
        case cLooper::kHome: LP->selectTrack(LP->sel - 1); break;
        case cLooper::kMixer: LP->mixRow = (uint8_t)clampi(LP->mixRow - 1, 0, 3); break;
        case cLooper::kInput: LP->inRow = (uint8_t)clampi(LP->inRow - 1, 0, 9); break;
        case cLooper::kConfig: LP->cfgRow = (uint8_t)clampi(LP->cfgRow - 1, 0, 1); break;
        case cLooper::kFile: LP->fileRow = (uint8_t)clampi(LP->fileRow - 1, 0, 15); break;
        case cLooper::kEditor:
            LP->edTrack = (uint8_t)clampi(LP->edTrack - 1, 0, kTracks - 1);
            LP->showPage(cLooper::kEditor);
            break;
        case cLooper::kManual: LP->manualPage = (uint8_t)clampi(LP->manualPage - 1, 0, 7); break;
        default: break;
    }
    LP->refreshKeys();
}

static void functDown(uint8_t state)
{
    if (state != buttonPress) return;
    switch (LP->page)
    {
        case cLooper::kHome: LP->selectTrack(LP->sel + 1); break;
        case cLooper::kMixer: LP->mixRow = (uint8_t)clampi(LP->mixRow + 1, 0, 3); break;
        case cLooper::kInput: LP->inRow = (uint8_t)clampi(LP->inRow + 1, 0, LP->inCol ? 9 : 3); break;
        case cLooper::kConfig: LP->cfgRow = (uint8_t)clampi(LP->cfgRow + 1, 0, 1); break;
        case cLooper::kFile:
            if (LP->projectCount) LP->fileRow = (uint8_t)clampi(LP->fileRow + 1, 0, LP->projectCount - 1);
            break;
        case cLooper::kEditor:
            LP->edTrack = (uint8_t)clampi(LP->edTrack + 1, 0, kTracks - 1);
            LP->showPage(cLooper::kEditor);
            break;
        case cLooper::kManual: LP->manualPage = (uint8_t)clampi(LP->manualPage + 1, 0, 7); break;
        default: break;
    }
    LP->refreshKeys();
}

static void functLeft(uint8_t state)
{
    if (state != buttonPress) return;
    switch (LP->page)
    {
        case cLooper::kMixer: LP->mixStrip = (uint8_t)clampi(LP->mixStrip - 1, 0, 6); break;
        case cLooper::kFx: LP->fxKnob = (uint8_t)clampi(LP->fxKnob - 1, 0, 8); break;
        case cLooper::kInput:
            LP->inCol = 0;
            if (LP->inRow > 3) LP->inRow = 3;
            break;
        case cLooper::kEditor: LP->edMarker = 0; break;
        case cLooper::kManual: LP->manualPage = (uint8_t)clampi(LP->manualPage - 1, 0, 7); break;
        default: break;
    }
    LP->refreshKeys();
}

static void functRight(uint8_t state)
{
    if (state != buttonPress) return;
    switch (LP->page)
    {
        case cLooper::kMixer: LP->mixStrip = (uint8_t)clampi(LP->mixStrip + 1, 0, 6); break;
        case cLooper::kFx: LP->fxKnob = (uint8_t)clampi(LP->fxKnob + 1, 0, 8); break;
        case cLooper::kInput: LP->inCol = 1; break;
        case cLooper::kEditor: LP->edMarker = 1; break;
        case cLooper::kManual: LP->manualPage = (uint8_t)clampi(LP->manualPage + 1, 0, 7); break;
        default: break;
    }
    LP->refreshKeys();
}

static void functEncoder(int16_t value) { LP->encoder(value); }

static void functPads(uint8_t pad, uint8_t state, int16_t velo)
{
    (void)velo;
    LP->pad(pad, state);
}

static void functMiddle(uint8_t state)
{
    if (state != buttonPress) return;
    if (LP->page == cLooper::kHome)
        LP->tapTempo();
    else if (LP->page == cLooper::kInput && (LP->inRow == 2 || LP->inRow == 5 || LP->inRow == 6))
        LP->inputChange(LP->inRow, LP->inCol, 1);
    LP->refreshKeys();
}

static void functMenu(uint8_t state)
{
    if (state == buttonPress)
        mtMenuPopup.showModulePopup(LP);
    else if (state == buttonRelease)
        mtMenuPopup.hideModulePopup(LP);
}

// page keys (whatever the panel has) and the Mini's Fn keys
static void functPageKey(uint8_t button, uint8_t state)
{
    bool press = state == buttonPress;
    bool release = state == buttonRelease;

    // Perform (or Instr): tap latches, hold works while held
    if (button == interfaceButtonPerformance || button == interfaceButtonInstr || button == interfaceButtonParams ||
        (button == interfaceButtonMiniFn3 && !LP->shift))
    {
        if (press)
        {
            LP->performHeld = true;
            LP->heldSince = LP->now();
            if (!LP->perform)
            {
                LP->perform = true;
                LP->bar = cLooper::kBarMain;
                LP->showPage(cLooper::kHome);
            }
            else
            {
                LP->perform = false;
                engine.perfClear();
                LP->performHeld = false;
            }
            LP->refreshAll();
        }
        else if (release && LP->performHeld)
        {
            LP->performHeld = false;
            if (LP->now() - LP->heldSince > 600)
            {
                // held: momentary
                LP->perform = false;
                engine.perfClear();
                LP->refreshAll();
            }
        }
        return;
    }
    if (!press) return;

    if (button == interfaceButtonMaster || (button == interfaceButtonMiniFn1 && !LP->shift))
    {
        LP->mixStrip = LP->sel;
        LP->showPage(cLooper::kMixer);
    }
    else if (button == interfaceButtonFx1 || button == interfaceButtonFx2 || (button == interfaceButtonMiniFn2 && !LP->shift))
    {
        if (button == interfaceButtonFx1)
            LP->fxSlot = 0;
        else if (button == interfaceButtonFx2)
            LP->fxSlot = 1;
        else if (LP->page == cLooper::kFx)
            LP->fxSlot ^= 1;
        LP->fxKnob = 1;
        LP->showPage(cLooper::kFx);
    }
    else if (button == interfaceButtonNote || button == interfaceButtonSampleEdit ||
             (button == interfaceButtonMiniFn4 && !LP->shift))
        LP->showPage(cLooper::kEditor);
    else if (button == interfaceButtonPattern)
    {
        if (LP->perform)
        {
            LP->perform = false;
            engine.perfClear();
        }
        LP->bar = cLooper::kBarMain;
        LP->showPage(cLooper::kHome);
    }
    else if (button == interfaceButtonConfig || (button == interfaceButtonMiniFn2 && LP->shift))
        LP->showPage(cLooper::kConfig);
    else if (button == interfaceButtonFile || button == interfaceButtonSampleLoad || (button == interfaceButtonMiniFn3 && LP->shift))
        LP->showPage(cLooper::kFile);
    else if (button == interfaceButtonSong || (button == interfaceButtonMiniFn4 && LP->shift))
        LP->showPage(cLooper::kManual);
    else if (button == interfaceButtonMiniFn1 && LP->shift)
        LP->showPage(cLooper::kInput);
    else if (button == interfaceButtonSamplePlay || button == interfaceButtonSampleRec)
        LP->showPage(cLooper::kInput);
}

#endif
