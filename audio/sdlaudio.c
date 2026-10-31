/*
 * QEMU SDL audio driver
 *
 * Copyright (c) 2004-2005 Vassili Karpov (malc)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include <SDL.h>
#include <SDL_thread.h>
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qemu/audio.h"
#include "qom/object.h"

#ifndef _WIN32
#ifdef __sun__
#define _POSIX_PTHREAD_SEMANTICS 1
#elif defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__DragonFly__)
#include <pthread.h>
#endif
#endif

#include "audio_int.h"

#define TYPE_AUDIO_SDL "audio-sdl"
OBJECT_DECLARE_SIMPLE_TYPE(AudioSdl, AUDIO_SDL)

static AudioBackendClass *audio_sdl_parent_class;

struct AudioSdl {
    AudioMixengBackend parent_obj;
};


typedef struct SDLVoiceOut {
    HWVoiceOut hw;
    int exit;
    int initialized;
    Audiodev *dev;
    SDL_AudioDeviceID devid;
    SDL_AudioSpec reopen_spec;
    char *device_name;
    gint64 next_route_check;
    bool route_matches;
    bool enabled;
} SDLVoiceOut;

typedef struct SDLVoiceIn {
    HWVoiceIn hw;
    int exit;
    int initialized;
    Audiodev *dev;
    SDL_AudioDeviceID devid;
    SDL_AudioSpec reopen_spec;
    char *device_name;
    gint64 next_route_check;
    bool route_matches;
    bool enabled;
} SDLVoiceIn;

static int aud_to_sdlfmt (AudioFormat fmt)
{
    switch (fmt) {
    case AUDIO_FORMAT_S8:
        return AUDIO_S8;

    case AUDIO_FORMAT_U8:
        return AUDIO_U8;

    case AUDIO_FORMAT_S16:
        return AUDIO_S16LSB;

    case AUDIO_FORMAT_U16:
        return AUDIO_U16LSB;

    case AUDIO_FORMAT_S32:
        return AUDIO_S32LSB;

    /* no unsigned 32-bit support in SDL */

    case AUDIO_FORMAT_F32:
        return AUDIO_F32LSB;

    default:
        error_report("sdl: internal logic error: bad audio format %d", fmt);
        return AUDIO_U8;
    }
}

static int sdl_to_audfmt(int sdlfmt, AudioFormat *fmt, bool *big_endian)
{
    switch (sdlfmt) {
    case AUDIO_S8:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_S8;
        break;

    case AUDIO_U8:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_U8;
        break;

    case AUDIO_S16LSB:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_S16;
        break;

    case AUDIO_U16LSB:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_U16;
        break;

    case AUDIO_S16MSB:
        *big_endian = true;
        *fmt = AUDIO_FORMAT_S16;
        break;

    case AUDIO_U16MSB:
        *big_endian = true;
        *fmt = AUDIO_FORMAT_U16;
        break;

    case AUDIO_S32LSB:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_S32;
        break;

    case AUDIO_S32MSB:
        *big_endian = true;
        *fmt = AUDIO_FORMAT_S32;
        break;

    case AUDIO_F32LSB:
        *big_endian = false;
        *fmt = AUDIO_FORMAT_F32;
        break;

    case AUDIO_F32MSB:
        *big_endian = true;
        *fmt = AUDIO_FORMAT_F32;
        break;

    default:
        error_report("sdl: unrecognized audio format %d", sdlfmt);
        return -1;
    }

    return 0;
}

/*
 * The launcher gives QEMU an initial direction-aware route through the
 * environment. A private, atomically replaced control file then lets the
 * guest-facing bridge change that route without restarting the VM.
 */
static char *sdl_initial_device_name(int rec)
{
    const char *name = g_getenv(rec ? "OMARCHY_SDL_INPUT_DEVICE_NAME" :
                                     "OMARCHY_SDL_OUTPUT_DEVICE_NAME");

    return name && *name ? g_strdup(name) : NULL;
}

