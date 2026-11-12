// Retrolooper One — stereo looper insert for Akai MPC standalone.
//
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 IAMTHEBAND LLC, Retroloop USA.
//
// Signal path, per sample:
//   dry input passes through unchanged, so the MPC track stays audible
//   capture input (gain and source) is written into the loop while recording
//     or added onto the loop while overdubbing
//   loop read -> edge fade -> high pass -> low pass -> loop level -> pan -> master
//   output = dry + loop
//
// The control helper (ControlHelper.h) decides when a take starts, when the
// host clock moves it, and when a switch fires. Parameter 0 is Master and only
// scales the loop, so the host's load-time nudge cannot mute the track.
//
// Memory is reserved in the constructor: two stereo takes (current and undo)
// plus one export copy, each up to 32 seconds at 48 kHz. process() does not
// allocate. The export WAV is written on a worker thread.

#include "mpc_plugin.h"
#include "dsp/biquad.h"
#include "dsp/math.h"
#include "dsp/smoother.h"

#include "ControlHelper.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <direct.h>
#else
  #include <sys/stat.h>
#endif

using namespace mpc::dsp;

namespace {

constexpr int kMaxFrames = 48000 * 32;

enum ParamIndex
{
    kMaster = 0,   // percent, loop output only — kept harmless on purpose
    kLoop,         // percent
    kInput,        // percent, 0..400
    kPan,          // -100..100
    kPlay,
    kRecord,
    kDub,
    kMute,
    kLength,       // /2, x1, x2
    kBars,
    kLaunch,       // Sequence, Next bar
    kFollow,
    kAfter,        // Play, Overdub
    kSource,       // Stereo, Left, Right, Off
    kOutput,
    kFeedback,
    kFade,
    kHighPass,
    kLowPass,
    kAuto,
    kThreshold,
    kUndo,
    kRedo,
    kClear,
    kReverse,
    kStop,
    kSync,
    kExport,
    kNumParams
};

struct LoopBuf
{
    std::vector<float> l, r;
};

enum class Mode { Stopped, ArmedRecord, Recording, Playing, ArmedDub, Overdubbing };

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((v >> (8 * i)) & 0xff));
}

bool getU32(const uint8_t* data, size_t size, size_t& pos, uint32_t& v)
{
    if (pos + 4 > size) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= (uint32_t)data[pos + (size_t)i] << (8 * i);
    pos += 4;
    return true;
}

} // namespace

