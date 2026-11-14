#include "usb_power.h"

#include "sdkconfig.h"  // defines which ESP32 chip this is built for

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "soc/usb_serial_jtag_reg.h"
#include "soc/soc.h"
#endif

// No new frames for this long = unplugged
static const uint32_t UNPLUGGED_AFTER_MS = 1000;

bool UsbPower::update(uint32_t now_ms) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    uint32_t frame = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & USB_SERIAL_JTAG_SOF_FRAME_INDEX;  // 11-bit frame counter
    if (frame != last_frame_) {
        last_frame_ = frame;
        last_change_ms_ = now_ms;
    }
    connected_ = last_change_ms_ != 0 && now_ms - last_change_ms_ < UNPLUGGED_AFTER_MS;
#endif
    return connected_;
}
