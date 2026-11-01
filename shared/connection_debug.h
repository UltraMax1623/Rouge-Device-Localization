#pragma once

// Set to 0 after collecting a connection trace. Never prints credentials.
#ifndef CONNECTION_DEBUG
#define CONNECTION_DEBUG 1
#endif

bool connectionDebugAllowed();
inline void connectionTrace(const char *line) {
#if CONNECTION_DEBUG
  if (connectionDebugAllowed()) Serial.printf("[%lu ms] %s\n", millis(), line);
#endif
}
