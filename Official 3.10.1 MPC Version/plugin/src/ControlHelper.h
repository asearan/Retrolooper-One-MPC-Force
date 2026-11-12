// ControlHelper.h — transport and button decisions for Retrolooper One.
//
// SPDX-License-Identifier: GPL-3.0-only
//
// This is the control helper that ships inside the official plugin. It watches
// the MPC transport (play, stop, record, tempo, time signature, cycle, bar)
// and the touch / Q-Link switches, then tells the audio code what to do.
//
// It replaces the parts of the older device helper that the official host
// already exposes: host record starts a take, host play and stop move the
// loop, and the on-screen switches do the same jobs when you want them by hand.
// It does not touch MPC system files, pad LEDs, track arm, or the timeline.

#pragma once

#include "mpc_plugin.h"
#include "dsp/math.h"

#include <cmath>

namespace retro {

struct ControlInput
{
    bool play = false;
    bool record = false;
    bool dub = false;
    bool mute = false;
    bool undo = false;
    bool redo = false;
    bool clear = false;
    bool reverse = false;
    bool stop = false;
    bool sync = false;
    bool exportWav = false;
    int length = 1;          // 0 = /2, 1 = x1, 2 = x2
    int bars = 4;            // used when the host has no cycle range
    int launch = 0;          // 0 = sequence phase, 1 = next bar
    bool follow = true;
    int after = 0;           // 0 = play, 1 = overdub
    int source = 0;          // 0 stereo, 1 left, 2 right, 3 off
    bool output = true;
    bool autoStart = false;
    float thresholdDb = -36.0f;
    float master = 100.0f;
    float loop = 100.0f;
    float input = 100.0f;
    float pan = 0.0f;        // -100..100
    float feedback = 100.0f; // percent of the previous layer kept while overdubbing
    float fadeMs = 5.0f;
    float highPassHz = 20.0f;
    float lowPassHz = 20000.0f;
};

struct ControlBlock
{
    int targetFrames = 0;
    int hostFrame = -1;     // playhead implied by the host clock, or -1
    bool hostPlaying = false;
    bool hostRecording = false;
    bool follow = true;
    bool nextBar = false;
    bool atBar = true;
    bool mute = false;
    bool playHeld = false;
    bool output = true;
    bool autoStart = false;
    int source = 0;
    int after = 0;
    float master = 1.0f;
    float loop = 1.0f;
    float input = 1.0f;
    float pan = 0.0f;        // -1..1
    float feedback = 1.0f;
    float fadeMs = 5.0f;
    float highPassHz = 20.0f;
    float lowPassHz = 20000.0f;
    float threshold = 0.0158f;

    bool risePlay = false, fallPlay = false;
    bool riseRecord = false, fallRecord = false;
    bool riseDub = false, fallDub = false;
    bool riseUndo = false, riseRedo = false, riseClear = false;
    bool riseReverse = false, riseStop = false, riseSync = false, riseExport = false;
    bool hostPlayRise = false, hostPlayFall = false;
    bool hostRecRise = false, hostRecFall = false;
};

class ControlHelper
{
public:
    ControlBlock update(const mpc::Transport& transport, const ControlInput& in, double sampleRate, int maxFrames)
    {
        ControlBlock b;
        b.follow = in.follow;
        b.nextBar = in.launch == 1;
        b.mute = in.mute;
        b.playHeld = in.play;
        b.output = in.output;
        b.autoStart = in.autoStart;
        b.source = in.source < 0 ? 0 : (in.source > 3 ? 3 : in.source);
        b.after = in.after == 1 ? 1 : 0;
        b.master = clamp01(in.master / 100.0f);
        b.loop = clamp01(in.loop / 100.0f);
        b.input = mpc::clamp(in.input / 100.0f, 0.0f, 4.0f);
        b.pan = mpc::clamp(in.pan / 100.0f, -1.0f, 1.0f);
        b.feedback = clamp01(in.feedback / 100.0f);
        b.fadeMs = mpc::clamp(in.fadeMs, 0.0f, 20.0f);
        b.highPassHz = mpc::clamp(in.highPassHz, 20.0f, 2000.0f);
        b.lowPassHz = mpc::clamp(in.lowPassHz, 500.0f, 20000.0f);
        b.threshold = mpc::dsp::dbToGain(mpc::clamp(in.thresholdDb, -72.0f, 0.0f));

        const bool hostValid = transport.valid;
        b.hostPlaying = hostValid && transport.playing;
        b.hostRecording = hostValid && transport.recording;
        b.atBar = barIsOpen(transport);
        b.targetFrames = loopFrames(transport, in, sampleRate, maxFrames);
        b.hostFrame = hostPlayhead(transport, b.targetFrames, sampleRate);

        b.risePlay = rose(in.play, play_);
        b.fallPlay = fell(in.play, play_);
        b.riseRecord = rose(in.record, record_);
        b.fallRecord = fell(in.record, record_);
        b.riseDub = rose(in.dub, dub_);
        b.fallDub = fell(in.dub, dub_);
        b.riseUndo = rose(in.undo, undo_);
        b.riseRedo = rose(in.redo, redo_);
        b.riseClear = rose(in.clear, clear_);
        b.riseReverse = rose(in.reverse, reverse_);
        b.riseStop = rose(in.stop, stop_);
        b.riseSync = rose(in.sync, sync_);
        b.riseExport = rose(in.exportWav, export_);
        b.hostPlayRise = rose(b.hostPlaying, hostPlay_);
        b.hostPlayFall = fell(b.hostPlaying, hostPlay_);
        b.hostRecRise = rose(b.hostRecording, hostRec_);
        b.hostRecFall = fell(b.hostRecording, hostRec_);

        play_ = in.play; record_ = in.record; dub_ = in.dub;
        undo_ = in.undo; redo_ = in.redo; clear_ = in.clear;
        reverse_ = in.reverse; stop_ = in.stop; sync_ = in.sync; export_ = in.exportWav;
        hostPlay_ = b.hostPlaying; hostRec_ = b.hostRecording;
        return b;
    }

private:
    static float clamp01(float v) { return mpc::clamp(v, 0.0f, 1.0f); }
    static bool rose(bool now, bool was) { return now && !was; }
    static bool fell(bool now, bool was) { return !now && was; }

