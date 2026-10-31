#pragma once
#include "ESPixelStick.h"
struct TestOLED { void ShowToast(const String&, const String&) {} };
inline TestOLED OLED;
