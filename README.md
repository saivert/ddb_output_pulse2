Improved DeaDBeeF pulse audio plugin
====================================

I hope for this to eventually replace the standard pulse audio plugin in DeaDBeeF.

It still requires lots of work.

master branch description
-------------------------

The master branch will be focused on the full rewrite of the plugin to use the asynchronous API directly without the simple API wrapper.
This is neccessary because the buffer and latency handling of the current plugin isn't good. I have noticed some glitches that is solved by adjusting the buffer in preferences.
