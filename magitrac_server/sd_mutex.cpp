#include "sd_mutex.h"

SemaphoreHandle_t sSdMutex = nullptr;

void sdMutexInit() {
    if (!sSdMutex) sSdMutex = xSemaphoreCreateMutex();
}

SdLock::SdLock() {
    if (sSdMutex) {
        xSemaphoreTake(sSdMutex, portMAX_DELAY);
        _taken = true;
    }
}

SdLock::~SdLock() {
    if (_taken && sSdMutex) xSemaphoreGive(sSdMutex);
}
