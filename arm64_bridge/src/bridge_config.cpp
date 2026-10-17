#include "bridge_config.h"
#include "thunk_manager.h"

BridgeConfig& GetBridgeConfig() {
    static BridgeConfig cfg;
    return cfg;
}

uint32_t GetVirtualWidth()  { return ThunkManager::s_screenWidth; }
uint32_t GetVirtualHeight() { return ThunkManager::s_screenHeight; }
