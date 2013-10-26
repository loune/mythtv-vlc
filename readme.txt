VLC MythTV access plugin Version 0.8 (for windows)
==================================================
by Loune Lam 26/10/2013

To install it, just extract the zip and move libaccess_myth_plugin.dll to your VLC/plugins directory. Once you've installed the plugin, you can now open URLs that start with myth://

Be sure to set your mythbackend URL in the VLC preferences for the Media Browser to work.

This was tested to be working with VLC 2.1.0 and MythTV 0.27.

More info:
http://siphon9.net/loune/2010/11/mythtv-vlc-plugin-now-supports-vlc-1-1-5/
http://siphon9.net/loune/2008/12/play-mythtv-recordings-in-vlc/


Build Instructions
==================

Copy myth.c to [vlc-source]/modules/access

Append these lines to Modules.am in the same directory:

SOURCES_access_myth = myth.c
libvlc_LTLIBRARIES += \
	libaccess_myth_plugin.la \
	$(NULL)

After that, please refer to the VLC wiki on compiling VLC.