static char *sdl_route_file_device_name(int rec, bool *present)
{
    const char *directory = g_getenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY");
    g_autofree char *path = NULL;
    g_autofree char *contents = NULL;
    g_autofree guchar *decoded = NULL;
    gsize decoded_length = 0;

    *present = false;
    if (!directory || !*directory) {
        return NULL;
    }
    path = g_build_filename(directory, rec ? "input" : "output", NULL);
    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        return NULL;
    }
    g_strchomp(contents);
    *present = true;
    if (g_str_equal(contents, "default")) {
        return NULL;
    }
    decoded = g_base64_decode(contents, &decoded_length);
    if (!decoded_length || memchr(decoded, '\0', decoded_length) ||
        !g_utf8_validate((const char *)decoded, decoded_length, NULL)) {
        warn_report("Ignoring invalid Omarchy SDL route file");
        *present = false;
        return NULL;
    }
    return g_strndup((const char *)decoded, decoded_length);
}

static bool sdl_audio_specs_match(const SDL_AudioSpec *left,
                                  const SDL_AudioSpec *right)
{
    return left->freq == right->freq &&
           left->format == right->format &&
           left->channels == right->channels &&
           left->samples == right->samples;
}

static bool sdl_route_check_due(gint64 *next_check)
{
    gint64 now = g_get_monotonic_time();

    if (now < *next_check) {
        return false;
    }
    *next_check = now + 250 * 1000;
    return true;
}

static SDL_AudioDeviceID sdl_open(SDL_AudioSpec *req, SDL_AudioSpec *obt,
                                  int rec, const char *device_name,
                                  bool *route_matches)
{
    SDL_AudioDeviceID devid;
#ifndef _WIN32
    int err;
    sigset_t new, old;

    /* Make sure potential threads created by SDL don't hog signals.  */
    err = sigfillset (&new);
    if (err) {
        error_report("sdl: sigfillset failed: %s", strerror (errno));
        return 0;
    }
    err = pthread_sigmask (SIG_BLOCK, &new, &old);
    if (err) {
        error_report("sdl: pthread_sigmask failed: %s", strerror (err));
        return 0;
    }
#endif

    devid = SDL_OpenAudioDevice(device_name, rec, req, obt, 0);
    *route_matches = devid || !device_name;
    if (!devid && device_name) {
        warn_report(
                "Requested SDL %s device disappeared; using system default",
                rec ? "recording" : "playback");
        SDL_ClearError();
        devid = SDL_OpenAudioDevice(NULL, rec, req, obt, 0);
        if (devid) {
            *route_matches = false;
        }
    }
    if (!devid) {
        error_report("SDL_OpenAudioDevice for %s failed: %s",
                   rec ? "recording" : "playback", SDL_GetError());
    }

#ifndef _WIN32
    err = pthread_sigmask (SIG_SETMASK, &old, NULL);
    if (err) {
        error_report("sdl: pthread_sigmask (restore) failed: %s",
                     strerror (errno));
        /* We have failed to restore original signal mask, all bets are off,
           so exit the process */
        exit (EXIT_FAILURE);
    }
#endif
    return devid;
}

static void sdl_close_out(SDLVoiceOut *sdl)
{
    if (sdl->initialized) {
        SDL_LockAudioDevice(sdl->devid);
        sdl->exit = 1;
        SDL_UnlockAudioDevice(sdl->devid);
        SDL_PauseAudioDevice(sdl->devid, 1);
        sdl->initialized = 0;
    }
    if (sdl->devid) {
        SDL_CloseAudioDevice(sdl->devid);
        sdl->devid = 0;
    }
}

static void sdl_callback_out(void *opaque, Uint8 *buf, int len)
{
    SDLVoiceOut *sdl = opaque;
    HWVoiceOut *hw = &sdl->hw;

    if (!sdl->exit) {

        while (hw->pending_emul && len) {
            size_t write_len, start;

            start = audio_ring_posb(hw->pos_emul, hw->pending_emul,
                                    hw->size_emul);
            assert(start < hw->size_emul);

            write_len = MIN(MIN(hw->pending_emul, len),
                            hw->size_emul - start);

            memcpy(buf, hw->buf_emul + start, write_len);
            hw->pending_emul -= write_len;
            len -= write_len;
            buf += write_len;
        }
    }

    /* clear remaining buffer that we couldn't fill with data */
    if (len) {
        audio_pcm_info_clear_buf(&hw->info, buf,
                                 len / hw->info.bytes_per_frame);
    }
}

