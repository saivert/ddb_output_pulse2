/*
    PulseAudio output plugin for DeaDBeeF Player
    Copyright (C) 2015-2020 Nicolai Syvertsen <saivert@saivert.com>

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



#include <pulse/pulseaudio.h>

#include <errno.h>
#include <string.h>
#include <stdbool.h>
#define DDB_API_LEVEL 10
#include <deadbeef/deadbeef.h>

#ifdef DBPULSE_DEBUG
#define trace(...) { fprintf(stdout, __VA_ARGS__); }
#else
#define trace(fmt,...)
#endif

#define log_err(...) { deadbeef->log_detailed (&plugin.plugin, DDB_LOG_LAYER_DEFAULT, __VA_ARGS__); }

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define BUG_ON(a)			\
do {					\
    if (unlikely(a))		\
        trace("%s\n", STR_HELPER(a));	\
} while (0)

#define OP_ERROR_SUCCESS 0
#define OP_ERROR_INTERNAL 1



/* Optimization: Condition @x is unlikely */
#define unlikely(x) __builtin_expect(!!(x), 0)

DB_functions_t * deadbeef;
static DB_output_t plugin;

#define PULSE_PLUGIN_ID "pulseaudio2"
#define CONFSTR_PULSE_SERVERADDR "pulse.serveraddr"
#define CONFSTR_PULSE_BUFFERSIZE "pulse.buffersize"
#define CONFSTR_PULSE_VOLUMECONTROL "pulse.volumecontrol"
#define CONFSTR_PULSE_PAUSEONCORK "pulse.pauseoncork"
#define PULSE_DEFAULT_VOLUMECONTROL 0
#define PULSE_DEFAULT_BUFFERSIZE 100
#define PULSE_DEFAULT_PAUSEONCORK 0



static ddb_waveformat_t requested_fmt;
static int state=OUTPUT_STATE_STOPPED;
static uintptr_t mutex;
static int buffer_size;
static int cork_requested;
static char *tfbytecode;
static int _setformat_requested;


static void stream_request_cb(pa_stream *s, size_t requested_bytes, void *userdata);
static int _setformat_state;
static void _setformat_apply ();
static void _setformat_apply_once (pa_mainloop_api *m, void *userdata);


static int pulse_init();

static int pulse_free();

static int pulse_setformat(ddb_waveformat_t *fmt);

static int pulse_play();

static int pulse_stop();

static int pulse_pause();

static int pulse_unpause();



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

