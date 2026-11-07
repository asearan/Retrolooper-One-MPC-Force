#pragma once

enum { FORCE_MATRIX_MODE = 0x209c, FORCE_NO_POPUP = 0x3080, FORCE_INSERT_EDITOR = 0x30fa };
static struct {
    uint32_t effect, token, project, source, started, pad;
    int matrix_ready;
    RloForceClipTarget target;
} force_picker;
#ifdef RLO_TRACK_BRIDGE_TEST
static void (*test_force_picker_select)(uint32_t, uint32_t, uint32_t, unsigned);
#endif

static int force_picker_context(uint32_t effect) {
    if (!word_is(native_engine_mode, native_base + 0x6ca2364)) return 0;
    uint32_t mode = native_word(native_engine_mode + 0x40);
    uint32_t popup = native_word(native_engine_mode + 0x6c);
    if (popup == FORCE_NO_POPUP) return mode == FORCE_MATRIX_MODE;
    RloTrackSnapshot view;
    return rlo_pad_context_allowed((mode << 16) | popup) && snapshot_read(&view) &&
           now_ms() - view.stamp <= 100 && view.editor_open && view.editor_fx == effect;
}

/* Begin a touchscreen pick with no Matrix slot selected. This makes tapping
   the previously selected slot observable, too. Native selection never launches. */
static int force_picker_begin_touch(void) {
    if (native_word(native_project + 0x1bc) & 1) return 1;
    uint32_t track = native_word(native_project + 0xa0);
    uint32_t sequence = native_word(native_project + 0xe4);
    if (!track || !sequence) return 0;
#ifdef RLO_TRACK_BRIDGE_TEST
    if (!test_force_picker_select) return 0;
    test_force_picker_select(native_project, track, sequence, 1);
#else
    if (!word_is(native_base + 0x28fee70, 0xe16d41f0)) return 0;
    ((void(*)(uint32_t, uint32_t, uint32_t, unsigned))(native_base + 0x28fee70))(
        native_project, track, sequence, 1);
#endif
    return (native_word(native_project + 0x1bc) & 1) != 0;
}

static int force_picker_touch_target(RloForceClipTarget *target) {
    uint32_t track = native_word(native_project + 0x1b4);
    return word_is(track, native_base + 0x6d50644) && native_word(track + 0x64c) == 6 &&
           force_clip_target(track, target);
}

/* Resolve the same visible Matrix column and row that the native pad
   selection action uses. Call its select operation only, never launch. */
static int force_picker_select(uint32_t effect, unsigned pad, RloForceClipTarget *target) {
    RloEditorOwner editor = {0};
    if (native_word(native_engine_mode + 0x6c) == FORCE_INSERT_EDITOR &&
        (!editor_owner(&editor) || editor.effect != effect)) return 0;
    uint32_t engine = force_engine();
    uint32_t controller = native_word(engine + 0x7b8);
    uint32_t parent = native_word(controller);
    uint32_t behaviour = parent ? parent + 0x14 : 0;
    if (!engine || !word_is(behaviour, native_base + 0x6ca8428) ||
        native_word(behaviour + 0x48) != native_project) return 0;
    uint32_t view = native_word(behaviour + 0x40);
    if (view < 280 || !word_is(view, native_base + 0x6c9a9b0)) return 0;
#ifndef RLO_TRACK_BRIDGE_TEST
    if (!word_is(native_base + 0x6c9a9b0 + 0x10, native_base + 0xe57ac8) ||
        !word_is(native_base + 0xe57ac8, 0xe52d4008) ||
        !word_is(native_base + 0x10185c4, 0xe59031bc) ||
        !word_is(native_base + 0x6c9bfe0 + 0x24, native_base + 0x10185c4) ||
        !word_is(native_base + 0x28fee70, 0xe16d41f0)) return 0;
#endif
    uint32_t aperture = view - 280;
    unsigned x = native_word(aperture + 0x14) + pad % 8;
    unsigned y = native_word(aperture + 0x28) + pad / 8;
    if (x >= MAX_NATIVE_TRACKS || !y || y >= 4096) return 0;
    uint32_t split = native_word(view + 0x1b4);
    if (!split) return 0;
    uint32_t lanes = native_word(view + (native_word(split + 0x14) == 0 ? 0x178 : 0x180));
    if (!word_is(lanes, native_base + 0x6c9bfe0) ||
        native_word(lanes + 0x184) != native_project ||
        y >= native_word(lanes + 0x48)) return 0;
    uint32_t columns = native_word(lanes + 0x1bc);
    uint32_t begin = native_word(columns + 4), end = native_word(columns + 8);
    if (!begin || end < begin || (end - begin) % 4 || end - begin > MAX_NATIVE_TRACKS * 4 ||
        x >= (end - begin) / 4) return 0;
    uint32_t track = native_word(begin + 4 * x);
    uint32_t clip, owner;
    if (!word_is(track, native_base + 0x6d50644) || native_word(track + 0x64c) != 6 ||
        !force_clip_slot(native_word(native_project + 0xb20), track, y, &clip, &owner))
        return 0;
#ifdef RLO_TRACK_BRIDGE_TEST
    if (!test_force_picker_select) return 0;
    test_force_picker_select(native_project, track, y, 0);
#else
    ((void(*)(uint32_t, unsigned, unsigned))(native_base + 0x10185c4))(lanes, x, y);
#endif
    // Keep this editor visible when the pad selects a different track.
    // The next explicit track or editor change resumes normal following.
    if (editor.effect && editor.track != track && word_is(native_project + 0xa0, track)) {
        force_clip_editor = editor;
        force_clip_editor_selection = track;
    }
    return force_clip_target(track, target) && target->row == y;
}