static void sdl_close_in(SDLVoiceIn *sdl)
{
    if (sdl->initialized) {
        SDL_LockAudioDevice(sdl->devid);
        sdl->exit = 1;
        SDL_UnlockAudioDevice(sdl->devid);
        SDL_PauseAudioDevice(sdl->devid, 1);
        sdl->initialized = 0;
    }
    if (sdl->devid) {
        SDL_CloseAudioDevice(sdl->devid);
        sdl->devid = 0;
    }
}

static void sdl_callback_in(void *opaque, Uint8 *buf, int len)
{
    SDLVoiceIn *sdl = opaque;
    HWVoiceIn *hw = &sdl->hw;

    if (sdl->exit) {
        return;
    }

    while (hw->pending_emul < hw->size_emul && len) {
        size_t read_len = MIN(len, MIN(hw->size_emul - hw->pos_emul,
                                       hw->size_emul - hw->pending_emul));

        memcpy(hw->buf_emul + hw->pos_emul, buf, read_len);

        hw->pending_emul += read_len;
        hw->pos_emul = (hw->pos_emul + read_len) % hw->size_emul;
        len -= read_len;
        buf += read_len;
    }
}

/*
 * SDL keeps the physical host device running whenever it is open, so a merely
 * paused voice still costs a full host render cycle while the guest is silent.
 * Release the device outright when the frontend stops the stream and take it
 * again when the stream starts. The reopen reuses the spec the voice was
 * initialised with, so the guest never observes a format change.
 *
 * Reacquiring costs one device open on the first sample after silence. The
 * frontend toggles this when the guest starts or stops a stream, not per
 * buffer, so that open never lands in a hot path.
 */
static void sdl_release_out(SDLVoiceOut *sdl)
{
    if (!sdl->devid) {
        return;
    }
    SDL_PauseAudioDevice(sdl->devid, 1);
    SDL_CloseAudioDevice(sdl->devid);
    sdl->devid = 0;
}

static bool sdl_acquire_out(SDLVoiceOut *sdl)
{
    g_autofree char *requested = NULL;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID devid;
    bool present;
    bool route_matches;

    requested = sdl_route_file_device_name(0, &present);
    if (!present) {
        requested = g_strdup(sdl->device_name);
    }
    devid = sdl_open(&sdl->reopen_spec, &obtained, 0, requested,
                     &route_matches);
    if (!devid) {
        return false;
    }
    if (!sdl_audio_specs_match(&sdl->reopen_spec, &obtained)) {
        warn_report(
                "Requested SDL playback route has an incompatible format");
        SDL_CloseAudioDevice(devid);
        return false;
    }

    SDL_PauseAudioDevice(devid, 1);
    sdl->devid = devid;
    g_free(sdl->device_name);
    sdl->device_name = g_steal_pointer(&requested);
    sdl->route_matches = route_matches;
    sdl->next_route_check = 0;
    return true;
}

static void sdl_refresh_route_Out(SDLVoiceOut *sdl)
{
    g_autofree char *requested = NULL;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID replacement;
    bool present;
    bool route_matches;

    if (!sdl->devid) {
        return;
    }
    if (!sdl_route_check_due(&sdl->next_route_check)) {
        return;
    }
    requested = sdl_route_file_device_name(0, &present);
    if (!present || (sdl->route_matches &&
                     g_strcmp0(requested, sdl->device_name) == 0)) {
        return;
    }
    replacement = sdl_open(&sdl->reopen_spec, &obtained, 0, requested,
                           &route_matches);
    if (!replacement) {
        return;
    }
    if (!sdl_audio_specs_match(&sdl->reopen_spec, &obtained)) {
        warn_report(
                "Requested SDL playback route has an incompatible format");
        SDL_CloseAudioDevice(replacement);
        return;
    }

    SDL_PauseAudioDevice(replacement, 1);
    SDL_PauseAudioDevice(sdl->devid, 1);
    SDL_CloseAudioDevice(sdl->devid);
    sdl->devid = replacement;
    g_free(sdl->device_name);
    sdl->device_name = g_steal_pointer(&requested);
    sdl->route_matches = route_matches;
    SDL_PauseAudioDevice(sdl->devid, !sdl->enabled);
}

