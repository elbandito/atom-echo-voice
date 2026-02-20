#ifndef LINUX_BUILD
#include <driver/i2s.h>
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <string.h>

#include "main.h"

#define TICK_INTERVAL 15

static const char *AGENT_INSTRUCTIONS =
    "You are a caring counselor named Sarah who is here to listen and offer "
    "encouragement. When a conversation begins, immediately greet the user "
    "warmly and introduce yourself. Be warm, empathetic, and supportive. "
    "Listen attentively and validate the user's feelings. Offer gentle "
    "encouragement and positive perspectives when appropriate. Speak in a "
    "calm, reassuring tone. Keep your responses concise and conversational.";

static const char *AGENT_VOICE = "shimmer";

PeerConnection *peer_connection = NULL;
static bool session_configured = false;
static bool greeting_triggered = false;

#ifndef LINUX_BUILD
StaticTask_t task_buffer;
void oai_send_audio_task(void *user_data) {
  oai_init_audio_encoder();

  while (1) {
#if SEND_AUDIO
    oai_send_audio(peer_connection);
#endif
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif

static void oai_onconnectionstatechange_task(PeerConnectionState state,
                                             void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED) {
#ifndef LINUX_BUILD
    esp_restart();
#endif
  } else if (state == PEER_CONNECTION_CONNECTED) {
#ifndef LINUX_BUILD
#if CONFIG_OPENAI_BOARD_ESP32_S3
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        20000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(oai_send_audio_task, "audio_publisher", 20000,
                                  NULL, 7, stack_memory, &task_buffer, 0);
#elif CONFIG_OPENAI_BOARD_M5_ATOMS3R
    // Because we change the sampling rate to 16K, so we need increased the 
    // memory size, if not will overflow :)
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        40000 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    xTaskCreateStaticPinnedToCore(oai_send_audio_task, "audio_publisher", 40000,
                                  NULL, 7, stack_memory, &task_buffer, 0);
#endif
#endif
  }
}

static void oai_on_icecandidate_task(char *description, void *user_data) {
  char local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
  oai_http_request(description, local_buffer);
  peer_connection_set_remote_description(peer_connection, local_buffer);
}

static void send_session_update() {
  char msg[2048];
  snprintf(msg, sizeof(msg),
      "{\"type\":\"session.update\",\"session\":{"
      "\"modalities\":[\"audio\",\"text\"],"
      "\"voice\":\"%s\","
      "\"instructions\":\"%s\","
      "\"input_audio_format\":\"pcm16\","
      "\"output_audio_format\":\"pcm16\","
      "\"turn_detection\":{\"type\":\"server_vad\"}}}",
      AGENT_VOICE, AGENT_INSTRUCTIONS);

  peer_connection_datachannel_send(peer_connection, msg, strlen(msg));
}

static void send_response_create() {
  const char *msg =
      "{\"type\":\"response.create\",\"response\":"
      "{\"modalities\":[\"audio\",\"text\"]}}";
  peer_connection_datachannel_send(peer_connection, (char *)msg, strlen(msg));
}

// Fast check if a substring exists in a non-null-terminated buffer
static bool buf_contains(const char *buf, size_t len, const char *needle) {
  size_t nlen = strlen(needle);
  if (nlen > len) return false;
  for (size_t i = 0; i <= len - nlen; i++) {
    if (memcmp(buf + i, needle, nlen) == 0) return true;
  }
  return false;
}

static void oai_ondatachannel_message(char *msg, size_t len, void *userdata,
                                      uint16_t sid) {
  if (len == 0) return;

  // Only handle events we care about — skip everything else fast
  // Check the most frequent events first to return early
  if (buf_contains(msg, len, "\"response.audio_transcript.delta\"") ||
      buf_contains(msg, len, "\"response.audio.delta\"") ||
      buf_contains(msg, len, "\"rate_limits.updated\"") ||
      buf_contains(msg, len, "\"output_audio_buffer.")) {
    return;  // High-frequency events — skip silently
  }

  if (buf_contains(msg, len, "\"session.created\"")) {
    ESP_LOGI(LOG_TAG, "Session created, sending instructions...");
    send_session_update();
  } else if (buf_contains(msg, len, "\"session.updated\"")) {
    ESP_LOGI(LOG_TAG, "Session configured");
    session_configured = true;
    if (!greeting_triggered) {
      greeting_triggered = true;
      send_response_create();
    }
  } else if (buf_contains(msg, len, "\"error\"")) {
    // Need null-terminated copy only for error logging
    size_t safe_len = len < 511 ? len : 511;
    char buf[512];
    memcpy(buf, msg, safe_len);
    buf[safe_len] = '\0';
    ESP_LOGE(LOG_TAG, "API error: %s", buf);
  }
}

static void oai_ondatachannel_open(void *userdata) {
  ESP_LOGI(LOG_TAG, "Data channel open, negotiating...");
  session_configured = false;
  greeting_triggered = false;
  peer_connection_datachannel_open(peer_connection, "oai-events", 0);
}

static void oai_ondatachannel_close(void *userdata) {
  ESP_LOGW(LOG_TAG, "=== Data channel CLOSED ===");
}

void oai_webrtc() {
  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        oai_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = NULL,
  };

  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  peer_connection_oniceconnectionstatechange(peer_connection,
                                             oai_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, oai_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection, oai_ondatachannel_message,
                                oai_ondatachannel_open,
                                oai_ondatachannel_close);
  peer_connection_create_offer(peer_connection);

  while (1) {
    peer_connection_loop(peer_connection);
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
