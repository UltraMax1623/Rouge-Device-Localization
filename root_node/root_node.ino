// Upload to the root ESP32 board. Select its actual board model in the IDE.
// S3 RGB only: uncomment with the actual data GPIO if the board variant differs.
// #define MODE_RGB_PIN 38  // DevKitC-1 v1.1; initial revision uses 48.
#define ROOT_FIRMWARE 1
#define FIRMWARE_DEBUG 1  // Serial connection and command logs at 115200 baud.
#define BOARD_NUMBER 0x01
static_assert(BOARD_NUMBER == 0x01, "Root station identity must always be 01");
#define TEST_BOARD_COUNT 4  // Root 01 plus uniquely numbered normal nodes 02, 03, 04.
static_assert(TEST_BOARD_COUNT >= 2 && TEST_BOARD_COUNT <= 15,
              "Test board count must be 2..15");
#include <WebSocketsClient.h>
#include "../shared/node_firmware.h"
