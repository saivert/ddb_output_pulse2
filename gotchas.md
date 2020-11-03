Some gotchas during development of this plugin
----------------------------------------------
The stream request callback set with `pa_stream_set_write_callback` must fill the buffer completely with length bytes as requested. Failure in doing so will cause Pulseaudio to no longer call your callback function and the stream will stall out. There is no indication from the client API or server that it is waiting for more data. This could easily have some timeout and then report an error if more data has not been written in a reasonable time.

I guess Lennart forgot to document this when he fixed his own mistake in February 2010:
https://github.com/pulseaudio/pulseaudio/commit/d57ba824145f9e42610c0aa21740f219ba671041



This is another beauty, `pa_stream_new_with_proplist` will error out if media name propery in the supplied property list is unset. Yes there is no mention that the name argument to this function is optional, but you generally don't want to set this in the call directly if you later plan to set it via properties. That the property must exist at the time you call `pa_stream_new_with_proplist` if name argument is NULL is not clearly documented. Also if name argument is supplied it will overwrite the media name property from the property list anyways which is something I consider a bug.

https://github.com/pulseaudio/pulseaudio/blob/4e3a080d7699732be9c522be9a96d851f97fbf11/src/pulse/stream.c#L102

So in short: PulseAudio API documentation is terrible and it is expected that you peruse the Pulseaudio source code to really understand how it works.
