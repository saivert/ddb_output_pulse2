/** Cork test
 ** Written by: Nicolai Syvertsen
 ** This creates a stream with media role set to "phone" which will cause pulseaudio to cork other streams.
 ** Press Ctrl-C to exit
 ** License: General Public License version 2
 **/

#include <stdio.h>
#include <pulse/pulseaudio.h>

pa_mainloop *pa_ml;
pa_context *pa_ctx;
pa_stream *pa_s;

static pa_proplist *_create_stream_proplist(void);

static void _pa_stream_running_cb(pa_stream *s, void *data)
{
    const pa_stream_state_t ss = pa_stream_get_state(s);

    switch (ss) {
    case PA_STREAM_READY:
        fprintf(stderr, "Stream ready!\n");
    break;
    case PA_STREAM_FAILED:
        fprintf(stderr, "Stream failed!\n");
    case PA_STREAM_TERMINATED:
        pa_mainloop_quit(pa_ml, 0);
    default:
        return;
    }
}

void enumctx_state_cb(pa_context *c, void *userdata)
{

    switch (pa_context_get_state(c))
    {
    case PA_CONTEXT_UNCONNECTED:
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
        break;

    case PA_CONTEXT_READY:
    {
        pa_sample_spec pa_ss;
        pa_sample_spec_init(&pa_ss);
        pa_ss.channels=2;
        pa_ss.format = PA_SAMPLE_S16LE;
        pa_ss.rate = 44100;
        pa_proplist *pl = _create_stream_proplist();
        pa_s = pa_stream_new_with_proplist(pa_ctx, "playback", &pa_ss, NULL, pl);
        if (!pa_s) {
            fprintf(stderr, "Failed to create stream %s\n", pa_strerror(pa_context_errno(pa_ctx)));
            pa_mainloop_quit(pa_ml, 0);
        }
        pa_proplist_free(pl);
        pa_stream_set_state_callback(pa_s, _pa_stream_running_cb, NULL);

        pa_stream_connect_playback(pa_s,
                        NULL,
                        NULL,
                        PA_STREAM_NODIRECTION,
                        NULL,
                        NULL);

        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
    {
        pa_mainloop_quit(pa_ml, 0);
        break;
    }
    default:
        return;
    }
}

static pa_proplist *_create_stream_proplist(void)
{
    pa_proplist	*pl;
    int		 rc;

    pl = pa_proplist_new();
    assert(pl);

    rc = pa_proplist_sets(pl, PA_PROP_MEDIA_ROLE, "phone");
    assert(pl);


    return pl;
}

static void
signal_cb(pa_mainloop_api *api, pa_signal_event *e, int sig, void *userdata)
{
    if (sig = SIGINT) {
    pa_mainloop_quit(pa_ml, 0);
    }
}
int
main(int argc, char *argv[])
{
    int ret;


    pa_ml = pa_mainloop_new();
    pa_mainloop_api *api = pa_mainloop_get_api(pa_ml);
    if (!(pa_ctx = pa_context_new(api, "corky"))) {
        fprintf(stderr, "Pulseaudio error: pa_context_new() failed.");
        goto fail;
    }

    pa_signal_init(api);
    pa_signal_event *se = pa_signal_new(SIGINT, signal_cb, NULL);

    pa_context_set_state_callback(pa_ctx, enumctx_state_cb, NULL);

    if (pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
    {
        fprintf(stderr, "Pulseaudio error: %s\n", pa_strerror(pa_context_errno(pa_ctx)));
        goto fail;
    }
   
    if (pa_mainloop_run(pa_ml, &ret) < 0) {
        fprintf(stderr, "Pulseaudio error: pa_mainloop_run() failed.\n");
    }

    if (pa_s) {
        pa_stream_disconnect(pa_s);
        pa_stream_unref(pa_s);
    }

    if (pa_ctx) {
        pa_context_disconnect(pa_ctx);
    }
fail:
    if (pa_ctx) {
        pa_context_unref(pa_ctx);
    }

    if (pa_ml) {
        pa_mainloop_free(pa_ml);
    }
    pa_signal_free(se);
    pa_signal_done();
    return 0;
}
