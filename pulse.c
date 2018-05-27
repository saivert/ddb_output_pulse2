/*
    PulseAudio output plugin for DeaDBeeF Player
    Copyright (C) 2015 Nicolai Syvertsen <saivert@saivert.com>

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

#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <deadbeef/deadbeef.h>

#define trace(...) { fprintf(stdout, __VA_ARGS__); }
//#define trace(fmt,...)

#define CMUS_STR(a) #a

#define BUG_ON(a)			\
do {					\
    if (unlikely(a))		\
        trace("%s\n", CMUS_STR(a));	\
} while (0)

#define OP_ERROR_SUCCESS 0
#define OP_ERROR_INTERNAL 1



/* Optimization: Condition @x is unlikely */
#define unlikely(x) __builtin_expect(!!(x), 0)

DB_functions_t * deadbeef;
static DB_output_t plugin;

#define CONFSTR_PULSE_SERVERADDR "pulse.serveraddr"
#define CONFSTR_PULSE_BUFFERSIZE "pulse.buffersize"
#define CONFSTR_PULSE_VOLUMECONTROL "pulse.volumecontrol"
#define PULSE_DEFAULT_VOLUMECONTROL 0
#define PULSE_DEFAULT_BUFFERSIZE 4096



static ddb_waveformat_t requested_fmt;
static int state=OUTPUT_STATE_STOPPED;
static uintptr_t mutex;
static int buffer_size;

static int pulse_init();

static int pulse_free();

static int pulse_setformat(ddb_waveformat_t *fmt);

static int pulse_play();

static int pulse_stop();

static int pulse_pause();

static int pulse_unpause();

static int pulse_set_spec(ddb_waveformat_t *fmt);


static pa_threaded_mainloop	*pa_ml;
static pa_context		*pa_ctx;
static pa_stream		*pa_s;
static pa_channel_map		 pa_cmap;
static pa_cvolume		 pa_vol;
static pa_sample_spec		 pa_ss;


#define ret_pa_error(err)						\
    do {								\
        trace("PulseAudio error: %s\n", pa_strerror(err));	\
        return -OP_ERROR_INTERNAL;				\
    } while (0)

#define ret_pa_last_error() ret_pa_error(pa_context_errno(pa_ctx))

static int _pa_nowait_unlock(pa_operation *o);

static pa_proplist *_create_app_proplist(void)
{
    pa_proplist	*pl;
    int		 rc;

    pl = pa_proplist_new();
    BUG_ON(!pl);

    rc = pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "DeaDBeeF Music Player");
    BUG_ON(rc);

    rc = pa_proplist_sets(pl, PA_PROP_APPLICATION_ID, "music.deadbeef.player");
    BUG_ON(rc);


    return pl;
}

static pa_proplist *_create_stream_proplist(void)
{
    pa_proplist	*pl;
    int		 rc;

    pl = pa_proplist_new();
    BUG_ON(!pl);

    rc = pa_proplist_sets(pl, PA_PROP_MEDIA_ROLE, "music");
    BUG_ON(rc);

    rc = pa_proplist_sets(pl, PA_PROP_MEDIA_ICON_NAME, "deadbeef");
    BUG_ON(rc);

    return pl;
}

static const char *_pa_context_state_str(pa_context_state_t s)
{
    switch (s) {
    case PA_CONTEXT_AUTHORIZING:
        return "PA_CONTEXT_AUTHORIZING";
    case PA_CONTEXT_CONNECTING:
        return "PA_CONTEXT_CONNECTING";
    case PA_CONTEXT_FAILED:
        return "PA_CONTEXT_FAILED";
    case PA_CONTEXT_READY:
        return "PA_CONTEXT_READY";
    case PA_CONTEXT_SETTING_NAME:
        return "PA_CONTEXT_SETTING_NAME";
    case PA_CONTEXT_TERMINATED:
        return "PA_CONTEXT_TERMINATED";
    case PA_CONTEXT_UNCONNECTED:
        return "PA_CONTEXT_UNCONNECTED";
    }

    return "unknown";
}

static void _pa_context_running_cb(pa_context *c, void *data)
{
    const pa_context_state_t cs = pa_context_get_state(c);

    trace("pulse: context state has changed to %s\n", _pa_context_state_str(cs));

    switch (cs) {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(pa_ml, 0);
    default:
        return;
    }
}

static const char *_pa_stream_state_str(pa_stream_state_t s)
{
    switch (s) {
    case PA_STREAM_CREATING:
        return "PA_STREAM_CREATING";
    case PA_STREAM_FAILED:
        return "PA_STREAM_FAILED";
    case PA_STREAM_READY:
        return "PA_STREAM_READY";
    case PA_STREAM_TERMINATED:
        return "PA_STREAM_TERMINATED";
    case PA_STREAM_UNCONNECTED:
        return "PA_STREAM_UNCONNECTED";
    }

    return "unknown";
}

