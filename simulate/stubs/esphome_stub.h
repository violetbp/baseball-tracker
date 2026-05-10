#pragma once

// This file provides stub ESPHome headers for the simulator.
// All ESPHome types are shimmed via esp_shim.h
#include "esp_shim.h"

// Component base class - handled by esp_shim.h
namespace esphome {
  class Component {
   public:
    virtual void setup() {}
    virtual void loop() {}
    virtual void dump_config() {}
    virtual float get_setup_priority() const { return 400.0f; }
  };
}
