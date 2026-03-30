#include "runtime_core/global.h"

AppConfig *AppConfig::Instance() {
  static AppConfig s_Config;
  return &s_Config;
}
