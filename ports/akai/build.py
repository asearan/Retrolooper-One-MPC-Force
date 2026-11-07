#!/usr/bin/env python3
"""Build an ARM Linux effect and/or run the native ABI/DSP regression host.
Python 3.9+, Pillow for skins. ARM build: Zig 0.13.0. No JUCE/Steinberg SDK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
BUILD = ROOT/"build"
KIT_COMMIT = "6dd48faa414bd9e3390e5824b0a0e3126f965dbd"
ENGINE_SOURCES = ["AudioImportAnalysisService.cpp", "LoopTrackStateMachine.cpp", "LooperEngine.cpp", "TaggedTempo.cpp",
                  "TempoEstimator.cpp", "TransportClock.cpp", "QuantizeScheduler.cpp", "Track.cpp", "TempoManager.cpp", "RealtimeTransientDetector.cpp"]

def run(args, **kw):
    print("+", " ".join(map(str,args)), flush=True)
    subprocess.run(list(map(str,args)),check=True,**kw)

def snapshot(force_record_offset=False):
    """The parent sources stay untouched. These narrowly scoped adaptations are checked, not guessed."""
    target=BUILD/"engine"
    shutil.copytree(REPO/"engine", target, dirs_exist_ok=True)
    changes=[]
    def replace(file, before, after):
        path=target/file
        text=path.read_text()
        if text.count(before)!=1: raise SystemExit(f"Source changed: review adaptation in {file}")
        path.write_text(text.replace(before,after));changes.append(dict(file=file,before=before,after=after))
    replace("LooperEngine.h", "kMaxFirstPassSeconds = 600", "kMaxFirstPassSeconds = 120")
    replace("LooperEngine.cpp", "        ensureStretchEngine(i);", "        if (i == 0) ensureStretchEngine(i); // Only track 1 exists in this product.")
    # MPC uses mlockall: untouched malloc/new reservations still consume RAM.
    # Empty instances must not reserve ten copies of a 120-second stereo loop.
    replace("LooperEngine.cpp", "    prepareLiveRecordHeadroom();", "    // The port's worker reserves the actual take length before capture.")
    replace("LooperEngine.cpp", "        tracks_[static_cast<std::size_t>(i)].reserveRecordHeadroom(headroom);",
            "        if (i == 0) tracks_[0].reserveRecordHeadroom(headroom); // Legacy standalone preparation only.")
    replace("LooperEngine.h", "    void prepareLiveRecordHeadroom() noexcept;", "    void prepareLiveRecordHeadroom() noexcept;\n    // Port only: caller excludes callbacks; allocation stays off the audio thread.\n    void reserveCaptureStorage(std::size_t frames) { tracks_[0].reserveRecordHeadroom(frames); tracks_[0].ensureRealtimeCapacity(frames); }\n    std::size_t captureStorageSize() const noexcept { return tracks_[0].bufferCapacity(); }\n    std::size_t captureStorageCapacity() const noexcept { return tracks_[0].captureStorageCapacity(); }\n#include \"HostLoopStorage.inc\"")
    replace("_segment_gen.cpp", "track.writeSample(channel, whead, inputSample);", "track.writeSample(channel, hostCaptureWriteIndex(ti,whead), inputSample);")
    if force_record_offset:
        replace("_segment_gen.cpp", "double ph = static_cast<double>(ts - phaseOrigin) * stretchRatio;", "double ph = static_cast<double>(ts - phaseOrigin - effectiveRecordLatencyCompensationSamples(trackIndex)) * stretchRatio;")
    replace("LooperEngine.cpp", "    const bool forcePlaying = finishRecordingToPlaying_[ti];", "    if(trackIndex==0&&hostThresholdFrames_)track.setRecordedSamples(hostThresholdFrames_);\n    const bool forcePlaying = finishRecordingToPlaying_[ti];")
    replace("LooperEngine.cpp", "    const std::int64_t recordGate = trackRecordInputGateTransport_[ti];", "    const std::int64_t recordGate = trackIndex==0&&hostThresholdFrames_?hostThresholdOrigin_:trackRecordInputGateTransport_[ti];")
    if force_record_offset:
        replace("LooperEngine.cpp", "        const int comp = effectiveRecordLatencyCompensationSamples(trackIndex);", "        const int comp = trackIndex==0&&hostThresholdFrames_?0:effectiveRecordLatencyCompensationSamples(trackIndex);")
    replace("LooperEngine.cpp", "    refreshIsRecordingFlag();\n    if (waveformEnvelopes_ != nullptr)\n    {\n        waveformEnvelopes_->rebuildFromTrack(trackIndex, track);", "    hostCompleteThresholdCapture(trackIndex,transportBoundaryExclusive>=0?transportBoundaryExclusive:transport_.samplePosition());\n    refreshIsRecordingFlag();\n    if (waveformEnvelopes_ != nullptr)\n    {\n        waveformEnvelopes_->rebuildFromTrack(trackIndex, track);")
    replace("LooperEngine.cpp", "    // Realtime-safe: keep reserved capacity. clearAudio() takes a mutex and can hitch the first hit.", "    if(trackIndex==0)hostThresholdFrames_=hostThresholdOffset_=0;\n    // Realtime-safe: keep reserved capacity. clearAudio() takes a mutex and can hitch the first hit.")
    replace("Track.h", "    void reserveRecordHeadroom(std::size_t samples) noexcept;", "    void reserveRecordHeadroom(std::size_t samples);")
    replace("Track.cpp", "void Track::reserveRecordHeadroom(std::size_t samples) noexcept", "void Track::reserveRecordHeadroom(std::size_t samples)")
    replace("Track.h", "    [[nodiscard]] std::size_t allocatedCapacity() const noexcept;", "    [[nodiscard]] std::size_t allocatedCapacity() const noexcept;\n    std::size_t captureStorageCapacity() const noexcept {\n        auto frames=allocatedCapacity();\n        for (const auto& bank:historyBanks_) frames=std::min(frames,bank.capacity);\n        return frames;\n    }")
    # Missing preparation must never fall back to desktop multi-minute slabs
    # on the realtime callback. The plugin waits for the worker instead.
    replace("Track.cpp", "    if (needRealloc)\n    {", "    if (needRealloc && realtimeActive_.load(std::memory_order_acquire)) return;\n    if (needRealloc)\n    {")
    replace("Track.cpp", "kMinReserve = 48000u * 180u", "kMinReserve = 512u")
    replace("Track.cpp", "kGrow = 48000u * 90u", "kGrow = 512u")
    # ARMv7 has a separate FPSCR; the shared engine currently only flushes SSE/ARM64.
    replace("FlushDenormals.h", "#else\n    ScopedNoDenormals() noexcept {}", "#elif defined(__arm__) && defined(__VFP_FP__)\n    std::uint32_t old_{0};\n    ScopedNoDenormals() noexcept { asm volatile(\"vmrs %0, fpscr\" : \"=r\"(old_)); auto v=old_|(1u<<24); asm volatile(\"vmsr fpscr, %0\" :: \"r\"(v)); }\n    ~ScopedNoDenormals() noexcept { asm volatile(\"vmsr fpscr, %0\" :: \"r\"(old_)); }\n#else\n    ScopedNoDenormals() noexcept {}")
    shutil.copy2(REPO/"plugin"/"PluginMidiGestures.h",BUILD/"PluginMidiGestures.h")
    (BUILD/"ui/metering").mkdir(parents=True,exist_ok=True)
    shutil.copy2(REPO/"ui/metering/TrackMeterBallistics.h",BUILD/"ui/metering/TrackMeterBallistics.h")
    manifest={"adaptations":changes,"sha256":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (REPO/"engine").rglob("*") if p.is_file()}}
    manifest["meter_ballistics_sha256"]=hashlib.sha256((REPO/"ui/metering/TrackMeterBallistics.h").read_bytes()).hexdigest()
    (BUILD/"source-manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")

def compile_flags():
    return ["-std=c++20","-O2","-fPIC","-fvisibility=hidden","-pthread","-Wall","-Wextra",
            "-I"+str(BUILD/"engine"),"-I"+str(BUILD),"-I"+str(ROOT/"src"),
            "-I"+str(REPO/"third_party/signalsmith-stretch/include"),"-I"+str(REPO/"third_party/signalsmith-linear/include")]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("action",choices=["test","arm","controls","skin","package","preview","all"])
    p.add_argument("--zig",default=os.environ.get("ZIG","zig"))
    p.add_argument("--kit",default=os.environ.get("MPC_VST_KIT",""))
    p.add_argument("--sanitize",action="store_true")
    a=p.parse_args()
    BUILD.mkdir(exist_ok=True)
    env={**os.environ,"ZIG_GLOBAL_CACHE_DIR":str(BUILD/"zig-cache"),"ZIG_LOCAL_CACHE_DIR":str(BUILD/"zig-local")}
    if a.action in ("arm","controls","all"):
        # Firmware-specific code belongs only in the separately shipped Device patch.
        control=ROOT/"control"
        target=["-target","arm-linux-gnueabihf.2.28","-mcpu=cortex_a17","-O2","-fPIC","-fvisibility=hidden"]
        bridge=BUILD/"mpc-controls-buttons.o"
        run([a.zig,"cc","-std=c11",*target,"-DRLO_MPC_TIMELINE","-c",control/"ButtonBridge.c","-o",bridge],env=env)
        run([a.zig,"c++","-std=c++17",*target,"-shared",control/"NativeTimelineCall.cpp",bridge,
             "-pthread","-ldl","-s","-Wl,--no-undefined","-Wl,--version-script="+str(control/"exports.map"),
             "-o",control/"retrolooper_buttons.so"],env=env)
        run([sys.executable,control/"check_exports.py"])
    if a.action=="controls":
        return
    run([sys.executable,ROOT/"tools/parameters.py"])
    snapshot()
    sources=[BUILD/"engine"/s for s in ENGINE_SOURCES]+[ROOT/"src/Plugin.cpp"]
    if a.action in ("test","all"):
        flags=compile_flags()+["-DRETROLOOPER_TESTING"]+(["-fsanitize=address,undefined","-fno-omit-frame-pointer","-g"] if a.sanitize else [])
        run(["clang++",*flags,*sources,ROOT/"tests/HostTests.cpp","-Wl,-export_dynamic","-o",BUILD/"host-tests"])
        run([BUILD/"host-tests"],env={**os.environ,"RETROLOOPER_TEST_LOOPS":str(BUILD/"test-loops")})
    if a.action in ("arm","all"):
        run([a.zig,"c++","-target","arm-linux-gnueabihf.2.28","-mcpu=cortex_a17",*compile_flags(),"-shared",
             *sources,"-s","-Wl,--no-undefined","-Wl,--version-script="+str(ROOT/"src/exports.map"),"-ldl","-o",BUILD/"retrolooper_one.so"],env=env)
        run(["file",BUILD/"retrolooper_one.so"])
    if a.action in ("preview","all"):
        run(["clang++",*compile_flags(),"-dynamiclib",*sources,ROOT/"src/TestBridge.cpp","-o",BUILD/"preview-engine.dylib"])
    if a.action in ("skin","package","all"):
        kit=Path(a.kit).resolve()
        if not (kit/"tools/shadow_skin.py").is_file(): raise SystemExit("Supply --kit /path/to/mpc-vst-plugins at "+KIT_COMMIT)
        actual=subprocess.check_output(["git","-C",str(kit),"rev-parse","HEAD"],text=True).strip()
        if actual!=KIT_COMMIT: raise SystemExit("Porting tools changed; use pinned commit "+KIT_COMMIT)
        run(["cc","-O2","-I"+str(kit/"tools/vendor/force-shadow/tools"),kit/"tools/shadow_art.c","-lm","-o",BUILD/"shadow_art"])
        run([sys.executable,kit/"tools/gen_vst.py",ROOT/"vst.json"],cwd=ROOT,env={**os.environ,"SHADOW_SKIN_MPC_OS":"2"})
        cfg=json.loads((ROOT/"vst.json").read_text())
        generated=BUILD/"skin"/(cfg["vendor"]+" - VST - "+cfg["name"])
        compatible=BUILD/"skin"/(cfg["vendor"]+" - VST - "+cfg["folder_name"])
        if generated!=compatible:generated.rename(compatible)
        run([sys.executable,ROOT/"tools/skin_finish.py","--kit",kit])
        skin=compatible
        run([sys.executable,kit/"tools/skin_check.py",skin])
        run([sys.executable,ROOT/"tools/check_native_skin.py","--kit",kit])
    if a.action in ("package","all"):
        if not (BUILD/"retrolooper_one.so").is_file():raise SystemExit("Build ARM first")
        run([sys.executable,kit/"tools/release.py","--so",BUILD/"retrolooper_one.so","--skin",skin,
             "--entry",BUILD/"pluginlist-entry.xml","--version","0.3.9","--id","retrolooper-one","--about","Stereo Audio insert looper with host sync, touch controls and MIDI. Native pad and track integration is optional.",
             "--user-data","Loops","--user-data","Exports","--extra",ROOT/"vendor/mpc-vst-plugins-LICENSE:licenses/mpc-vst-plugins-LICENSE",
             "--extra",REPO/"third_party/signalsmith-stretch/LICENSE.txt:licenses/signalsmith-stretch-LICENSE.txt",
             "--extra",REPO/"third_party/signalsmith-linear/LICENSE.txt:licenses/signalsmith-linear-LICENSE.txt",
             "--extra",ROOT/"README.md:RETROLOOPER.md",
             "--extra",ROOT/"preview/layout-audit.json:validation/layout-audit.json",
             "--extra",ROOT/"preview/resource-audit.json:validation/resource-audit.json","-o",ROOT/"dist"])
        archive=ROOT/"dist/RetroLooper-One-0.3.9-mpc-armv7.zip"
        (ROOT/"dist/Retrolooper-One-0.3.9-mpc-armv7.zip").replace(archive)
        run([sys.executable,kit/"tools/catalog_check.py",archive])

if __name__=="__main__":main()
