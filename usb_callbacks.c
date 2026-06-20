// usb_callbacks.c — intentionally empty for v24
//
// Older project revisions had TinyUSB audio callbacks in this file.
// In v23/v24 the callbacks live in main.c so they can share audio/I2S state.
//
// This file is kept only because the existing CMakeLists.txt still compiles
// usb_callbacks.c. Overwriting the old file with this empty compatibility unit
// fixes linker errors like:
//
//   multiple definition of `tud_audio_set_itf_cb'
//   multiple definition of `tud_audio_get_req_ep_cb'
//   multiple definition of `tud_audio_set_req_itf_cb'
//
// Do not add TinyUSB callback definitions here unless you also remove them
// from main.c.
