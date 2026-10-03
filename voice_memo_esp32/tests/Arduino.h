#pragma once

// Host-test facade: with `-Itests` on the command line, `#include <Arduino.h>`
// resolves here instead of to the ESP32 core. Only the pieces the Arduino-free
// power headers happen to include are provided; see tests/arduino_shim.h.
//
// Deliberately a separate file from arduino_shim.h so that `-Itests` in
// run_host_tests.sh does NOT shadow <Arduino.h> for every other suite: that flag
// is added only to the power_manager suite, which is the only one that includes
// a header using <Arduino.h>.

#include "arduino_shim.h"
