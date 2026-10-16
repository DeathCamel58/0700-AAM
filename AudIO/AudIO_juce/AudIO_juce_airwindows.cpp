// The airwindows effects are AudIO's second translation unit
// (AudIO_airwindows.cpp): DawEngine registers them through
// initAirWindowsEffects(). JUCE compiles every AudIO_juce*.cpp at the top
// of the module separately, so they stay out of AudIO_juce.cpp's unit.
#include "AudIO_juce.h"
#include "../AudIO/AudIO_airwindows.cpp"