static pa_proplist* get_stream_prop_song(DB_playItem_t *track)
{
    pa_proplist	*pl;
    int rc, notrackgiven=0;
    ddb_tf_context_t ctx = {
        ._size = sizeof(ddb_tf_context_t),
        .flags = DDB_TF_CONTEXT_NO_DYNAMIC,
        .plt = NULL,
        .iter = PL_MAIN};

    pl = pa_proplist_new();
    BUG_ON(!pl);

    if (!track) {
        track = deadbeef->streamer_get_playing_track();
        notrackgiven = 1;
    }
    if (track) {

        char buf[1000];
        const char *artist, *title;

        ctx.it = track;
        if (deadbeef->tf_eval(&ctx, tfbytecode, buf, sizeof(buf)) > 0) {
            rc = pa_proplist_sets(pl, PA_PROP_MEDIA_NAME, buf);
            BUG_ON(rc);
        }

        deadbeef->pl_lock();
        artist = deadbeef->pl_find_meta(track, "artist");
        title = deadbeef->pl_find_meta(track, "title");

        if (artist) {
            rc = pa_proplist_sets(pl, PA_PROP_MEDIA_ARTIST, artist);
            BUG_ON(rc);
        }

        if (title) {
            rc = pa_proplist_sets(pl, PA_PROP_MEDIA_TITLE, title);
            BUG_ON(rc);
        }

        rc = pa_proplist_sets(pl, PA_PROP_MEDIA_FILENAME, deadbeef->pl_find_meta(track, ":URI"));
        BUG_ON(rc);

        deadbeef->pl_unlock();
        if (notrackgiven) deadbeef->pl_item_unref(track);
    } else {
        rc = pa_proplist_sets(pl, PA_PROP_MEDIA_NAME, "");
        BUG_ON(rc);
    }
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

#ifdef DBPULSE_DEBUG
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
#endif

static void _pa_context_running_cb(pa_context *c, void *data)
{
    const pa_context_state_t cs = pa_context_get_state(c);

    trace("pulse: context state has changed to %s\n", _pa_context_state_str(cs));

    switch (cs) {
    case PA_CONTEXT_READY:
        _setformat_state = 1;
        _setformat_apply();
        //pa_mainloop_api_once(pa_threaded_mainloop_get_api(pa_ml), _setformat_apply_once, NULL );
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(pa_ml, 0);
    default:
        return;
    }
}

#ifdef DBPULSE_DEBUG
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
#endif

static void _pa_stream_running_cb(pa_stream *s, void *data)
{
    const pa_stream_state_t ss = pa_stream_get_state(s);

    trace("pulse: stream state has changed to %s\n", _pa_stream_state_str(ss));

    switch (ss) {
    case PA_STREAM_FAILED:
        log_err("Pulseaudio: Stopping playback. Reason: %s", pa_strerror(pa_context_errno(pa_ctx)));
        deadbeef->sendmessage(DB_EV_STOP, 0, 0, 0);
        break;
    case PA_STREAM_READY:
        if (_setformat_requested /* && _setformat_state == 1 */) {
            _setformat_state = 2;
            //pa_mainloop_api_once(pa_threaded_mainloop_get_api(pa_ml), _setformat_apply_once, NULL );
            _setformat_apply();
        }
        if (state == OUTPUT_STATE_STOPPED) {
            state = OUTPUT_STATE_PLAYING;
        }
        break;
    case PA_STREAM_TERMINATED:
        if (_setformat_requested) {
            //pa_mainloop_api_once(pa_threaded_mainloop_get_api(pa_ml), _setformat_apply_once, NULL );
            _setformat_apply();
        }
        //pa_threaded_mainloop_signal(pa_ml, 0);
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
        if (pa_cvolume_equal(&pa_vol, &i->volume)) return;
        memcpy(&pa_vol, &i->volume, sizeof(pa_vol));
        pa_volume_t v = pa_cvolume_avg(&pa_vol);
        if (v <= PA_VOLUME_NORM) {
            deadbeef->volume_set_amp(pa_sw_volume_to_linear(v));
        }
    }
}

static void set_volume_value(void)
{
    pa_cvolume_set(&pa_vol, pa_ss.channels, pa_sw_volume_from_linear(deadbeef->volume_get_amp()));
}

static int set_volume()
{
    if (state == OUTPUT_STATE_STOPPED || !pa_s || !plugin.has_volume) {
        return -OP_ERROR_INTERNAL;
    }

    set_volume_value();

    pa_threaded_mainloop_lock(pa_ml);
    uint32_t idx = pa_stream_get_index(pa_s);
    if (idx == PA_INVALID_INDEX) {
        pa_threaded_mainloop_unlock(pa_ml);
        return -OP_ERROR_INTERNAL;
    }
    return _pa_nowait_unlock(pa_context_set_sink_input_volume(pa_ctx,
                            idx,
                            &pa_vol,
                            NULL,
                            NULL));

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

static int _pa_stream_flush(void)
{
    pa_threaded_mainloop_lock(pa_ml);
    return _pa_wait_unlock(pa_stream_flush(pa_s, _pa_stream_success_cb, NULL));
}

static int _pa_stream_cork(int pause_)
{
    pa_threaded_mainloop_lock(pa_ml);

    return _pa_wait_unlock(pa_stream_cork(pa_s, pause_, _pa_stream_success_cb, NULL));
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
    log_err("Pulseaudio: Error creating context. Reason: %s", pa_strerror(pa_context_errno(pa_ctx)));
    pa_context_unref(pa_ctx);
    pa_ctx = NULL;

    pa_threaded_mainloop_unlock(pa_ml);

    return -OP_ERROR_INTERNAL;
}

static void
stream_event_cb(pa_stream *p, const char *name, pa_proplist *pl, void *userdata)
{
    if (!pa_s || !deadbeef->conf_get_int (CONFSTR_PULSE_PAUSEONCORK, PULSE_DEFAULT_PAUSEONCORK)) {
        return;
    }

    if (!strcmp(name, PA_STREAM_EVENT_REQUEST_CORK) && state != OUTPUT_STATE_PAUSED) {
        cork_requested = 1;
        pa_stream_flush(pa_s, NULL, NULL);
        pa_stream_cork(pa_s, 1, NULL, NULL);
        state = OUTPUT_STATE_PAUSED;
        deadbeef->sendmessage(DB_EV_PAUSED, 0, 1, 0);
    } else if (!strcmp(name, PA_STREAM_EVENT_REQUEST_UNCORK) && cork_requested) {
        cork_requested = 0;
        pa_stream_cork(pa_s, 0, NULL, NULL);
        state = OUTPUT_STATE_PLAYING;
        deadbeef->sendmessage(DB_EV_PAUSED, 0, 0, 0);
    }
}

static void _setformat_apply() {

    deadbeef->mutex_lock(mutex);

    if (_setformat_state == 0) {
        _setformat_state = 1;
        pa_stream_disconnect(pa_s);
    } else if (_setformat_state == 1) {
        if (pa_s) pa_stream_unref(pa_s);
        pa_s = NULL;

        pa_proplist	*pl;
        int rc;

        memcpy (&plugin.fmt, &requested_fmt, sizeof (ddb_waveformat_t));
        if (!plugin.fmt.channels) {
            // generic format
            plugin.fmt.bps = 16;
            plugin.fmt.is_float = 0;
            plugin.fmt.channels = 2;
            plugin.fmt.samplerate = 44100;
            plugin.fmt.channelmask = 3;
        }
        if (plugin.fmt.samplerate > PA_RATE_MAX) {
            plugin.fmt.samplerate = PA_RATE_MAX;
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
            pa_ss.format = PA_SAMPLE_INVALID;
        };

        // Simulate stream create failure by uncommenting next line:
        //pa_ss.format = PA_SAMPLE_INVALID;


        pl = _create_stream_proplist();
        pa_proplist *songpl = get_stream_prop_song(NULL);
        pa_proplist_update(pl, PA_UPDATE_MERGE, songpl);
        pa_proplist_free(songpl);


        trace("Pulseaudio: create stream\n");
        pa_s = pa_stream_new_with_proplist(pa_ctx, NULL, &pa_ss, &pa_cmap, pl);
        pa_proplist_free(pl);
        if (!pa_s) {
            log_err("Pulseaudio: Error creating stream! Check sample format, etc...\n");
            state = OUTPUT_STATE_STOPPED;
            //pa_threaded_mainloop_signal(pa_ml, 0);
            goto out_fail;
        }

        pa_stream_set_state_callback(pa_s, _pa_stream_running_cb, NULL);
        pa_stream_set_write_callback(pa_s, stream_request_cb, NULL);
        pa_stream_set_event_callback(pa_s, stream_event_cb, NULL);

        int ms = deadbeef->conf_get_int(CONFSTR_PULSE_BUFFERSIZE, PULSE_DEFAULT_BUFFERSIZE);
        if (ms < 0) ms = 100;
        buffer_size = pa_usec_to_bytes(ms * 1000, &pa_ss);

        pa_buffer_attr attr = {
            .maxlength = (uint32_t) -1,
            .tlength = (uint32_t) buffer_size,
            .prebuf = (uint32_t) -1,
            .minreq = (uint32_t) -1,
        };

        if (plugin.has_volume) {
            set_volume_value();
        }

        deadbeef->conf_lock ();
        const char *dev = deadbeef->conf_get_str_fast (PULSE_PLUGIN_ID "_soundcard", "default");

        // TODO: Handle case of configured device no longer existing, fallback to default

        rc = pa_stream_connect_playback(pa_s,
                        (!strcmp(dev, "default")) ? NULL: dev,
                        &attr,
                        PA_STREAM_NOFLAGS,
                        plugin.has_volume ? &pa_vol : NULL,
                        NULL);
        deadbeef->conf_unlock ();

        if (rc)
            trace("Pulseaudio: Error connecting stream!\n");

    } else if  (_setformat_state == 2) {

        pa_context_get_sink_input_info(pa_ctx, pa_stream_get_index(pa_s),
                _pa_sink_input_info_cb, NULL);

        _setformat_requested = 0;

    }
out_fail:


    deadbeef->mutex_unlock(mutex);

    trace("Pulseaudio: _setformat_apply end state = %d\n", _setformat_state);
}

static void _setformat_apply_once (pa_mainloop_api *m, void *userdata) {
    _setformat_apply();
}


static void stream_request_cb(pa_stream *s, size_t requested_bytes, void *userdata) {
    char *buffer = NULL;
    ssize_t buftotal = requested_bytes;
    int bytesread;
    // trace("Pulseaudio: buftotal preloop %zd\n", buftotal);
    while (buftotal > 0)  {
        size_t bufsize = buftotal;

        pa_stream_begin_write(s, (void**) &buffer, &bufsize);
        // trace("Pulseaudio: bufsize begin write %zu\n", bufsize);

        if (_setformat_requested || state != OUTPUT_STATE_PLAYING || !deadbeef->streamer_ok_to_read (-1)) {
            memset (buffer, 0, bufsize);
            bytesread = bufsize;
        } else {
            bytesread = deadbeef->streamer_read(buffer, bufsize);
            if (bytesread < 0) {
                bytesread = 0;
            }
        }
        pa_stream_write(s, buffer, bytesread, NULL, 0LL, PA_SEEK_RELATIVE);

        buftotal -= bytesread;
        // trace("Pulseaudio: buftotal %zd\n", buftotal);
    }

    if (_setformat_requested && _setformat_state == 0 ) {
        pa_mainloop_api_once(pa_threaded_mainloop_get_api(pa_ml), _setformat_apply_once, NULL );
    }
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
    trace("Pulseaudio: setformat called!\n");
    deadbeef->mutex_lock(mutex);
    _setformat_requested = 1;
    _setformat_state = 0;
    memcpy (&requested_fmt, fmt, sizeof (ddb_waveformat_t));
    deadbeef->mutex_unlock(mutex);
    return 0;
}

static int pulse_free(void)
{
    trace("pulse_free\n");

    state = OUTPUT_STATE_STOPPED;
    if (!pa_ml) {
        return OP_ERROR_SUCCESS;
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

    if (pa_ml) {
        pa_threaded_mainloop_stop(pa_ml);
        pa_threaded_mainloop_free(pa_ml);
        pa_ml = NULL;
    }

    return OP_ERROR_SUCCESS;
}

static int pulse_play(void)
{
    trace ("pulse_play\n");

    if (!pa_ml) {
        pulse_init();
    }
    int ret;

    memcpy (&requested_fmt, &plugin.fmt, sizeof (ddb_waveformat_t));
    ret = _pa_create_context(); // Should only return once context and stream is ready/failed

    trace("Pulseaudio: after context create, pa_s = %p\n", pa_s);
    if (!pa_s) ret = OP_ERROR_INTERNAL;

    if (ret != OP_ERROR_SUCCESS) {
        pulse_free();
    }
    return ret;
}

static int pulse_stop(void)
{
    pulse_free();

    return OP_ERROR_SUCCESS;
}

static int pulse_pause(void)
{
    if (!pa_s) {
        pulse_play();
    }

    state = OUTPUT_STATE_PAUSED;
    _pa_stream_flush();
    return _pa_stream_cork(1);
}

static int pulse_unpause(void)
{
    if (!pa_s) {
        pulse_play();
    }

    state = OUTPUT_STATE_PLAYING;
    cork_requested=0;
    return _pa_stream_cork(0);
}


static int pulse_get_state(void)
{
    return state;
}



static int pulse_plugin_start(void)
{
    mutex = deadbeef->mutex_create();
    tfbytecode = deadbeef->tf_compile("[%artist% - ]%title%");
    return 0;
}

static int pulse_plugin_stop(void)
{
    deadbeef->mutex_free(mutex);
    deadbeef->tf_free(tfbytecode);
    return 0;
}

DB_plugin_t * pulse2_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

static void proplistupdate_success_cb(pa_stream *s, int success, void *userdata)
{
    pa_proplist_free(userdata);
}

static int
pulse_message (uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2) {
    switch (id) {
    case DB_EV_SONGSTARTED:
        if (state == OUTPUT_STATE_PLAYING) {
            pa_proplist *pl = get_stream_prop_song(((ddb_event_track_t *)ctx)->track);
            pa_threaded_mainloop_lock(pa_ml);
            _pa_nowait_unlock(pa_stream_proplist_update (pa_s, PA_UPDATE_REPLACE, pl, proplistupdate_success_cb, pl));
        }
        break;
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

struct enum_card_userdata {
    void (*callback)(const char *name, const char *desc, void *);
    void *userdata;
    pa_mainloop *ml;
};

static void
sink_info_callback(pa_context *c, const pa_sink_info *i, int eol, void *userdata)  {
    struct enum_card_userdata *ud = (struct enum_card_userdata *)userdata;

    if (eol) {
        pa_mainloop_quit(ud->ml, 0);
        return;
    }

    // Avoid crazy long descriptions, they grow the GTK dropdown box in deadbeef GUI
    // Truncate with a middle ellipsis so we catch output port names that are always at the end
    char buf[256];
    if (strlen(i->description) > 80) {
        strncpy (buf, i->description, 38);
        strcat(buf, "...");
        strcat (buf, i->description+strlen(i->description)-38);
        
    } else {
        strcpy(buf, i->description ? i->description : "");
    }

    ud->callback(i->name ? i->name : "", buf, ud->userdata);
}

void enumctx_state_cb(pa_context *c, void *userdata)
{
    struct enum_card_userdata *ud = (struct enum_card_userdata *)userdata;
    switch (pa_context_get_state(c))
    {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;

    case PA_CONTEXT_READY:
    {
        pa_context_get_sink_info_list(c, sink_info_callback, ud);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
    {
        pa_mainloop_quit(ud->ml, 0);
        break;
    }
    default:
        return;
    }
}

static void
pulse_enum_soundcards(void (*callback)(const char *name, const char *desc, void *), void *userdata)
{
    pa_context *enumctx;
    int ret;
    struct enum_card_userdata ud = {callback, userdata};

    pa_mainloop *ml = pa_mainloop_new();
    ud.ml = ml;
    pa_mainloop_api *api = pa_mainloop_get_api(ml);
    if (!(enumctx = pa_context_new(api, "DeaDBeeF"))) {
        fprintf(stderr, "Pulseaudio enum soundcards error: pa_context_new() failed.");
        goto fail;
    }

    pa_context_set_state_callback(enumctx, enumctx_state_cb, &ud);

    // Read serveraddr from config
    const char *server;
    deadbeef->conf_lock();
    server = deadbeef->conf_get_str_fast (CONFSTR_PULSE_SERVERADDR, "");

    if (pa_context_connect(enumctx, *server ? server : NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
    {
        deadbeef->conf_unlock();
        fprintf(stderr, "Pulseaudio enum soundcards error: %s\n", pa_strerror(pa_context_errno(enumctx)));
        goto fail;
    }
    deadbeef->conf_unlock();
    if (pa_mainloop_run(ml, &ret) < 0) {
        fprintf(stderr, "Pulseaudio enum soundcards error: pa_mainloop_run() failed.\n");
    }

    if (enumctx) {
        pa_context_disconnect(enumctx);
    }
fail:
    if (enumctx) {
        pa_context_unref(enumctx);
    }

    if (ml) {
        pa_mainloop_free(ml);
    }
}


static const char settings_dlg[] =
    "property \"PulseAudio server (leave empty for default)\" entry " CONFSTR_PULSE_SERVERADDR " \"\";\n"
    "property \"Preferred buffer size in ms\" entry " CONFSTR_PULSE_BUFFERSIZE " " STR(PULSE_DEFAULT_BUFFERSIZE) ";\n"
    "property \"Use pulseaudio volume control\" checkbox " CONFSTR_PULSE_VOLUMECONTROL " " STR(PULSE_DEFAULT_VOLUMECONTROL) ";\n"
    "property \"Pause instead of mute when corked (e.g. when receiving calls)\" checkbox " CONFSTR_PULSE_PAUSEONCORK " " STR(PULSE_DEFAULT_PAUSEONCORK) ";\n";

static DB_output_t plugin =
{
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 1,
    .plugin.version_minor = 1,
    .plugin.flags = DDB_PLUGIN_FLAG_LOGGING,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = PULSE_PLUGIN_ID,
    .plugin.name = "PulseAudio output plugin version 2",
    .plugin.descr = "This is a new pulseaudio plugin that uses the asynchronous API",
    .plugin.copyright =
        "PulseAudio output plugin for DeaDBeeF Player\n"
        "Copyright (C) 2015-2020 Nicolai Syvertsen <saivert@saivert.com>\n"
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
    .enum_soundcards = pulse_enum_soundcards,
    .has_volume = PULSE_DEFAULT_VOLUMECONTROL,
};