static void _pa_stream_running_cb(pa_stream *s, void *data)
{
    const pa_stream_state_t ss = pa_stream_get_state(s);

    trace("pulse: stream state has changed to %s\n", _pa_stream_state_str(ss));

    switch (ss) {
    case PA_STREAM_READY:
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        pa_threaded_mainloop_signal(pa_ml, 0);
    default:
        return;
    }
}

static void _pa_sink_input_info_cb(pa_context *c,
                   const pa_sink_input_info *i,
                   int eol,
                   void *data)
{
    if (i && plugin.has_volume) {
        memcpy(&pa_vol, &i->volume, sizeof(pa_vol));
        deadbeef->volume_set_amp(pa_sw_volume_to_linear(pa_cvolume_avg(&pa_vol)));
    }
}

static int set_volume()
{
    if (!pa_s || !plugin.has_volume) {
        return -1;
    }

    pa_cvolume_set(&pa_vol, pa_ss.channels, pa_sw_volume_from_linear(deadbeef->volume_get_amp()));


    if (!pa_s) {
        return OP_ERROR_SUCCESS;
    } else {
        pa_threaded_mainloop_lock(pa_ml);

        return _pa_nowait_unlock(pa_context_set_sink_input_volume(pa_ctx,
                                          pa_stream_get_index(pa_s),
                                          &pa_vol,
                                          NULL,
                                          NULL));
    }
}

static void _pa_stream_success_cb(pa_stream *s, int success, void *data)
{
    pa_threaded_mainloop_signal(pa_ml, 0);
}

static int _pa_wait_unlock(pa_operation *o)
{
    pa_operation_state_t state;

    if (!o) {
        pa_threaded_mainloop_unlock(pa_ml);
        ret_pa_last_error();
    }

    while ((state = pa_operation_get_state(o)) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(pa_ml);

    pa_operation_unref(o);
    pa_threaded_mainloop_unlock(pa_ml);

    if (state == PA_OPERATION_DONE)
        return OP_ERROR_SUCCESS;
    else
        ret_pa_last_error();
}

static int _pa_nowait_unlock(pa_operation *o)
{
    if (!o) {
        pa_threaded_mainloop_unlock(pa_ml);
        ret_pa_last_error();
    }

    pa_operation_unref(o);
    pa_threaded_mainloop_unlock(pa_ml);

    return OP_ERROR_SUCCESS;
}

static int _pa_stream_cork(int pause_)
{
    pa_threaded_mainloop_lock(pa_ml);

    return _pa_wait_unlock(pa_stream_cork(pa_s, pause_, _pa_stream_success_cb, NULL));
}

static int _pa_stream_drain(void)
{
    pa_threaded_mainloop_lock(pa_ml);

    return _pa_wait_unlock(pa_stream_drain(pa_s, _pa_stream_success_cb, NULL));
}

static void _pa_ctx_subscription_cb(pa_context *ctx, pa_subscription_event_type_t t,
        uint32_t idx, void *userdata)
{
    pa_subscription_event_type_t type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (type != PA_SUBSCRIPTION_EVENT_CHANGE)
        return;

    if (pa_s && idx == pa_stream_get_index(pa_s))
        pa_context_get_sink_input_info(ctx, idx, _pa_sink_input_info_cb, NULL);
}

static int _pa_create_context(void)
{
    pa_mainloop_api	*api;
    pa_proplist	*pl;
    int		 rc;

    pl = _create_app_proplist();

    api = pa_threaded_mainloop_get_api(pa_ml);
    BUG_ON(!api);

    pa_threaded_mainloop_lock(pa_ml);

    pa_ctx = pa_context_new_with_proplist(api, "DeaDBeeF Music Player", pl);
    BUG_ON(!pa_ctx);
    pa_proplist_free(pl);

    pa_context_set_state_callback(pa_ctx, _pa_context_running_cb, NULL);

    // Read serveraddr from config
    char server[1000];
    deadbeef->conf_get_str (CONFSTR_PULSE_SERVERADDR, "", server, sizeof (server));

    rc = pa_context_connect(pa_ctx, *server ? server : NULL, PA_CONTEXT_NOFLAGS, NULL);
    if (rc)
        goto out_fail;

    for (;;) {
        pa_context_state_t state;
        state = pa_context_get_state(pa_ctx);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state))
            goto out_fail_connected;
        pa_threaded_mainloop_wait(pa_ml);
    }

    pa_context_set_subscribe_callback(pa_ctx, _pa_ctx_subscription_cb, NULL);
    pa_operation *op = pa_context_subscribe(pa_ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT,
            NULL, NULL);
    if (!op)
        goto out_fail_connected;
    pa_operation_unref(op);

    pa_threaded_mainloop_unlock(pa_ml);

    return OP_ERROR_SUCCESS;