/* The recording counterpart of sdl_release_out(). */
static void sdl_release_in(SDLVoiceIn *sdl)
{
    if (!sdl->devid) {
        return;
    }
    SDL_PauseAudioDevice(sdl->devid, 1);
    SDL_CloseAudioDevice(sdl->devid);
    sdl->devid = 0;
}

static bool sdl_acquire_in(SDLVoiceIn *sdl)
{
    g_autofree char *requested = NULL;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID devid;
    bool present;
    bool route_matches;

    requested = sdl_route_file_device_name(1, &present);
    if (!present) {
        requested = g_strdup(sdl->device_name);
    }
    devid = sdl_open(&sdl->reopen_spec, &obtained, 1, requested,
                     &route_matches);
    if (!devid) {
        return false;
    }
    if (!sdl_audio_specs_match(&sdl->reopen_spec, &obtained)) {
        warn_report(
                "Requested SDL recording route has an incompatible format");
        SDL_CloseAudioDevice(devid);
        return false;
    }

    SDL_PauseAudioDevice(devid, 1);
    sdl->devid = devid;
    g_free(sdl->device_name);
    sdl->device_name = g_steal_pointer(&requested);
    sdl->route_matches = route_matches;
    sdl->next_route_check = 0;
    return true;
}

static void sdl_refresh_route_In(SDLVoiceIn *sdl)
{
    g_autofree char *requested = NULL;
    SDL_AudioSpec obtained;
    SDL_AudioDeviceID replacement;
    bool present;
    bool route_matches;

    if (!sdl->devid) {
        return;
    }
    if (!sdl_route_check_due(&sdl->next_route_check)) {
        return;
    }
    requested = sdl_route_file_device_name(1, &present);
    if (!present || (sdl->route_matches &&
                     g_strcmp0(requested, sdl->device_name) == 0)) {
        return;
    }
    replacement = sdl_open(&sdl->reopen_spec, &obtained, 1, requested,
                           &route_matches);
    if (!replacement) {
        return;
    }
    if (!sdl_audio_specs_match(&sdl->reopen_spec, &obtained)) {
        warn_report(
                "Requested SDL recording route has an incompatible format");
        SDL_CloseAudioDevice(replacement);
        return;
    }

    SDL_PauseAudioDevice(replacement, 1);
    SDL_PauseAudioDevice(sdl->devid, 1);
    SDL_CloseAudioDevice(sdl->devid);
    sdl->devid = replacement;
    g_free(sdl->device_name);
    sdl->device_name = g_steal_pointer(&requested);
    sdl->route_matches = route_matches;
    SDL_PauseAudioDevice(sdl->devid, !sdl->enabled);
}

#define SDL_WRAPPER_FUNC(name, ret_type, args_decl, args, dir) \
    static ret_type glue(sdl_, name)args_decl                  \
    {                                                          \
        ret_type ret;                                          \
        glue(SDLVoice, dir) *sdl = (glue(SDLVoice, dir) *)hw;  \
                                                               \
        glue(sdl_refresh_route_, glue(dir, ))(sdl);             \
        SDL_LockAudioDevice(sdl->devid);                       \
        ret = glue(audio_generic_, name)args;                  \
        SDL_UnlockAudioDevice(sdl->devid);                     \
                                                               \
        return ret;                                            \
    }

#define SDL_WRAPPER_VOID_FUNC(name, args_decl, args, dir)      \
    static void glue(sdl_, name)args_decl                      \
    {                                                          \
        glue(SDLVoice, dir) *sdl = (glue(SDLVoice, dir) *)hw;  \
                                                               \
        glue(sdl_refresh_route_, glue(dir, ))(sdl);             \
        SDL_LockAudioDevice(sdl->devid);                       \
        glue(audio_generic_, name)args;                        \
        SDL_UnlockAudioDevice(sdl->devid);                     \
    }

