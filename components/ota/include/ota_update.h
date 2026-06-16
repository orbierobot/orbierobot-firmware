// OpenPaw OTA updater. App-mediated, GitHub-hosted firmware updates over HTTPS
// with bootloader rollback safety.
//
// Flow: a manifest JSON ({"version","url","sha256","board"}) advertises the
// latest build. If its version differs from the running build, the .bin at
// "url" is downloaded straight to the spare OTA slot, verified, and booted.
// The new image boots in PENDING_VERIFY; it must call ota_mark_valid() after a
// health check or the bootloader reverts on the next reboot.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Log running version and pending-verify state. Call early in app_main.
void ota_update_init(void);

// Confirm the current app is healthy, cancelling any pending rollback.
// No-op if the image is already marked valid.
void ota_mark_valid(void);

// Fetch the manifest at manifest_url; if it advertises a different version,
// download and apply it, then reboot (does not return on success). Blocking —
// call only once Wi-Fi is connected. Returns ESP_OK when already up to date.
esp_err_t ota_check_and_update(const char *manifest_url);

// Running firmware version string from esp_app_desc (the git tag/describe).
const char *ota_running_version(void);

#ifdef __cplusplus
}
#endif
