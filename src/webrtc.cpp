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
  ESP_LOGI(LOG_TAG, "ICE candidate ready, sending HTTP offer...");
  char local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
  oai_http_request(description, local_buffer);
  ESP_LOGI(LOG_TAG, "Got SDP answer (%d bytes)", (int)strlen(local_buffer));
  peer_connection_set_remote_description(peer_connection, local_buffer);
  ESP_LOGI(LOG_TAG, "Remote description set");
}

static void send_session_update() {
  char msg[2048];
  int written = snprintf(msg, sizeof(msg),
      "{\"type\":\"session.update\",\"session\":{"
      "\"modalities\":[\"audio\",\"text\"],"
      "\"voice\":\"%s\","
      "\"instructions\":\"%s\","
      "\"input_audio_format\":\"pcm16\","
      "\"output_audio_format\":\"pcm16\","
      "\"turn_detection\":{\"type\":\"server_vad\"}}}",
      AGENT_VOICE, AGENT_INSTRUCTIONS);

  ESP_LOGI(LOG_TAG, "Sending session.update (%d bytes)...", written);
  ESP_LOGD(LOG_TAG, "session.update payload: %s", msg);
  int ret = peer_connection_datachannel_send(peer_connection, msg, strlen(msg));
  ESP_LOGI(LOG_TAG, "datachannel_send(session.update) returned: %d", ret);
}

static void send_response_create() {
  const char *msg =
      "{\"type\":\"response.create\",\"response\":"
      "{\"modalities\":[\"audio\",\"text\"]}}";
  ESP_LOGI(LOG_TAG, "Sending response.create to trigger greeting...");
  int ret = peer_connection_datachannel_send(peer_connection, (char *)msg, strlen(msg));
  ESP_LOGI(LOG_TAG, "datachannel_send(response.create) returned: %d", ret);
}

static void oai_ondatachannel_message(char *msg, size_t len, void *userdata,
                                      uint16_t sid) {
  if (len == 0) {
    ESP_LOGW(LOG_TAG, "DC recv: empty message (sid=%u)", sid);
    return;
  }

  // Messages from SCTP are not null-terminated; make a safe copy for strstr
  size_t safe_len = len < 4095 ? len : 4095;
  char buf[4096];
  memcpy(buf, msg, safe_len);
  buf[safe_len] = '\0';

  // Log every message type and a preview
  ESP_LOGI(LOG_TAG, "DC recv (sid=%u, len=%d): %.200s%s",
           sid, (int)len, buf, len > 200 ? "..." : "");

  if (strstr(buf, "\"session.created\"")) {
    ESP_LOGI(LOG_TAG, ">>> Event: session.created - sending session.update...");
    send_session_update();
  } else if (strstr(buf, "\"session.updated\"")) {
    ESP_LOGI(LOG_TAG, ">>> Event: session.updated - session configured!");
    session_configured = true;
    if (!greeting_triggered) {
      greeting_triggered = true;
      ESP_LOGI(LOG_TAG, "Triggering initial greeting...");
      send_response_create();
    }
  } else if (strstr(buf, "\"response.created\"")) {
    ESP_LOGI(LOG_TAG, ">>> Event: response.created");
  } else if (strstr(buf, "\"response.audio.delta\"")) {
    ESP_LOGD(LOG_TAG, ">>> Event: response.audio.delta (audio chunk)");
  } else if (strstr(buf, "\"response.audio.done\"")) {
    ESP_LOGI(LOG_TAG, ">>> Event: response.audio.done");
  } else if (strstr(buf, "\"response.done\"")) {
    ESP_LOGI(LOG_TAG, ">>> Event: response.done");
  } else if (strstr(buf, "\"error\"")) {
    ESP_LOGE(LOG_TAG, ">>> Event: ERROR: %s", buf);
  } else {
    // Extract type field for unknown events
    const char *type_start = strstr(buf, "\"type\":\"");
    if (type_start) {
      type_start += 8; // skip past "type":"
      const char *type_end = strchr(type_start, '"');
      if (type_end) {
        ESP_LOGI(LOG_TAG, ">>> Event: %.*s (unhandled)",
                 (int)(type_end - type_start), type_start);
      }
    }
  }
}

static void oai_ondatachannel_open(void *userdata) {
  ESP_LOGI(LOG_TAG, "=== Data channel OPENED (SCTP association up) ===");
  session_configured = false;
  greeting_triggered = false;

  ESP_LOGI(LOG_TAG, "Sending DATA_CHANNEL_OPEN with label 'oai-events'...");
  int ret = peer_connection_datachannel_open(peer_connection, "oai-events", 0);
  ESP_LOGI(LOG_TAG, "datachannel_open returned: %d", ret);
  ESP_LOGI(LOG_TAG, "Waiting for session.created before sending instructions...");
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

  ESP_LOGI(LOG_TAG, "Creating peer connection (datachannel=STRING, codec=OPUS)...");
  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }
  ESP_LOGI(LOG_TAG, "Peer connection created, registering callbacks...");

  peer_connection_oniceconnectionstatechange(peer_connection,
                                             oai_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, oai_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection, oai_ondatachannel_message,
                                oai_ondatachannel_open,
                                oai_ondatachannel_close);
  ESP_LOGI(LOG_TAG, "Callbacks registered, creating SDP offer...");
  peer_connection_create_offer(peer_connection);
  ESP_LOGI(LOG_TAG, "Entering peer connection loop...");

  while (1) {
    peer_connection_loop(peer_connection);
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
