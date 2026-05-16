#pragma once
#include "esp_err.h"
#include "debbie.h"
#include <stdbool.h>

/**
 * @brief  Connect to the companion server WebSocket for push notifications.
 *
 *  The companion server (Node.js) bridges WhatsApp, email, Spotify, and
 *  custom agent notifications.  Messages arrive as JSON on the WebSocket
 *  and are surfaced via the callback set in notification_client_init().
 */

typedef void (*notif_cb_t)(const debbie_notification_t *notif, void *user_ctx);

/**
 * @brief  Initialise and connect to the companion server.
 *
 * @param server_url   WebSocket URL of companion server, e.g. "ws://192.168.1.10:3001"
 * @param cb           Callback invoked on new notification (task context).
 * @param user_ctx     Passed through to callback.
 */
esp_err_t notification_client_init(const char *server_url,
                                   notif_cb_t cb,
                                   void *user_ctx);

/**
 * @brief  Disconnect from the companion server.
 */
esp_err_t notification_client_deinit(void);

/**
 * @brief  Return true if connected to companion server.
 */
bool notification_client_is_connected(void);

/**
 * @brief  Get the count of unread notifications.
 */
int notification_client_unread_count(void);

/**
 * @brief  Mark all notifications as read.
 */
void notification_client_clear(void);

/**
 * @brief  Get a summary of pending notifications as a JSON string.
 *         Caller must free() the returned pointer.
 */
char *notification_client_get_summary_json(void);

/**
 * @brief  Send a Spotify command to the companion server.
 *         action examples: "play", "pause", "next", "search:lofi beats"
 */
esp_err_t notification_client_spotify_command(const char *action);
