/*
    PulseAudio output plugin for DeaDBeeF Player
    Copyright (C) 2011 Jan D. Behrens <zykure@web.de>
    Copyright (C) 2010-2012 Alexey Yakovenko <waker@users.sourceforge.net>
    Copyright (C) 2010 Anton Novikov <tonn.post@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#  include "../config.h"
#endif

#include <pulse/pulseaudio.h>

#include <string.h>
#include <deadbeef/deadbeef.h>

#define trace(...) { fprintf(stdout, __VA_ARGS__); }
//#define trace(fmt,...)
#define FALSE (0)
#define TRUE (1)

DB_functions_t * deadbeef;
static DB_output_t plugin;

#define CONFSTR_PULSE_SERVERADDR "pulse.serveraddr"
#define CONFSTR_PULSE_BUFFERSIZE "pulse.buffersize"
#define PULSE_DEFAULT_BUFFERSIZE 4096

static pa_context *context = NULL;
static pa_stream *stream = NULL;
static pa_threaded_mainloop *mainloop = NULL;

static pa_cvolume volume;
static int volume_valid = FALSE;

static int connected = FALSE;

static pa_sample_spec ss;
static ddb_waveformat_t requested_fmt;
static int state;
static uintptr_t mutex;

static int buffer_size;

static int pulse_init();

static int pulse_free();

static int pulse_setformat(ddb_waveformat_t *fmt);

static int pulse_play();

static int pulse_stop();

static int pulse_pause();

static int pulse_unpause();




#define CHECK_DEAD_GOTO(label, warn) do { \
if (!mainloop || \
    !context || pa_context_get_state(context) != PA_CONTEXT_READY || \
    !stream || pa_stream_get_state(stream) != PA_STREAM_READY) { \
        if (warn) \
            trace("Connection died: %s\n", context ? pa_strerror(pa_context_errno(context)) : "null"); \
        goto label; \
    }  \
} while(0);

#define CHECK_CONNECTED(retval) \
do { \
    if (!connected) return retval; \
} while (0);

static void info_cb(struct pa_context *c, const struct pa_sink_input_info *i, int is_last, void *userdata) {
    assert(c);

    if (!i)
        return;

    volume = i->volume;
    volume_valid = TRUE;

    // Wrapping this in mutex lock doesn't work
    deadbeef->volume_set_db(pa_sw_volume_to_dB(pa_cvolume_avg(&volume)));
}

static void subscribe_cb(struct pa_context *c, enum pa_subscription_event_type t, uint32_t index, void *userdata) {
    pa_operation *o;

    assert(c);

    if (!stream ||
        index != pa_stream_get_index(stream) ||
        (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_CHANGE) &&
         t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW)))
        return;

    if (!(o = pa_context_get_sink_input_info(c, index, info_cb, NULL))) {
        trace("pa_context_get_sink_input_info() failed: %s\n", pa_strerror(pa_context_errno(c)));
        return;
    }

    pa_operation_unref(o);
}

static void context_state_cb(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_TERMINATED:
        case PA_CONTEXT_FAILED:
            trace("Context error: %s", pa_strerror(pa_context_errno(c)));
            pa_threaded_mainloop_signal(mainloop, 0);
            break;

        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;
    }
}

static void stream_state_cb(pa_stream *s, void * userdata) {
    assert(s);

    switch (pa_stream_get_state(s)) {

        case PA_STREAM_READY:
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            trace("Stream error: %s", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            pa_threaded_mainloop_signal(mainloop, 0);
            break;

        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;
    }
}

static void stream_success_cb(pa_stream *s, int success, void *userdata) {
    assert(s);

    if (userdata)
        *(int*) userdata = success;

    pa_threaded_mainloop_signal(mainloop, 0);
}

static void context_success_cb(pa_context *c, int success, void *userdata) {
    assert(c);

    if (userdata)
        *(int*) userdata = success;

    pa_threaded_mainloop_signal(mainloop, 0);
}

static void stream_request_cb(pa_stream *s, size_t length, void *userdata) {
    assert(s);

    //char *buffer;
    size_t sz = 4096;
    char buf[sz];

    if (state == OUTPUT_STATE_PLAYING && deadbeef->streamer_ok_to_read (-1)) {
        // pa_stream_begin_write(s, (void*)&buffer, &sz)
        int len = deadbeef->streamer_read (buf, sz);
        if (len) {

            if (pa_stream_write(s, buf, len, NULL, 0, PA_SEEK_RELATIVE) < 0)
            {
                trace ("pa_stream_write() failed: %s\n", pa_strerror (pa_context_errno (context)));
                goto fail;
            }
        } else {
            trace("pa_stream_begin_write() failed");
        }
    }

fail:
    pa_threaded_mainloop_signal(mainloop, 0);
}

static void stream_latency_update_cb(pa_stream *s, void *userdata) {
    assert(s);

    pa_threaded_mainloop_signal(mainloop, 0);
}



static int pulse_set_spec(ddb_waveformat_t *fmt)
{
    memcpy (&plugin.fmt, fmt, sizeof (ddb_waveformat_t));
    if (!plugin.fmt.channels) {
        // generic format
        plugin.fmt.bps = 16;
        plugin.fmt.is_float = 0;
        plugin.fmt.channels = 2;
        plugin.fmt.samplerate = 44100;
        plugin.fmt.channelmask = 3;
    }

    trace ("format %dbit %s %dch %dHz channelmask=%X\n", plugin.fmt.bps, plugin.fmt.is_float ? "float" : "int", plugin.fmt.channels, plugin.fmt.samplerate, plugin.fmt.channelmask);

    ss.channels = plugin.fmt.channels;
    // Try to auto-configure the channel map, see <pulse/channelmap.h> for details
    pa_channel_map channel_map;
    pa_channel_map_init_extend(&channel_map, ss.channels, PA_CHANNEL_MAP_WAVEEX);
    trace ("pulse: channels: %d\n", ss.channels);

    // Read samplerate from config
    //ss.rate = deadbeef->conf_get_int(CONFSTR_PULSE_SAMPLERATE, 44100);
    ss.rate = plugin.fmt.samplerate;
    trace ("pulse: samplerate: %d\n", ss.rate);

    switch (plugin.fmt.bps) {
    case 8:
        ss.format = PA_SAMPLE_U8;
        break;
    case 16:
        ss.format = PA_SAMPLE_S16LE;
        break;
    case 24:
        ss.format = PA_SAMPLE_S24LE;
        break;
    case 32:
        if (plugin.fmt.is_float) {
            ss.format = PA_SAMPLE_FLOAT32LE;
        }
        else {
            ss.format = PA_SAMPLE_S32LE;
        }
        break;
    default:
        return -1;
    };



    if (!pa_sample_spec_valid(&ss)) {
        trace ("Sample spec invalid\n");
        return FALSE;
    }

    if (!(mainloop = pa_threaded_mainloop_new())) {
        trace ("Failed to allocate main loop\n");
        return FALSE;
    }

    pa_threaded_mainloop_lock(mainloop);

    if (!(context = pa_context_new(pa_threaded_mainloop_get_api(mainloop), "DeaDBeeF"))) {
        trace ("Failed to allocate context\n");
        goto FAIL1;
    }

    pa_context_set_state_callback(context, context_state_cb, NULL);
    pa_context_set_subscribe_callback(context, subscribe_cb, NULL);

    if (pa_context_connect(context, NULL, (pa_context_flags_t) 0, NULL) < 0) {
        trace ("Failed to connect to server: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL1;
    }

    if (pa_threaded_mainloop_start(mainloop) < 0) {
        trace ("Failed to start main loop\n");
        goto FAIL1;
    }

    /* Wait until the context is ready */
    pa_threaded_mainloop_wait(mainloop);

    if (pa_context_get_state(context) != PA_CONTEXT_READY) {
        trace ("Failed to connect to server: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL1;
    }

    if (!(stream = pa_stream_new(context, "Music", &ss, &channel_map))) {
        trace ("Failed to create stream: %s\n", pa_strerror(pa_context_errno(context)));

FAIL1:
        pa_threaded_mainloop_unlock (mainloop);
        pulse_free ();
        return FALSE;
    }

    pa_stream_set_state_callback(stream, stream_state_cb, NULL);
    pa_stream_set_write_callback(stream, stream_request_cb, NULL);
    pa_stream_set_latency_update_callback(stream, stream_latency_update_cb, NULL);

    /* Connect stream with sink and default volume */
    /* Buffer struct */

    int aud_buffer = 4096;
    size_t buffer_size = pa_usec_to_bytes(aud_buffer, &ss) * 1000;
    pa_buffer_attr buffer = {(uint32_t) -1, (uint32_t) buffer_size,
     (uint32_t) -1, (uint32_t) -1, (uint32_t) buffer_size};

    pa_operation *o = NULL;
    int success;

    if (pa_stream_connect_playback (stream, NULL, & buffer, (pa_stream_flags_t)
     (PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE), NULL, NULL) < 0)
    {
        trace ("Failed to connect stream: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }


    /* Wait until the stream is ready */
    pa_threaded_mainloop_wait(mainloop);

    if (pa_stream_get_state(stream) != PA_STREAM_READY) {
        trace ("Failed to connect stream: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }

    /* Now subscribe to events */
    if (!(o = pa_context_subscribe(context, PA_SUBSCRIPTION_MASK_SINK_INPUT, context_success_cb, &success))) {
        trace ("pa_context_subscribe() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }

    success = 0;
    while (pa_operation_get_state(o) != PA_OPERATION_DONE) {
        CHECK_DEAD_GOTO(FAIL2, 1);
        pa_threaded_mainloop_wait(mainloop);
    }

    if (!success) {
        trace ("pa_context_subscribe() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }

    pa_operation_unref(o);

    /* Now request the initial stream info */
    if (!(o = pa_context_get_sink_input_info(context, pa_stream_get_index(stream), info_cb, NULL))) {
        trace ("pa_context_get_sink_input_info() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }

    while (pa_operation_get_state(o) != PA_OPERATION_DONE) {
        CHECK_DEAD_GOTO(FAIL2, 1);
        pa_threaded_mainloop_wait(mainloop);
    }

    if (!volume_valid) {
        trace ("pa_context_get_sink_input_info() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto FAIL2;
    }
    pa_operation_unref(o);

    connected = TRUE;

    pa_threaded_mainloop_unlock(mainloop);

    return TRUE;

FAIL2:
    if (o)
        pa_operation_unref(o);

    pa_threaded_mainloop_unlock(mainloop);
    pulse_free ();
    return FALSE;
}

static int pulse_init(void)
{
    trace ("pulse_init\n");
    deadbeef->mutex_lock(mutex);

    if (requested_fmt.samplerate != 0) {
        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    }

    if (0 != pulse_set_spec(&plugin.fmt)) {
        deadbeef->mutex_unlock(mutex);
        return -1;
    }

    deadbeef->mutex_unlock(mutex);

    return 0;
}

static int pulse_setformat (ddb_waveformat_t *fmt)
{
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    if (!connected) {
        return -1;
    }
    if (!memcmp (fmt, &plugin.fmt, sizeof (ddb_waveformat_t))) {
        trace ("pulse_setformat ignored\n");
        return 0;
    }
    trace ("pulse_setformat %dbit %s %dch %dHz channelmask=%X\n", fmt->bps, fmt->is_float ? "float" : "int", fmt->channels, fmt->samplerate, fmt->channelmask);

    switch (state) {
    case OUTPUT_STATE_STOPPED:
        return 0;
    case OUTPUT_STATE_PLAYING:
        pulse_stop ();
        return pulse_play ();
    case OUTPUT_STATE_PAUSED:
        if (0 != pulse_play ()) {
            return -1;
        }
        if (0 != pulse_pause ()) {
            return -1;
        }
        break;
    }
    return 0;
}

static int pulse_free(void)
{
    trace("pulse_free\n");

    deadbeef->mutex_lock(mutex);

    connected = FALSE;

    if (mainloop)
        pa_threaded_mainloop_stop(mainloop);

    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = NULL;
    }

    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
        context = NULL;
    }

    if (mainloop) {
        pa_threaded_mainloop_free(mainloop);
        mainloop = NULL;
    }

    volume_valid = FALSE;

    state = OUTPUT_STATE_STOPPED;
    deadbeef->mutex_unlock(mutex);

    return 0;
}

static int pulse_play(void)
{
    enum output_state_t prev_state = state;
    if (state != OUTPUT_STATE_PLAYING)
    {
        state = OUTPUT_STATE_PLAYING;
        if (connected) return 0;
        if (pulse_init () < 0)
        {
            state = prev_state;
            return -1;
        }
    }

    return 0;
}

static int pulse_stop(void)
{
    if (state != OUTPUT_STATE_STOPPED)
    {
        state = OUTPUT_STATE_STOPPED;
        deadbeef->streamer_reset(1);
        pulse_free();
    }

    return 0;
}

void pulse_pause_internal(int pause)
{
    pa_operation *o = NULL;
    int success = 0;

    CHECK_CONNECTED();

    pa_threaded_mainloop_lock(mainloop);
    CHECK_DEAD_GOTO(fail, 1);

    if (!(o = pa_stream_cork(stream, pause, stream_success_cb, &success))) {
        trace("pa_stream_cork() failed: %s\n", pa_strerror(pa_context_errno(context)));
        goto fail;
    }

    while (pa_operation_get_state(o) != PA_OPERATION_DONE) {
        CHECK_DEAD_GOTO(fail, 1);
        pa_threaded_mainloop_wait(mainloop);
    }

    if (!success)
        trace("pa_stream_cork() failed: %s\n", pa_strerror(pa_context_errno(context)));

fail:

    if (o)
        pa_operation_unref(o);

    pa_threaded_mainloop_unlock(mainloop);
}

static int pulse_pause(void)
{
    if (state == OUTPUT_STATE_STOPPED)
    {
        return -1;
    }

    state = OUTPUT_STATE_PAUSED;
    pulse_pause_internal(TRUE);
    return 0;
}

static int pulse_unpause(void)
{
    if (state == OUTPUT_STATE_PAUSED)
    {
        state = OUTPUT_STATE_PLAYING;
        pulse_pause_internal(FALSE);
    }

    return 0;
}


static int pulse_get_state(void)
{
    return state;
}

static int pulse_plugin_start(void)
{
    state = OUTPUT_STATE_STOPPED;

    mutex = deadbeef->mutex_create();

    return 0;
}

static int pulse_plugin_stop(void)
{
    deadbeef->mutex_free(mutex);
    return 0;
}

DB_plugin_t * pulse_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

void set_volume ()
{
    pa_operation * o;

    if (! connected)
        return;

    pa_threaded_mainloop_lock (mainloop);
    CHECK_DEAD_GOTO (fail, 1);


    volume.values[0] = pa_sw_volume_from_dB(deadbeef->volume_get_db());
    volume.values[1] = pa_sw_volume_from_dB(deadbeef->volume_get_db());
    volume.channels = 2;

    volume_valid = TRUE;

    if (! (o = pa_context_set_sink_input_volume (context, pa_stream_get_index
     (stream), & volume, NULL, NULL))) {
        trace ("pa_context_set_sink_input_volume() failed: %s\n", pa_strerror
         (pa_context_errno (context)));
    } else
        pa_operation_unref(o);

fail:
    pa_threaded_mainloop_unlock (mainloop);
}

static int
pulse_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_VOLUMECHANGED:
        set_volume();
        break;
    }
    return 0;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char settings_dlg[] =
    "property \"PulseAudio server\" entry " CONFSTR_PULSE_SERVERADDR " default;\n"
    "property \"Preferred buffer size\" entry " CONFSTR_PULSE_BUFFERSIZE " " STR(PULSE_DEFAULT_BUFFERSIZE) ";\n";

static DB_output_t plugin =
{
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = "pulseaudio2",
    .plugin.name = "PulseAudio output plugin version 2",
    .plugin.descr = "This is a new pulseaudio plugin that uses the asynchronous API",
    .plugin.copyright =
        "PulseAudio output plugin for DeaDBeeF Player\n"
        "Copyright (C) 2015 Nicolai Syvertsen <saivert@saivert.com>\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n",
    .plugin.website = "http://saivert.com",
    .plugin.start = pulse_plugin_start,
    .plugin.stop = pulse_plugin_stop,
    .plugin.configdialog = settings_dlg,
    .plugin.message = pulse_message,
    .init = pulse_init,
    .free = pulse_free,
    .setformat = pulse_setformat,
    .play = pulse_play,
    .stop = pulse_stop,
    .pause = pulse_pause,
    .unpause = pulse_unpause,
    .state = pulse_get_state,
    .has_volume = 1,
};
