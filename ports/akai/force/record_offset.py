"""Force recording offset; leave the released MPC audio path intact."""


def prepare(workspace, adapt):
    source = workspace / "src/Plugin.cpp"
    adapt(source, "kStateVersion=2,kLegacyParams", "kStateVersion=3,kLegacyParams", 1)
    adapt(source, "version!=1&&version!=kStateVersion", "version!=1&&version!=2&&version!=kStateVersion", 1)
    adapt(source, "    void configure() {\n", """    void configure() {
#ifdef RETROLOOPER_FORCE_BOUNCE
        engine->setRecordLatencyCompensationSamples(hostMode()?recordOffsetFrames():0);
#endif
""", 1)
    adapt(source, "    bool hostMode() const {return on(host_sync);}", """    int recordOffsetFrames() const {
#ifdef RETROLOOPER_FORCE_BOUNCE
        const int selected = choice(record_offset);
        // Force 3.10 reports zero for both VST device-latency queries.
        // Use the verified built-in analog input setting for Auto.
        const int milliseconds = selected < 0 ? 7 : selected;
        return static_cast<int>(std::lround(milliseconds * rate / 1000.));
#else
        return 0;
#endif
    }
    bool hostMode() const {return on(host_sync);}""", 1)
    adapt(source, "        double phase=std::fmod(hostBeat+offset*perSample-captureOrigin(),span);if(phase<0)phase+=span;", """        // Capture the input window after the audible timeline boundary when
        // compensating round-trip delay. The stored loop still starts at the boundary.
        double phase=std::fmod(hostBeat+(offset-recordOffsetFrames())*perSample-captureOrigin(),span);if(phase<0)phase+=span;""", 1)
    adapt(source, """        if(engine->captureStorageCapacity()<needed||(on(auto_start)&&engine->captureStorageSize()<needed)){storageRequested.store(needed);messageCode.store(failedStorageRequest==needed?MemoryLimit:PreparingAudio);return;}
        if(on(auto_start)){
            const double quarters=captureSpan()*lengthMultiplier();
            double phase=std::fmod(hostBeat+offset*hostTempo/(60*rate)-captureOrigin(),quarters);if(phase<0)phase+=quarters;
            engine->hostBeginThresholdCapture(static_cast<std::size_t>(std::llround(count)),static_cast<std::size_t>(std::llround(phase*60*rate/hostTempo)));""", """        const auto advance=static_cast<std::size_t>(recordOffsetFrames());
        const bool circular=on(auto_start)||advance!=0;
        if(engine->captureStorageCapacity()<needed||(circular&&engine->captureStorageSize()<needed)){storageRequested.store(needed);messageCode.store(failedStorageRequest==needed?MemoryLimit:PreparingAudio);return;}
        if(circular){
            const auto frames=static_cast<std::size_t>(std::llround(count));
            const double quarters=captureSpan()*lengthMultiplier();
            double phase=std::fmod(hostBeat+offset*hostTempo/(60*rate)-captureOrigin(),quarters);if(phase<0)phase+=quarters;
            const auto phaseFrames=on(auto_start)?static_cast<std::size_t>(std::llround(phase*60*rate/hostTempo))%frames:advance%frames;
            const auto writePhase=(phaseFrames+frames-advance%frames)%frames;
            engine->hostBeginThresholdCapture(frames,phaseFrames,writePhase);""", 1)
    adapt(source, '        if(i==clip_picker){text(dest,pickerProject.load()?"PICK DESTINATION":"",24);return;}\n',
        '        if(i==clip_picker){text(dest,pickerProject.load()?"PICK DESTINATION":"",24);return;}\n'
        '''        if(i==record_offset){
            const int selected = choice(i);
            if(selected < 0) text(dest,"Auto (7 ms)",24);
            else {
                std::snprintf(buf,sizeof buf,"%d ms",selected);
                text(dest,buf,24);
            }
            return;
        }
''', 1)
    adapt(source, "            ||count==266||count==267", "            ||count==266||count==267||count==270||count==271", 1)
    adapt(source, "        if(count<=216)params[capture_source]", """        // The 0.2.7 range was 0–50 ms. Move its default 0 to Auto and
        // preserve any nonzero manual calibration in the new range.
        if(version == 2 && count > record_offset){
            const auto previous = std::lround(params[record_offset] * 50.f);
            params[record_offset] = norm(record_offset,previous == 0 ? -1 : previous);
        }
        if(count<=216)params[capture_source]""", 1)

    storage = workspace / "src/HostLoopStorage.inc"
    adapt(storage, "void hostBeginThresholdCapture(std::size_t frames,std::size_t phase) noexcept {",
        "void hostBeginThresholdCapture(std::size_t frames,std::size_t phase,std::size_t writePhase) noexcept {", 1)
    adapt(storage, """    hostThresholdFrames_=frames;hostThresholdOffset_=phase%frames;
    hostThresholdOrigin_=pos-static_cast<std::int64_t>(hostThresholdOffset_);""", """    hostThresholdFrames_=frames;hostThresholdOffset_=writePhase%frames;
    hostThresholdOrigin_=pos-static_cast<std::int64_t>(phase%frames);""", 1)