out_fail_connected:
    pa_context_disconnect(pa_ctx);

out_fail:
    pa_context_unref(pa_ctx);
    pa_ctx = NULL;

    pa_threaded_mainloop_unlock(pa_ml);

    ret_pa_last_error();
}

static void stream_request_cb(pa_stream *s, size_t requested_bytes, void *userdata) {
    uint8_t *buffer = NULL;
    size_t bufsize = requested_bytes;
    int bytesread;

    pa_stream_begin_write(s, (void**) &buffer, &bufsize);

    if (state != OUTPUT_STATE_PLAYING || !deadbeef->streamer_ok_to_read (-1)) {
        memset (buffer, 0, bufsize);
        bytesread = bufsize;
    } else {
        bytesread = deadbeef->streamer_read(buffer, bufsize);
        if (bytesread < 0) {
            bytesread = 0;
        }
    }
    pa_stream_write(s, buffer, bytesread, NULL, 0LL, PA_SEEK_RELATIVE);
}


static int pulse_init(void)
{
    trace ("pulse_init\n");
    int rc;

    state = OUTPUT_STATE_STOPPED;

    if (requested_fmt.samplerate != 0) {
        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
    }

    pa_ml = pa_threaded_mainloop_new();
    BUG_ON(!pa_ml);

    rc = pa_threaded_mainloop_start(pa_ml);
    if (rc) {
        pa_threaded_mainloop_free(pa_ml);
        ret_pa_error(rc);
    }

    return OP_ERROR_SUCCESS;
}

static int pulse_setformat (ddb_waveformat_t *fmt)
{
    int st = state;
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    if (!pa_s
        || !memcmp (fmt, &plugin.fmt, sizeof (ddb_waveformat_t))) {
        return 0;
    }

    pulse_stop();
    pulse_free ();
    pulse_init ();
    int res = 0;
    if (st == OUTPUT_STATE_PLAYING) {
        res = pulse_play ();
    }
    else if (st == OUTPUT_STATE_PAUSED) {
        res = pulse_pause ();
    }

    return 0;
}

static int pulse_free(void)
{
    trace("pulse_free\n");

    if (pa_s) {
        pulse_stop();
    }

    if (pa_ml) {
        pa_threaded_mainloop_stop(pa_ml);
        pa_threaded_mainloop_free(pa_ml);
        pa_ml = NULL;
    }

    return OP_ERROR_SUCCESS;
}