SDL_WRAPPER_FUNC(buffer_get_free, size_t, (HWVoiceOut *hw), (hw), Out)
SDL_WRAPPER_FUNC(get_buffer_out, void *, (HWVoiceOut *hw, size_t *size),
                 (hw, size), Out)
SDL_WRAPPER_FUNC(put_buffer_out, size_t,
                 (HWVoiceOut *hw, void *buf, size_t size), (hw, buf, size), Out)
SDL_WRAPPER_FUNC(write, size_t,
                 (HWVoiceOut *hw, void *buf, size_t size), (hw, buf, size), Out)
SDL_WRAPPER_FUNC(read, size_t, (HWVoiceIn *hw, void *buf, size_t size),
                 (hw, buf, size), In)
SDL_WRAPPER_FUNC(get_buffer_in, void *, (HWVoiceIn *hw, size_t *size),
                 (hw, size), In)
SDL_WRAPPER_VOID_FUNC(put_buffer_in, (HWVoiceIn *hw, void *buf, size_t size),
                      (hw, buf, size), In)
#undef SDL_WRAPPER_FUNC
#undef SDL_WRAPPER_VOID_FUNC

static void sdl_fini_out(HWVoiceOut *hw)
{
    SDLVoiceOut *sdl = (SDLVoiceOut *)hw;

    sdl_close_out(sdl);
    g_clear_pointer(&sdl->device_name, g_free);
    sdl->next_route_check = 0;
}

static int sdl_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    SDLVoiceOut *sdl = (SDLVoiceOut *)hw;
    SDL_AudioSpec req, obt;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevSdlPerDirectionOptions *spdo = dev->u.sdl.out;
    struct audsettings obt_as;

    req.freq = as->freq;
    req.format = aud_to_sdlfmt (as->fmt);
    req.channels = as->nchannels;
    /* SDL samples are QEMU frames */
    req.samples = audio_buffer_frames(
        qapi_AudiodevSdlPerDirectionOptions_base(spdo), as, 11610);
    req.callback = sdl_callback_out;
    req.userdata = sdl;

    sdl->dev = dev;
    sdl->device_name = sdl_initial_device_name(0);
    sdl->devid = sdl_open(&req, &obt, 0, sdl->device_name,
                          &sdl->route_matches);
    if (!sdl->devid) {
        return -1;
    }

    err = sdl_to_audfmt(obt.format, &obt_as.fmt, &obt_as.big_endian);
    if (err) {
        sdl_close_out(sdl);
        return -1;
    }

    obt_as.freq = obt.freq;
    obt_as.nchannels = obt.channels;

    audio_pcm_init_info (&hw->info, &obt_as);
    hw->samples = (spdo->has_buffer_count ? spdo->buffer_count : 4) *
        obt.samples;

    sdl->reopen_spec = obt;
    sdl->reopen_spec.callback = sdl_callback_out;
    sdl->reopen_spec.userdata = sdl;
    sdl->initialized = 1;
    sdl->exit = 0;
    sdl_release_out(sdl);
    return 0;
}

static void sdl_enable_out(HWVoiceOut *hw, bool enable)
{
    SDLVoiceOut *sdl = (SDLVoiceOut *)hw;

    if (sdl->enabled == enable) {
        return;
    }
    sdl->enabled = enable;
    if (!enable) {
        sdl_release_out(sdl);
        return;
    }
    if (!sdl->devid && !sdl_acquire_out(sdl)) {
        return;
    }
    SDL_PauseAudioDevice(sdl->devid, 0);
}

static void sdl_fini_in(HWVoiceIn *hw)
{
    SDLVoiceIn *sdl = (SDLVoiceIn *)hw;

    sdl_close_in(sdl);
    g_clear_pointer(&sdl->device_name, g_free);
    sdl->next_route_check = 0;
}

