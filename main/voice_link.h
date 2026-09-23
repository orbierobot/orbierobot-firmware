#pragma once
/* The robot's half of push-to-talk.
 *
 * The phone and the robot never speak to each other, and cannot: the
 * microphone needs a secure context, which a plain-HTTP LAN page is not, and
 * an HTTPS page cannot reach back to plain HTTP. So the phone posts a question
 * to the server, and the robot collects the spoken answer from the same place.
 *
 *   phone  --HTTPS-->  /api/ask       question in, answer queued
 *   robot  --HTTPS-->  /api/pending   collects the answer, plays it
 *   robot  --HTTPS-->  /api/frame     pushes what it can see
 *
 * A poll rather than a push: the robot sits behind the owner's NAT with no
 * inbound route, and holding a socket open from an ESP32 costs a task and
 * reconnects badly on flaky Wi-Fi. A cheap GET every couple of seconds is far
 * more robust, and the server clears each answer as it is read so nothing
 * plays twice.
 *
 * Requires an internet connection, which the robot only has after the captive
 * portal has joined it to a real network.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the polling task.
 *
 * @param api_base   e.g. "https://orbie-apis.vercel.app" (no trailing slash)
 * @param robot_id   the unit's ORBIE_XXXX name, matching its SSID and label
 * @param device_key sent as X-Orbie-Key, or NULL/"" when the server does not
 *                   enforce one
 * @param play_pcm   called with 16 kHz mono 16-bit PCM to play. Given in
 *                   chunks as they arrive, so a long answer never has to fit
 *                   in RAM; `first` marks the start of a new clip.
 * @param grab_jpeg  called to fetch a camera frame to upload. Return the
 *                   buffer and its length, or NULL to skip. `release` is
 *                   called when the upload finishes.
 */
typedef void (*voice_play_fn)(const uint8_t *pcm, size_t len, bool first);
typedef const uint8_t *(*voice_frame_fn)(size_t *len_out);
typedef void (*voice_frame_release_fn)(void);

void voice_link_start(const char *api_base,
                      const char *robot_id,
                      const char *device_key,
                      voice_play_fn play_pcm,
                      voice_frame_fn grab_jpeg,
                      voice_frame_release_fn release_jpeg);

/**
 * @brief Replace the device key used on every call.
 *
 * The app provisions the key over the LAN after registering the robot, which
 * can happen long after voice_link_start(). Without this the robot would keep
 * sending the old (or empty) key until the next reboot, and every poll would
 * 401.
 */
void voice_link_set_key(const char *device_key);

/**
 * @brief Say something out loud, now, in Orbie's voice.
 *
 * Posts @p text to /api/speak and streams the PCM straight to the speaker,
 * the same path a push-to-talk answer takes. Blocking, and it needs the
 * internet, so call it from a task that can afford a few seconds - not from
 * an event handler.
 *
 * @return true if audio played.
 */
bool voice_link_say(const char *text);

/** @brief Upload a frame on the next poll, rather than waiting for the timer. */
void voice_link_request_frame(void);

/** @brief True once a poll has succeeded, for the control panel's status line. */
bool voice_link_online(void);

/** @brief Last error text, or "" - shown in the panel so a failure is visible. */
const char *voice_link_last_error(void);

#ifdef __cplusplus
}
#endif