static int pulse_set_spec(ddb_waveformat_t *fmt)
{
    pa_proplist	*pl;
    int		 rc, i;

    deadbeef->mutex_lock(mutex);


    memcpy (&plugin.fmt, fmt, sizeof (ddb_waveformat_t));
    if (!plugin.fmt.channels) {
        // generic format
        plugin.fmt.bps = 16;
        plugin.fmt.is_float = 0;
        plugin.fmt.channels = 2;
        plugin.fmt.samplerate = 44100;
        plugin.fmt.channelmask = 3;
    }
    if (plugin.fmt.samplerate > 192000) {
        plugin.fmt.samplerate = 192000;
    }

    trace ("format %dbit %s %dch %dHz channelmask=%X\n", plugin.fmt.bps, plugin.fmt.is_float ? "float" : "int", plugin.fmt.channels, plugin.fmt.samplerate, plugin.fmt.channelmask);

    pa_ss.channels = plugin.fmt.channels;
    // Try to auto-configure the channel map, see <pulse/channelmap.h> for details

    pa_channel_map_init_extend(&pa_cmap, pa_ss.channels, PA_CHANNEL_MAP_WAVEEX);
    trace ("pulse: channels: %d\n", pa_ss.channels);

    // Read samplerate from config
    pa_ss.rate = plugin.fmt.samplerate;
    trace ("pulse: samplerate: %d\n", pa_ss.rate);

    switch (plugin.fmt.bps) {
    case 8:
        pa_ss.format = PA_SAMPLE_U8;
        break;
    case 16:
        pa_ss.format = PA_SAMPLE_S16LE;
        break;
    case 24:
        pa_ss.format = PA_SAMPLE_S24LE;
        break;
    case 32:
        if (plugin.fmt.is_float) {
            pa_ss.format = PA_SAMPLE_FLOAT32LE;
        }
        else {
            pa_ss.format = PA_SAMPLE_S32LE;
        }
        break;
    default:
        return -1;
    };

    trace("Pulseaudio: create context\n");
    rc = _pa_create_context();
    if (rc)
        return rc;

    pl = _create_stream_proplist();

    pa_threaded_mainloop_lock(pa_ml);

    trace("Pulseaudio: create stream\n");
    pa_s = pa_stream_new_with_proplist(pa_ctx, "playback", &pa_ss, &pa_cmap, pl);
    pa_proplist_free(pl);
    if (!pa_s) {
        pa_threaded_mainloop_unlock(pa_ml);
        ret_pa_last_error();
    }

    pa_stream_set_state_callback(pa_s, _pa_stream_running_cb, NULL);
    pa_stream_set_write_callback(pa_s, stream_request_cb, NULL);

    buffer_size = deadbeef->conf_get_int(CONFSTR_PULSE_BUFFERSIZE, PULSE_DEFAULT_BUFFERSIZE);

    pa_buffer_attr attr = {
        .maxlength = -1,
        .tlength = buffer_size,
        .prebuf = -1,
        .minreq = -1,
    };

    rc = pa_stream_connect_playback(pa_s,
                    NULL,
                    &attr,
                    PA_STREAM_NOFLAGS,
                    NULL,
                    NULL);
    if (rc)
        goto out_fail;

    pa_threaded_mainloop_wait(pa_ml);

    if (pa_stream_get_state(pa_s) != PA_STREAM_READY)
        goto out_fail;

    pa_context_get_sink_input_info(pa_ctx, pa_stream_get_index(pa_s),
            _pa_sink_input_info_cb, NULL);

    pa_threaded_mainloop_unlock(pa_ml);

    deadbeef->mutex_unlock(mutex);


    state = OUTPUT_STATE_PLAYING;
    return OP_ERROR_SUCCESS;

out_fail:
    pa_stream_unref(pa_s);

    pa_threaded_mainloop_unlock(pa_ml);

    deadbeef->mutex_unlock(mutex);


    ret_pa_last_error();
}

static int pulse_play(void)
{
    trace ("pulse_play\n");

    if (!pa_ml) {
        pulse_init();
    }

    deadbeef->mutex_lock (mutex);

    int ret = pulse_set_spec(&plugin.fmt);
    deadbeef->mutex_unlock (mutex);
    return ret;
}

static int pulse_stop(void)
{
    if (state == OUTPUT_STATE_STOPPED) {
       return 0;
    }


    pa_threaded_mainloop_lock(pa_ml);

    if (pa_s) {
        pa_stream_disconnect(pa_s);
        pa_stream_unref(pa_s);
        pa_s = NULL;
    }

    if (pa_ctx) {
        pa_context_disconnect(pa_ctx);
        pa_context_unref(pa_ctx);
        pa_ctx = NULL;
    }

    pa_threaded_mainloop_unlock(pa_ml);

    state = OUTPUT_STATE_STOPPED;

    return OP_ERROR_SUCCESS;
}

static int pulse_pause(void)
{
    state = OUTPUT_STATE_PAUSED;
      return _pa_stream_cork(1);
}

static int pulse_unpause(void)
{
    state = OUTPUT_STATE_PLAYING;
    return _pa_stream_cork(0);
}


static int pulse_get_state(void)
{
    return state;
}



static int pulse_plugin_start(void)
{
    mutex = deadbeef->mutex_create();
    return pulse_init();
}

static int pulse_plugin_stop(void)
{
    int ret = pulse_free();
    deadbeef->mutex_free(mutex);
    return ret;
}

DB_plugin_t * pulse2_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}


static int
pulse_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_VOLUMECHANGED:
        {
            set_volume();
        }
        break;
    case DB_EV_CONFIGCHANGED:
        plugin.has_volume = deadbeef->conf_get_int(CONFSTR_PULSE_VOLUMECONTROL, PULSE_DEFAULT_VOLUMECONTROL);
        break;
    }
    return 0;
}

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char settings_dlg[] =
    "property \"PulseAudio server (leave empty for default)\" entry " CONFSTR_PULSE_SERVERADDR " \"\";\n"
    "property \"Preferred buffer size\" entry " CONFSTR_PULSE_BUFFERSIZE " " STR(PULSE_DEFAULT_BUFFERSIZE) ";\n"
    "property \"Use pulseaudio volume control\" checkbox " CONFSTR_PULSE_VOLUMECONTROL " " STR(PULSE_DEFAULT_VOLUMECONTROL) ";\n";

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
    .has_volume = PULSE_DEFAULT_VOLUMECONTROL,
};
