#define SDU_VERSION_MAJOR 1
#define SDU_VERSION_MINOR 1
#define SDU_VERSION_PATCH 0
#define _SDU_STR(x) #x
#define SDU_STR(x) _SDU_STR(x)
#define M5_SD_UPDATER_VERSION SDU_STR(SDU_VERSION_MAJOR) "." SDU_STR(SDU_VERSION_MINOR) "." SDU_STR(SDU_VERSION_PATCH)
