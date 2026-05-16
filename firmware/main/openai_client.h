#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief  Initialise the OpenAI Realtime (WebSocket) client.
 *
 *  - Uses WebSocket to wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview
 *  - Audio is streamed bidirectionally as PCM16 mono 24 kHz.
 *  - Callbacks deliver events to the application layer.
 */

typedef enum {
    OAI_EVT_CONNECTED    = 0,
    OAI_EVT_DISCONNECTED,
    OAI_EVT_AUDIO_DELTA,     /* AI speaking — PCM chunk ready */
    OAI_EVT_TRANSCRIPT,      /* text transcript available */
    OAI_EVT_FUNCTION_CALL,   /* function/tool call requested by model */
    OAI_EVT_SESSION_CREATED,
    OAI_EVT_ERROR,
} oai_event_t;

typedef struct {
    oai_event_t type;
    union {
        struct { const int16_t *pcm; size_t count; } audio;   /* AUDIO_DELTA */
        struct { const char *text;                 } transcript;
        struct {                                               /* FUNCTION_CALL */
            const char *name;
            const char *args_json;
            const char *call_id;  /* Must be passed back to send_function_result */
        } fn;
        struct { const char *message;              } error;
    };
} oai_event_data_t;

typedef void (*oai_event_cb_t)(const oai_event_data_t *evt, void *user_ctx);

/**
 * @brief  Connect to OpenAI Realtime API.
 *
 * @param api_key     OpenAI API key.
 * @param system_prompt  Optional persona/system prompt.
 * @param cb          Event callback invoked from a dedicated task.
 * @param user_ctx    Passed through to the callback.
 */
esp_err_t openai_client_connect(const char *api_key,
                                const char *system_prompt,
                                oai_event_cb_t cb,
                                void *user_ctx);

/**
 * @brief  Disconnect and clean up.
 */
esp_err_t openai_client_disconnect(void);

/**
 * @brief  Stream a chunk of microphone PCM audio to the API.
 *         Call this continuously while the user is speaking.
 */
esp_err_t openai_client_send_audio(const int16_t *pcm, size_t count);

/**
 * @brief  Signal end of user speech (triggers model response).
 */
esp_err_t openai_client_commit_audio(void);

/**
 * @brief  Send a text message (alternative to voice).
 */
esp_err_t openai_client_send_text(const char *text);

/**
 * @brief  Send a camera image to the model for visual analysis.
 *         The image is sent as a vision user message via the Chat API
 *         (OpenAI Realtime API does not yet support image input directly).
 *
 * @param jpeg_b64   Base64-encoded JPEG image.
 * @param prompt     User prompt to accompany the image.
 */
esp_err_t openai_client_send_image(const char *jpeg_b64,
                                   const char *prompt);

/**
 * @brief  Return a function-call result so the model can continue.
 */
esp_err_t openai_client_send_function_result(const char *call_id,
                                             const char *result_json);

/**
 * @brief  Return true if the WebSocket is currently connected.
 */
bool openai_client_is_connected(void);
