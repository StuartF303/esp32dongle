#include "partition_info.h"

const char *partitionSubtypeName(const esp_partition_t *p) {
  if (p->type == ESP_PARTITION_TYPE_APP) {
    switch (p->subtype) {
      case ESP_PARTITION_SUBTYPE_APP_FACTORY:
        return "factory";
      case ESP_PARTITION_SUBTYPE_APP_OTA_0:
        return "ota_0";
      case ESP_PARTITION_SUBTYPE_APP_OTA_1:
        return "ota_1";
      default:
        return "app/?";
    }
  }
  switch (p->subtype) {
    case ESP_PARTITION_SUBTYPE_DATA_NVS:
      return "nvs";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS:
      return "nvs_keys";
    case ESP_PARTITION_SUBTYPE_DATA_OTA:
      return "ota";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP:
      return "coredump";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:
      return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS:
      return "littlefs";
    default:
      return "data/?";
  }
}

const char *otaStateName(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW:
      return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
      return "VALID";
    case ESP_OTA_IMG_INVALID:
      return "INVALID";
    case ESP_OTA_IMG_ABORTED:
      return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
      return "UNDEFINED";
    default:
      return "UNKNOWN";
  }
}
