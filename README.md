Improved DeaDBeeF pulse audio plugin
====================================

I hope for this to eventually replace the standard pulse audio plugin in DeaDBeeF.

2018 Update
-----------

I have done a complete rewrite using code lifted from cmus' pulse output plugin. This turned out to be a huge success. Buffer is now properly handled by pulseudio server which should reduce latency a lot. The extra buffer copying is also gone compared to the old pulseaudio plugin.

Changes from the original plugin
--------------------------------
* Sound output device selection
* Volume control adjusts Pulseaudio's per-app volume
* Better error handling, giving user useful error messages on failure.
* Better buffer handling, now using duration instead of fixed bytecount.
* Pausing playback corks the stream
