Onset Trigger LV2
=================

An audio to midi converter currently intended for Bass/Kick-drums.

About
-----

Did you ever had a recording of a kickdrum that just sounds bad and pretty
much any MIDI sampled kick is more suitable for repairing the recording?

You have two options, re-record the drums or trigger a MIDI instrument instead.
This plugin is intended for the latter. It's in very early development but
works already for some cases.

Install
-------

Compiling this plugin requires the LV2 SDK, gnu-make and a c-compiler.

```bash
  git clone git://github.com/x42/onsettrigger.lv2
  cd onsettrigger.lv2
  make
  sudo make install PREFIX=/usr

  # test run w/simple GTK GUI
  jalv.gtk 'http://gareus.org/oss/lv2/onsettrigger#bassdrum_mono'
  #or
  jalv.gtk 'http://gareus.org/oss/lv2/onsettrigger#bassdrum_stereo'


  sudo make uninstall PREFIX=/usr
```

Note to packagers: The Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CFLAGS`).
