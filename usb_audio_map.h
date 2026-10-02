#pragma once

// UAC2 channel-config bitmap for the detection cube.
// Linux prints these names in ascending bit order, and that order is the
// USB channel order. Channel 1 stays microphone 1 (north). A closer name
// whose bit is lower would be printed on an earlier channel, so it is not
// used. There is no standard bottom-front-left or bottom-front-right name.
//
//   ch  mic  name  bit  direction on the cube
//    1    1  TFC    13  north,              +35°
//    2    2  TRR    17  azimuth 120°,       +35°
//    3    3  TSL    22  azimuth 240°,       +35°
//    4    4  BC     24  azimuth 180°,       −35°
//    5    5  RLC    25  azimuth 300°,       −35°
//    6    6  RRC    26  azimuth  60°,       −35°
#define HET68_BM_CHANNEL_CONFIG ((audio_channel_config_t)( \
    AUDIO_CHANNEL_CONFIG_TOP_FRONT_CENTER | \
    AUDIO_CHANNEL_CONFIG_TOP_BACK_RIGHT | \
    AUDIO_CHANNEL_CONFIG_TOP_SIDE_LEFT | \
    AUDIO_CHANNEL_CONFIG_BOTTOM_CENTER | \
    AUDIO_CHANNEL_CONFIG_BACK_LEFT_OF_CENTER | \
    AUDIO_CHANNEL_CONFIG_BACK_RIGHT_OF_CENTER))
