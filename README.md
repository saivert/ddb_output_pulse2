Improved DeaDBeeF pulseaudio plugin
===================================

simplefork branch description
-----------------------------
This branch contains a fork of the pulseaudio simple API wrapper. I have hadded callbacks for sink input volume control and a function for setting the volume. I wonder why this code wasn't there to begin with because this is an essential feature that I think the simple API could have without suddenly trying to cram all of pulseaudio into it and thus not making it so simple anymore.

It is what I could quickly put together to have a working plugin that has the volume control integration with pulseaudio with minimal changes to the original DeaDBeeF pulseaudio plugin code.
This sadly requires me to pull in all of pulseaudio source as there are a few internal function dependencies in the simple API.