static int sdl_init_in(HWVoiceIn *hw, audsettings *as)
{
    SDLVoiceIn *sdl = (SDLVoiceIn *)hw;
    SDL_AudioSpec req, obt;
    int err;
    Audiodev *dev = hw->s->dev;
    AudiodevSdlPerDirectionOptions *spdo = dev->u.sdl.in;
    struct audsettings obt_as;

    req.freq = as->freq;
    req.format = aud_to_sdlfmt(as->fmt);
    req.channels = as->nchannels;
    /* SDL samples are QEMU frames */
    req.samples = audio_buffer_frames(
        qapi_AudiodevSdlPerDirectionOptions_base(spdo), as, 11610);
    req.callback = sdl_callback_in;
    req.userdata = sdl;

    sdl->dev = dev;
    sdl->device_name = sdl_initial_device_name(1);
    /*
     * Opening a recording device starts CoreAudio capture and trips macOS's
     * privacy indicator. SDL is called with allowed_changes=0, so a
     * successful open cannot change this requested spec; defer the real open
     * until the guest enables a recording stream.
     */
    obt = req;
    err = sdl_to_audfmt(obt.format, &obt_as.fmt, &obt_as.big_endian);
    if (err) {
        sdl_close_in(sdl);
        return -1;
    }

    obt_as.freq = obt.freq;
    obt_as.nchannels = obt.channels;

    audio_pcm_init_info(&hw->info, &obt_as);
    hw->samples = (spdo->has_buffer_count ? spdo->buffer_count : 4) *
        obt.samples;
    hw->size_emul = hw->samples * hw->info.bytes_per_frame;
    hw->buf_emul = g_malloc(hw->size_emul);
    hw->pos_emul = hw->pending_emul = 0;

    sdl->reopen_spec = obt;
    sdl->reopen_spec.callback = sdl_callback_in;
    sdl->reopen_spec.userdata = sdl;
    sdl->initialized = 1;
    sdl->exit = 0;
    return 0;
}

static void sdl_enable_in(HWVoiceIn *hw, bool enable)
{
    SDLVoiceIn *sdl = (SDLVoiceIn *)hw;

    if (sdl->enabled == enable) {
        return;
    }
    sdl->enabled = enable;
    if (!enable) {
        sdl_release_in(sdl);
        return;
    }
    if (!sdl->devid && !sdl_acquire_in(sdl)) {
        return;
    }
    SDL_PauseAudioDevice(sdl->devid, 0);
}

static bool audio_sdl_realize(AudioBackend *abe, Audiodev *dev, Error **errp)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error_setg(errp, "SDL failed to initialize audio subsystem");
        qapi_free_Audiodev(dev);
        return false;
    }

    return audio_sdl_parent_class->realize(abe, dev, errp);
}

static void audio_sdl_finalize(Object *obj)
{
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void audio_sdl_class_init(ObjectClass *klass, const void *data)
{
    AudioBackendClass *b = AUDIO_BACKEND_CLASS(klass);
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    audio_sdl_parent_class = AUDIO_BACKEND_CLASS(object_class_get_parent(klass));

    b->realize = audio_sdl_realize;
    k->max_voices_out = INT_MAX;
    k->max_voices_in = INT_MAX;
    k->voice_size_out = sizeof(SDLVoiceOut);
    k->voice_size_in = sizeof(SDLVoiceIn);

    k->init_out = sdl_init_out;
    k->fini_out = sdl_fini_out;
    /* wrapper for audio_generic_write */
    k->write = sdl_write;
    /* wrapper for audio_generic_buffer_get_free */
    k->buffer_get_free = sdl_buffer_get_free;
    /* wrapper for audio_generic_get_buffer_out */
    k->get_buffer_out = sdl_get_buffer_out;
    /* wrapper for audio_generic_put_buffer_out */
    k->put_buffer_out = sdl_put_buffer_out;
    k->enable_out = sdl_enable_out;

    k->init_in = sdl_init_in;
    k->fini_in = sdl_fini_in;
    /* wrapper for audio_generic_read */
    k->read = sdl_read;
    /* wrapper for audio_generic_get_buffer_in */
    k->get_buffer_in = sdl_get_buffer_in;
    /* wrapper for audio_generic_put_buffer_in */
    k->put_buffer_in = sdl_put_buffer_in;
    k->enable_in = sdl_enable_in;
}

static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_SDL,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioSdl),
        .class_init = audio_sdl_class_init,
        .instance_finalize = audio_sdl_finalize,
    },
};

DEFINE_TYPES(audio_types)
module_obj(TYPE_AUDIO_SDL);