static int force_source_owns(uint32_t track, uint32_t effect) {
    uint32_t pool = native_word(native_project + 0xb1c);
    if (!word_is(pool, native_base + 0x6cb6548)) return 0;
    uint32_t begin = native_word(pool + 0x60), end = native_word(pool + 0x64);
    if (!begin || end < begin || (end - begin) % 4 || end - begin > MAX_NATIVE_TRACKS * 4)
        return 0;
    uint32_t tracks[MAX_NATIVE_TRACKS];
    unsigned total = (end - begin) / 4, index = 0;
    if (!total || !native_read(tracks, begin, end - begin)) return 0;
    while (index < total && tracks[index] != track) ++index;
    if (index == total) return 0;
    uint32_t program, arm, effects[MAX_TRACK_FX];
    unsigned count;
    if (!track_effects(track, &program, &arm, effects, &count)) return 0;
    for (unsigned i = 0; i < count; ++i) if (effects[i] == effect) return 1;
    return 0;
}

static int force_picker_pad(void *unused, unsigned pad, int press) {
    (void)unused;
    if (!press) return 0;
    pthread_mutex_lock(&force_bounce_mutex);
    int picking = force_picker.effect && !force_picker.target.project &&
                  now_ms() - force_picker.started < 60000 && force_picker_context(force_picker.effect);
    if (picking && !force_picker.pad) force_picker.pad = pad + 1;
    pthread_mutex_unlock(&force_bounce_mutex);
    return picking;
}

static void force_picker_refresh(void) {
    if (pthread_mutex_trylock(&force_bounce_mutex)) return;
    if (!force_picker.effect || force_picker.target.project) {
        pthread_mutex_unlock(&force_bounce_mutex);
        return;
    }
    uint32_t effect = force_picker.effect, token = force_picker.token, source = force_picker.source;
    uint32_t project = force_picker.project, started = force_picker.started;
    unsigned pad = force_picker.pad;
    force_picker.pad = 0;
    int matrix_ready = force_picker.matrix_ready;
    pthread_mutex_unlock(&force_bounce_mutex);

    int valid = now_ms() - started < 60000 && project == native_project && force_source_owns(source, effect);
    RloForceClipTarget target = {0};
    if (valid) {
        if (pad && force_picker_context(effect)) force_picker_select(effect, pad - 1, &target);
        else if (native_word(native_engine_mode + 0x40) == FORCE_MATRIX_MODE &&
                 native_word(native_engine_mode + 0x6c) == FORCE_NO_POPUP) {
            if (matrix_ready) force_picker_touch_target(&target);
            else matrix_ready = force_picker_begin_touch();
        } else matrix_ready = 0;
    }
    pthread_mutex_lock(&force_bounce_mutex);
    if (force_picker.effect == effect && force_picker.token == token) {
        if (!valid) memset(&force_picker, 0, sizeof force_picker);
        else {
            force_picker.matrix_ready = matrix_ready;
            if (target.project) {
                force_picker.target = target;
                force_picker.pad = 0;
            }
        }
    }
    pthread_mutex_unlock(&force_bounce_mutex);
}

__attribute__((visibility("default"))) int retrolooper_force_picker_begin(void *effect, uint32_t token) {
    RloTrackSnapshot view;
    if (!effect || !token || !snapshot_read(&view) || !view.project || !view.editor_track ||
        now_ms() - view.stamp > 100 || !view.editor_open || view.editor_fx != (uintptr_t)effect)
        return RLO_CLIP_SELECTION_CHANGED;
    if (pthread_mutex_trylock(&force_bounce_mutex)) return RLO_CLIP_BUSY;
    if (force_picker.effect || force_bounce.id) {
        pthread_mutex_unlock(&force_bounce_mutex);
        return RLO_CLIP_BUSY;
    }
    force_picker.effect = (uintptr_t)effect;
    force_picker.token = token;
    force_picker.project = view.project;
    force_picker.source = view.editor_track;
    force_picker.started = now_ms();
    pthread_mutex_unlock(&force_bounce_mutex);
    return RLO_CLIP_PENDING;
}

__attribute__((visibility("default"))) int retrolooper_force_picker_poll(
        void *effect, uint32_t token, RloForceClipTarget *out, int cancel) {
    pthread_mutex_lock(&force_bounce_mutex);
    int result = RLO_CLIP_SELECTION_CHANGED;
    if (force_picker.effect == (uintptr_t)effect && force_picker.token == token) {
        if (cancel) memset(&force_picker, 0, sizeof force_picker);
        else if (force_picker.target.project && out) {
            *out = force_picker.target;
            result = RLO_CLIP_BOUNCED;
        } else result = RLO_CLIP_PENDING;
    }
    pthread_mutex_unlock(&force_bounce_mutex);
    return result;
}