    static double bpmOf(const mpc::Transport& t)
    {
        return (t.tempoValid && t.bpm > 1.0 && t.bpm < 999.0) ? t.bpm : 120.0;
    }

    static double quartersPerBar(const mpc::Transport& t)
    {
        int num = t.timeSigNumerator >= 1 && t.timeSigNumerator <= 16 ? t.timeSigNumerator : 4;
        int den = t.timeSigDenominator >= 1 && t.timeSigDenominator <= 16 ? t.timeSigDenominator : 4;
        return (double)num * (4.0 / (double)den);
    }

    // Musical length of the next take: the host cycle when it has one, otherwise Bars.
    // /2, x1 and x2 scale that length. The result is clamped to the prepared buffer.
    static int loopFrames(const mpc::Transport& t, const ControlInput& in, double sampleRate, int maxFrames)
    {
        const double bpm = bpmOf(t);
        double quarters = 0.0;
        if (t.valid && t.cycleEndPpq > t.cycleStartPpq + 0.01)
            quarters = t.cycleEndPpq - t.cycleStartPpq;
        else
        {
            int bars = in.bars < 1 ? 1 : (in.bars > 32 ? 32 : in.bars);
            quarters = bars * quartersPerBar(t);
        }
        const double ratio = in.length <= 0 ? 0.5 : (in.length >= 2 ? 2.0 : 1.0);
        quarters *= ratio;
        if (quarters < 0.25) quarters = 0.25;
        double seconds = quarters * 60.0 / bpm;
        const double cap = maxFrames > 0 && sampleRate > 0.0 ? (double)maxFrames / sampleRate : 32.0;
        if (seconds > cap) seconds = cap;
        int frames = (int)(seconds * sampleRate + 0.5);
        if (frames < 64) frames = 64;
        if (maxFrames > 0 && frames > maxFrames) frames = maxFrames;
        return frames;
    }

    static int hostPlayhead(const mpc::Transport& t, int frames, double sampleRate)
    {
        if (!t.ppqValid || frames <= 1 || sampleRate <= 0.0) return -1;
        const double bpm = bpmOf(t);
        const double loopQuarters = (double)frames / sampleRate * bpm / 60.0;
        if (loopQuarters <= 0.0) return 0;
        const double origin = (t.cycleEndPpq > t.cycleStartPpq) ? t.cycleStartPpq : 0.0;
        double phase = (t.ppqPos - origin) / loopQuarters;
        phase -= std::floor(phase);
        if (phase < 0.0) phase += 1.0;
        int frame = (int)(phase * (double)frames);
        if (frame < 0) frame = 0;
        if (frame >= frames) frame = frames - 1;
        return frame;
    }

    // True at the start of a bar, so Next Bar can open a take on the boundary.
    static bool barIsOpen(const mpc::Transport& t)
    {
        if (!t.ppqValid) return true;
        const double bar = quartersPerBar(t);
        if (bar <= 0.0) return true;
        double into = t.ppqPos - t.barStartPpq;
        if (into < 0.0) into = 0.0;
        const double frac = into / bar;
        return frac < 0.02 || frac > 0.98;
    }

    bool play_ = false, record_ = false, dub_ = false;
    bool undo_ = false, redo_ = false, clear_ = false, reverse_ = false;
    bool stop_ = false, sync_ = false, export_ = false;
    bool hostPlay_ = false, hostRec_ = false;
};

} // namespace retro
