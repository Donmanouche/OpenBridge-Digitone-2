// Parameters of the Digitakt II machines played by the Digitone II's alternative firmware (DN2 MK2).
// Measured on the device (OS 1.11 build 0059): the machine number in the sound (setup byte 22 /
// container 0xF4), the container slot each knob changes, and a Digitone II parameter id that writes
// that slot with the whole 0..32512 range (the firmware accepts any machine's id for a slot and
// clamps the value to that id's range). Section 40 + n = alternative machine 8 + n.
// key, object, metaIndex, pid, section, knob, min, max, default, decimals, cc, nrpn, format, slot, name, sectionName, label

// One Shot (machine 8)
{"Sound_OneShotTune", ParamObject::Sound, -1, 254, 40, 25, 1024, 31744, 16384, true, -1, -1, ParamFormat::BipolarDecimal, 33, "Tune", "One Shot", "TUNE"},
{"Sound_OneShotPlayMode", ParamObject::Sound, -1, 234, 40, 26, 0, 32512, 0, false, -1, -1, ParamFormat::Plain, 49, "Play Mode", "One Shot", "PLAY"},
{"Sound_OneShotSample", ParamObject::Sound, -1, 237, 40, 28, 0, 32512, 0, false, -1, -1, ParamFormat::Plain, 51, "Sample", "One Shot", "SAMP"},
{"Sound_OneShotStart", ParamObject::Sound, -1, 317, 40, 29, 0, 32512, 0, true, -1, -1, ParamFormat::Plain, 34, "Start", "One Shot", "STRT"},
{"Sound_OneShotLength", ParamObject::Sound, -1, 276, 40, 30, 0, 32512, 30720, true, -1, -1, ParamFormat::Plain, 53, "Length", "One Shot", "LEN"},
{"Sound_OneShotLoop", ParamObject::Sound, -1, 275, 40, 31, 0, 32512, 0, true, -1, -1, ParamFormat::Plain, 52, "Loop Position", "One Shot", "LOOP"},
{"Sound_OneShotLevel", ParamObject::Sound, -1, 219, 40, 32, 0, 32512, 25600, false, -1, -1, ParamFormat::Plain, 38, "Level", "One Shot", "LEV"},