class RetrolooperOne : public mpc::Plugin
{
public:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo i;
        i.name = "Retrolooper One";
        i.vendor = "IAMTHEBAND LLC";
        i.uniqueId = MPC_FOURCC('I', 'H', '9', '7');
        i.version = 1000;
        i.isInstrument = false;
        i.numInputs = 2;
        i.numOutputs = 2;
        i.tailSamples = 1;
        return i;
    }

    RetrolooperOne() : mpc::Plugin(makeInfo())
    {
        using namespace mpc;
        addParam(floatParam ("master", "Master",     0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam ("loop",   "Loop",       0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam ("input",  "Input",      0.0f, 400.0f, 100.0f, "%", 0));
        addParam(floatParam ("pan",    "Pan",     -100.0f, 100.0f,   0.0f, "", 0));
        addParam(boolParam  ("play",   "Play", false));
        addParam(boolParam  ("record", "Record", false));
        addParam(boolParam  ("dub",    "Overdub", false));
        addParam(boolParam  ("mute",   "Mute", false));
        addParam(choiceParam("length", "Length", {"/2", "x1", "x2"}, 1));
        addParam(intParam   ("bars",   "Bars", 1, 32, 4, "bars"));
        addParam(choiceParam("launch", "Launch", {"Sequence", "Next bar"}, 0));
        addParam(boolParam  ("follow", "Follow", true));
        addParam(choiceParam("after",  "After", {"Play", "Overdub"}, 0));
        addParam(choiceParam("source", "Source", {"Stereo", "Left", "Right", "Off"}, 0));
        addParam(boolParam  ("output", "Output", true));
        addParam(floatParam ("feedback","Feedback", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(floatParam ("fade",   "Fade", 0.0f, 20.0f, 5.0f, "ms", 1));
        addParam(logParam   ("hp",     "High Pass", 20.0f, 2000.0f, 20.0f, "Hz", 0));
        addParam(logParam   ("lp",     "Low Pass", 500.0f, 20000.0f, 20000.0f, "Hz", 0));
        addParam(boolParam  ("auto",   "Auto Start", false));
        addParam(floatParam ("thresh", "Threshold", -72.0f, 0.0f, -36.0f, "dB", 0));
        addParam(boolParam  ("undo",   "Undo", false));
        addParam(boolParam  ("redo",   "Redo", false));
        addParam(boolParam  ("clear",  "Clear", false));
        addParam(boolParam  ("reverse","Reverse", false));
        addParam(boolParam  ("stop",   "Stop", false));
        addParam(boolParam  ("sync",   "Sync", false));
        addParam(boolParam  ("export", "Export", false));

        for (auto& b : loop_)
        {
            b.l.assign((size_t)kMaxFrames, 0.0f);
            b.r.assign((size_t)kMaxFrames, 0.0f);
        }
        exportL_.assign((size_t)kMaxFrames, 0.0f);
        exportR_.assign((size_t)kMaxFrames, 0.0f);

        addPrograms();
    }

    ~RetrolooperOne() override
    {
        stop_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sampleRate_ = sampleRate > 1.0 ? sampleRate : 44100.0;
        master_.setTime(20.0f, sampleRate_);
        loopGain_.setTime(20.0f, sampleRate_);
        inputGain_.setTime(20.0f, sampleRate_);
        feedback_.setTime(20.0f, sampleRate_);
        master_.snap(get(kMaster) / 100.0f);
        loopGain_.snap(get(kLoop) / 100.0f);
        inputGain_.snap(get(kInput) / 100.0f);
        feedback_.snap(get(kFeedback) / 100.0f);
        applyStaging();
    }

    void reset() override
    {
        mode_ = Mode::Stopped;
        heldStop_ = false;
        playhead_ = 0;
        recorded_ = 0;
        high_.reset();
        low_.reset();
        master_.snap(get(kMaster) / 100.0f);
        loopGain_.snap(get(kLoop) / 100.0f);
        inputGain_.snap(get(kInput) / 100.0f);
        feedback_.snap(get(kFeedback) / 100.0f);
        applyStaging();
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        if (index == kHighPass && plain <= 22.0f) { out = "Off"; return true; }
        if (index == kLowPass && plain >= 19000.0f) { out = "Off"; return true; }
        if (index == kPan)
        {
            int p = (int)std::lround(plain);
            if (p == 0) out = "C";
            else if (p < 0) out = "L" + std::to_string(-p);
            else out = "R" + std::to_string(p);
            return true;
        }
        return false;
    }

    void saveExtraState(std::vector<uint8_t>& out) override
    {
        const int frames = frames_[active_];
        putU32(out, 0x314F4C52u); // 'RLO1' little-endian
        putU32(out, 1u);
        putU32(out, (uint32_t)frames);
        putU32(out, reverse_[active_] ? 1u : 0u);
        if (frames <= 0) return;
        const size_t n = (size_t)frames;
        const size_t bytes = n * sizeof(float);
        const size_t at = out.size();
        out.resize(at + bytes * 2);
        std::memcpy(out.data() + at, loop_[active_].l.data(), bytes);
        std::memcpy(out.data() + at + bytes, loop_[active_].r.data(), bytes);
    }

    void loadExtraState(const uint8_t* data, size_t size) override
    {
        staged_.l.clear();
        staged_.r.clear();
        stagedFrames_ = 0;
        stagedReverse_ = false;
        hasStaging_ = false;
        if (data == nullptr || size < 16) return;
        size_t pos = 0;
        uint32_t magic = 0, version = 0, frames = 0, reverse = 0;
        if (!getU32(data, size, pos, magic) || magic != 0x314F4C52u) return;
        if (!getU32(data, size, pos, version) || version != 1u) return;
        if (!getU32(data, size, pos, frames) || !getU32(data, size, pos, reverse)) return;
        if (frames > (uint32_t)kMaxFrames) return;
        const size_t bytes = (size_t)frames * sizeof(float);
        if (frames > 0 && pos + bytes * 2 != size) return;
        if (frames == 0 && pos != size) return;
        staged_.l.assign((size_t)frames, 0.0f);
        staged_.r.assign((size_t)frames, 0.0f);
        if (frames > 0)
        {
            std::memcpy(staged_.l.data(), data + pos, bytes);
            std::memcpy(staged_.r.data(), data + pos + bytes, bytes);
        }
        stagedFrames_ = (int)frames;
        stagedReverse_ = reverse != 0;
        hasStaging_ = true;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const retro::ControlBlock ctl = controls_.update(ctx.transport, readControls(), ctx.sampleRate, kMaxFrames);
        handleActions(ctl);

        const bool transportOk = !ctl.follow || !ctx.transport.valid || ctl.hostPlaying;
        const bool barOk = !ctl.nextBar || ctl.atBar;
        const bool hear = (mode_ == Mode::Playing || mode_ == Mode::Overdubbing) && hasAudio() && !heldStop_;
        const bool dubbing = mode_ == Mode::Overdubbing;
        if ((hear || dubbing) && ctl.hostFrame >= 0 && hasAudio())
        {
            const int window = windowFrames(ctl);
            const int expected = wrap(ctl.hostFrame, window);
            int diff = playhead_ - expected;
            if (diff < 0) diff = -diff;
            if (diff > window / 2) diff = window - diff;
            if (diff > ctx.numFrames * 2) playhead_ = expected;
        }

        master_.setTarget(ctl.master);
        loopGain_.setTarget(ctl.loop);
        inputGain_.setTarget(ctl.input);
        feedback_.setTarget(ctl.feedback);

        const bool hpOn = ctl.highPassHz > 22.0f;
        const bool lpOn = ctl.lowPassHz < 19000.0f;
        if (hpOn) high_.setCoeffs(BiquadCoeffs::highpass(ctx.sampleRate, ctl.highPassHz, 0.7071f));
        if (lpOn) low_.setCoeffs(BiquadCoeffs::lowpass(ctx.sampleRate, ctl.lowPassHz, 0.7071f));

        float panL = 1.0f, panR = 1.0f;
        equalPowerPan(ctl.pan, panL, panR);
        const int fade = (int)(ctl.fadeMs * 0.001f * (float)ctx.sampleRate);

        if (ctl.riseSync && hasAudio())
            playhead_ = ctl.hostFrame >= 0 ? ctl.hostFrame % windowFrames(ctl) : 0;

        const float* inL = ctx.inputs[0];
        const float* inR = ctx.numInputs > 1 && ctx.inputs[1] != nullptr ? ctx.inputs[1] : inL;
        float* outL = ctx.outputs[0];
        float* outR = ctx.numOutputs > 1 ? ctx.outputs[1] : ctx.outputs[0];
        if (inL == nullptr || outL == nullptr || outR == nullptr) return;

        bool closeTake = false;
        const int target = ctl.targetFrames > 0 ? ctl.targetFrames : 64;

        for (int i = 0; i < ctx.numFrames; ++i)
        {
            const float dryL = sanitize(inL[i]);
            const float dryR = sanitize(inR[i]);
            const float gMaster = master_.process();
            const float gLoop = loopGain_.process();
            const float gIn = inputGain_.process();
            const float fb = feedback_.process();
            float capL = 0.0f, capR = 0.0f;
            capture(dryL, dryR, gIn, ctl.source, capL, capR);

            if (mode_ == Mode::ArmedRecord && transportOk && barOk)
            {
                const bool open = !ctl.autoStart || std::fabs(capL) >= ctl.threshold || std::fabs(capR) >= ctl.threshold;
                if (open) beginRecord(target, startFrame(ctl, i, target));
            }
            else if (mode_ == Mode::ArmedDub && transportOk && barOk && hasAudio())
                beginDub();

            float wetL = 0.0f, wetR = 0.0f;
            if (mode_ == Mode::Recording && !closeTake)
            {
                const int idx = wrap(playhead_, target_);
                loop_[recDest_].l[(size_t)idx] = capL;
                loop_[recDest_].r[(size_t)idx] = capR;
                playhead_ = idx + 1;
                if (playhead_ >= target_) playhead_ = 0;
                if (++recorded_ >= target_) closeTake = true;
            }
            else if ((hear || dubbing) && hasAudio())
            {
                const int window = windowFrames(ctl);
                const int idx = wrap(reverse_[active_] ? window - 1 - playhead_ : playhead_, window);
                float l = loop_[active_].l[(size_t)idx];
                float r = loop_[active_].r[(size_t)idx];
                if (dubbing && !heldStop_)
                {
                    l = softClip(l * fb + capL);
                    r = softClip(r * fb + capR);
                    loop_[active_].l[(size_t)idx] = l;
                    loop_[active_].r[(size_t)idx] = r;
                }
                edgeFade(l, r, idx, window, fade, active_);
                if (hpOn) high_.process(l, r);
                if (lpOn) low_.process(l, r);
                if (hear && ctl.output && !ctl.mute)
                {
                    wetL = l * gLoop * gMaster * panL;
                    wetR = r * gLoop * gMaster * panR;
                }
                playhead_ = wrap(playhead_ + 1, window);
            }

            outL[i] = sanitize(dryL + wetL);
            outR[i] = sanitize(dryR + wetR);
        }

        if (closeTake) finishTake(ctl.after == 1 && (getBool(kDub) || ctl.hostRecording));
    }

private:
    void addPrograms()
    {
        auto values = [] {
            return std::vector<float>{
                100, 100, 100, 0,  // master, loop, input, pan
                0, 0, 0, 0,        // play, record, overdub, mute
                1, 4, 0, 1,        // length x1, bars, sequence, follow on
                0, 0, 1, 100,      // after play, stereo, output on, feedback
                5, 20, 20000,      // fade, high pass, low pass
                0, -36,            // auto start off, threshold
                0, 0, 0, 0, 0, 0, 0
            };
        };
        auto put = [&](const char* name, std::vector<float> v) { addProgram(name, std::move(v)); };
        put("Init", values());
        auto overdub = values(); overdub[kAfter] = 1; put("Overdub", overdub);
        auto half = values(); half[kLength] = 0; put("Half Length", half);
        auto dbl = values(); dbl[kLength] = 2; put("Double", dbl);
        auto filtered = values(); filtered[kHighPass] = 180; filtered[kLowPass] = 4000; put("Filtered", filtered);
        auto punch = values(); punch[kAuto] = 1; punch[kThreshold] = -24; put("Punch", punch);
        auto soft = values(); soft[kFeedback] = 70; soft[kFade] = 12; soft[kAfter] = 1; put("Soft Dub", soft);
        auto manual = values(); manual[kFollow] = 0; put("Manual", manual);
    }

    retro::ControlInput readControls() const
    {
        retro::ControlInput in;
        in.master = get(kMaster); in.loop = get(kLoop); in.input = get(kInput); in.pan = get(kPan);
        in.play = getBool(kPlay); in.record = getBool(kRecord); in.dub = getBool(kDub); in.mute = getBool(kMute);
        in.length = getInt(kLength); in.bars = getInt(kBars); in.launch = getInt(kLaunch);
        in.follow = getBool(kFollow); in.after = getInt(kAfter); in.source = getInt(kSource);
        in.output = getBool(kOutput); in.feedback = get(kFeedback); in.fadeMs = get(kFade);
        in.highPassHz = get(kHighPass); in.lowPassHz = get(kLowPass);
        in.autoStart = getBool(kAuto); in.thresholdDb = get(kThreshold);
        in.undo = getBool(kUndo); in.redo = getBool(kRedo); in.clear = getBool(kClear);
        in.reverse = getBool(kReverse); in.stop = getBool(kStop); in.sync = getBool(kSync);
        in.exportWav = getBool(kExport);
        return in;
    }

    void handleActions(const retro::ControlBlock& ctl)
    {
        if (ctl.risePlay || ctl.hostPlayRise) heldStop_ = false;
        if (ctl.riseStop)
        {
            if (mode_ == Mode::Recording) finishTake(false);
            heldStop_ = true;
            mode_ = Mode::Stopped;
        }
        if (ctl.riseClear) clearLoop();
        if (ctl.riseUndo && mode_ != Mode::Recording && mode_ != Mode::Overdubbing) undo();
        if (ctl.riseRedo && mode_ != Mode::Recording && mode_ != Mode::Overdubbing) redo();
        if (ctl.riseReverse && hasAudio() && mode_ != Mode::Recording) reverse_[active_] = !reverse_[active_];
        if (ctl.riseExport) requestExport();

        if (ctl.follow && ctl.hostPlayFall)
        {
            if (mode_ == Mode::Recording) finishTake(false);
            if (mode_ == Mode::Overdubbing || mode_ == Mode::Playing || mode_ == Mode::ArmedRecord || mode_ == Mode::ArmedDub)
                mode_ = Mode::Stopped;
        }
        if (ctl.fallPlay && !ctl.follow) mode_ = Mode::Stopped;
        if ((ctl.fallRecord || ctl.hostRecFall) && mode_ == Mode::Recording) finishTake(false);
        if ((ctl.fallDub || ctl.hostRecFall) && mode_ == Mode::Overdubbing)
            mode_ = (ctl.follow && ctl.hostPlaying) || ctl.playHeld ? Mode::Playing : Mode::Stopped;

        const bool idle = mode_ == Mode::Stopped || mode_ == Mode::Playing;
        if (idle && (ctl.riseRecord || (ctl.follow && ctl.hostRecRise)))
        {
            if (hasAudio()) mode_ = Mode::ArmedDub;
            else mode_ = Mode::ArmedRecord;
        }
        else if (idle && ctl.riseDub && hasAudio())
            mode_ = Mode::ArmedDub;

        if (ctl.risePlay || (ctl.follow && ctl.hostPlayRise))
        {
            if (hasAudio() && (mode_ == Mode::Stopped || mode_ == Mode::ArmedRecord))
                mode_ = Mode::Playing;
        }
        if (ctl.playHeld && hasAudio() && mode_ == Mode::Stopped && !heldStop_)
            mode_ = Mode::Playing;
        if (!ctl.playHeld && !ctl.follow && mode_ == Mode::Playing)
            mode_ = Mode::Stopped;
        if (ctl.follow && !ctl.hostPlaying && !ctl.playHeld && mode_ == Mode::Playing)
            mode_ = Mode::Stopped;
    }

    void beginRecord(int target, int startPos)
    {
        if (target < 64) target = 64;
        if (target > kMaxFrames) target = kMaxFrames;
        recDest_ = 1 - active_;
        std::memset(loop_[recDest_].l.data(), 0, sizeof(float) * (size_t)target);
        std::memset(loop_[recDest_].r.data(), 0, sizeof(float) * (size_t)target);
        target_ = target;
        recorded_ = 0;
        playhead_ = wrap(startPos, target);
        mode_ = Mode::Recording;
    }

    void beginDub()
    {
        if (!hasAudio() || mode_ == Mode::Overdubbing) return;
        const int n = frames_[active_];
        const int spare = 1 - active_;
        std::memcpy(loop_[spare].l.data(), loop_[active_].l.data(), sizeof(float) * (size_t)n);
        std::memcpy(loop_[spare].r.data(), loop_[active_].r.data(), sizeof(float) * (size_t)n);
        frames_[spare] = n;
        reverse_[spare] = reverse_[active_];
        undoValid_ = true;
        redoValid_ = false;
        mode_ = Mode::Overdubbing;
    }

    void finishTake(bool thenDub)
    {
        if (mode_ != Mode::Recording) { if (thenDub) beginDub(); return; }
        const int previous = active_;
        frames_[recDest_] = target_ > 0 ? target_ : recorded_;
        reverse_[recDest_] = false;
        active_ = recDest_;
        undoValid_ = previous != active_ && frames_[previous] > 0;
        redoValid_ = false;
        mode_ = Mode::Playing;
        if (thenDub) beginDub();
    }

    void clearLoop()
    {
        if (mode_ == Mode::Recording) mode_ = Mode::Stopped;
        if (frames_[active_] > 0)
        {
            active_ = 1 - active_;
            frames_[active_] = 0;
            reverse_[active_] = false;
            undoValid_ = true;
            redoValid_ = false;
        }
        mode_ = Mode::Stopped;
        playhead_ = 0;
    }

    void undo()
    {
        if (!undoValid_) return;
        active_ = 1 - active_;
        playhead_ = 0;
        undoValid_ = false;
        redoValid_ = true;
    }

    void redo()
    {
        if (!redoValid_) return;
        active_ = 1 - active_;
        playhead_ = 0;
        undoValid_ = true;
        redoValid_ = false;
    }

    bool hasAudio() const { return frames_[active_] > 0; }

    int windowFrames(const retro::ControlBlock& ctl) const
    {
        int stored = frames_[active_];
        if (stored < 1) stored = 1;
        int window = stored;
        if (ctl.targetFrames > 0 && ctl.targetFrames < stored) window = ctl.targetFrames;
        return window;
    }

    static int wrap(int frame, int length)
    {
        if (length <= 1) return 0;
        if (frame >= 0 && frame < length) return frame;
        int m = frame % length;
        if (m < 0) m += length;
        return m;
    }

    static int startFrame(const retro::ControlBlock& ctl, int sample, int target)
    {
        if (ctl.hostFrame < 0) return 0;
        return wrap(ctl.hostFrame + sample, target);
    }

    static void capture(float inL, float inR, float gain, int source, float& capL, float& capR)
    {
        if (source == 3) { capL = capR = 0.0f; return; }
        if (source == 1) inR = inL;
        else if (source == 2) inL = inR;
        capL = sanitize(inL * gain);
        capR = sanitize(inR * gain);
    }

    void edgeFade(float& l, float& r, int index, int frames, int fade, int buffer) const
    {
        if (fade <= 1 || frames <= fade) return;
        if (index >= fade) return;
        const float t = (float)index / (float)fade;
        int tail = frames - fade + index;
        if (tail < 0) tail = 0;
        if (tail >= frames) tail = frames - 1;
        l = loop_[buffer].l[(size_t)tail] * (1.0f - t) + l * t;
        r = loop_[buffer].r[(size_t)tail] * (1.0f - t) + r * t;
    }

    void applyStaging()
    {
        if (!hasStaging_) return;
        if (loop_[0].l.empty()) return;
        const int n = stagedFrames_ < kMaxFrames ? stagedFrames_ : kMaxFrames;
        if (n > 0)
        {
            std::memcpy(loop_[0].l.data(), staged_.l.data(), sizeof(float) * (size_t)n);
            std::memcpy(loop_[0].r.data(), staged_.r.data(), sizeof(float) * (size_t)n);
        }
        frames_[0] = n;
        reverse_[0] = stagedReverse_;
        frames_[1] = 0;
        reverse_[1] = false;
        active_ = 0;
        undoValid_ = false;
        redoValid_ = false;
        playhead_ = 0;
        mode_ = Mode::Stopped;
        hasStaging_ = false;
    }

    void requestExport()
    {
        const int n = frames_[active_];
        if (n <= 0) return;
        ensureWorker();
        if (exportDone_.load(std::memory_order_acquire) != exportSerial_.load(std::memory_order_relaxed))
            return;
        std::memcpy(exportL_.data(), loop_[active_].l.data(), sizeof(float) * (size_t)n);
        std::memcpy(exportR_.data(), loop_[active_].r.data(), sizeof(float) * (size_t)n);
        exportFrames_ = n;
        exportRate_ = sampleRate_;
        exportSerial_.fetch_add(1, std::memory_order_release);
    }

    void ensureWorker()
    {
        if (worker_.joinable()) return;
        stop_.store(false, std::memory_order_release);
        worker_ = std::thread([this] { exportMain(); });
    }

    void exportMain()
    {
        int seen = 0;
        while (!stop_.load(std::memory_order_acquire))
        {
            const int serial = exportSerial_.load(std::memory_order_acquire);
            if (serial != seen)
            {
                writeWav(exportFrames_, exportRate_);
                seen = serial;
                exportDone_.store(serial, std::memory_order_release);
            }
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void writeWav(int frames, double sampleRate)
    {
        if (frames <= 0) return;
        const std::string folder = mpc::pluginFolder();
        if (folder.empty()) return;
        const std::string dir = folder + "/Exports";
#if defined(_WIN32)
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0755);
#endif
        std::string path;
        for (int slot = 1; slot <= 64; ++slot)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "/Loop-%02d.wav", slot);
            const std::string candidate = dir + name;
            std::FILE* existing = std::fopen(candidate.c_str(), "rb");
            if (existing != nullptr) { std::fclose(existing); continue; }
            path = candidate;
            break;
        }
        if (path.empty()) return;

        float peak = 0.0f;
        for (int i = 0; i < frames; ++i)
        {
            peak = std::fmax(peak, std::fabs(exportL_[(size_t)i]));
            peak = std::fmax(peak, std::fabs(exportR_[(size_t)i]));
        }
        const float scale = peak > 1.0f ? 1.0f / peak : 1.0f;
        const int rate = sampleRate > 1.0 ? (int)std::lround(sampleRate) : 44100;

        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr) return;
        auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) std::fputc((int)((v >> (8 * i)) & 0xff), f); };
        auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) std::fputc((int)((v >> (8 * i)) & 0xff), f); };
        const uint32_t dataBytes = (uint32_t)frames * 4u;
        std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
        std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1u); u16(2u); u32((uint32_t)rate);
        u32((uint32_t)rate * 4u); u16(4u); u16(16u);
        std::fwrite("data", 1, 4, f); u32(dataBytes);
        for (int i = 0; i < frames; ++i)
        {
            for (float s : { exportL_[(size_t)i], exportR_[(size_t)i] })
            {
                float x = sanitize(s) * scale;
                int v = (int)std::lround(x * 32767.0f);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                u16((uint16_t)(int16_t)v);
            }
        }
        std::fclose(f);
    }

    retro::ControlHelper controls_;
    LoopBuf loop_[2];
    int frames_[2] = {0, 0};
    bool reverse_[2] = {false, false};
    int active_ = 0;
    int recDest_ = 1;
    bool undoValid_ = false;
    bool redoValid_ = false;
    Mode mode_ = Mode::Stopped;
    bool heldStop_ = false;
    int playhead_ = 0;
    int target_ = 0;
    int recorded_ = 0;
    double sampleRate_ = 44100.0;

    Smoother master_, loopGain_, inputGain_, feedback_;
    StereoBiquad high_, low_;

    LoopBuf staged_;
    int stagedFrames_ = 0;
    bool stagedReverse_ = false;
    bool hasStaging_ = false;

    std::vector<float> exportL_, exportR_;
    int exportFrames_ = 0;
    double exportRate_ = 44100.0;
    std::atomic<int> exportSerial_{0};
    std::atomic<int> exportDone_{0};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

MPC_PLUGIN_ENTRY(RetrolooperOne)
