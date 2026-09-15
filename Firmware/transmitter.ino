/*
  TRANSMITTER FIRMWARE — XIAO ESP32S3 Sense
  ------------------------------------------
  Role: Connects to home WiFi, runs a camera web server that:
    1. Serves a live MJPEG stream at /stream (low-res, for preview)
    2. Serves a single full-resolution JPEG at /capture (on-demand, for saving)

  Board: Seeed XIAO ESP32S3 Sense (built-in OV2640/OV3660 camera)
  Arduino IDE board setting: "XIAO_ESP32S3"
  PSRAM: Must be ENABLED in board settings (Tools > PSRAM > OPI PSRAM)

  Required library: none extra — uses built-in esp_camera.h (comes with
  the ESP32 Arduino core once you select an ESP32-S3 board with camera support)
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// ---------- WiFi credentials ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---------- XIAO ESP32S3 Sense camera pin map ----------
// These are fixed by the module itself — do not change.
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13

WebServer server(80);

// ---------- Camera init ----------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // If PSRAM is available, use higher-res full frame buffers for /capture,
  // and we'll downscale for /stream on the fly via a second lower-res grab.
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;   // 1600x1200 max for capture
    config.jpeg_quality = 10;             // lower number = higher quality
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return false;
  }

  // Optional: drop the live sensor resolution down for smoother streaming,
  // full captures still use full frame size defined above at init.
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, FRAMESIZE_UXGA); // default; stream handler will
                                          // request a smaller frame_size
                                          // dynamically before each capture
  }
  return true;
}

// ---------- /stream handler: MJPEG live preview ----------
// Sends a continuous multipart JPEG stream. Receiver reads this
// frame-by-frame to show a live preview on the TFT.
void handleStream() {
  WiFiClient client = server.client();

  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
  server.sendContent(response);

  sensor_t* s = esp_camera_sensor_get();

  while (client.connected()) {
    // Use a smaller frame size for the live stream to keep it fast/light
    // on both the WiFi link and the receiver's decode/display step.
    if (s) s->set_framesize(s, FRAMESIZE_QVGA); // 320x240

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Stream: camera capture failed");
      break;
    }

    client.print("--frame\r\n");
    client.print("Content-Type: image/jpeg\r\n");
    client.print("Content-Length: " + String(fb->len) + "\r\n\r\n");
    client.write(fb->buf, fb->len);
    client.print("\r\n");

    esp_camera_fb_return(fb);

    if (!client.connected()) break;
    delay(50); // roughly caps stream to ~15-20fps depending on network/frame size
  }
}

// ---------- /capture handler: single full-res JPEG ----------
// Called when the receiver's Capture button is pressed.
void handleCapture() {
  sensor_t* s = esp_camera_sensor_get();
  if (s) s->set_framesize(s, FRAMESIZE_UXGA); // switch to full-res for this shot

  // Discard one frame after resolution change — sensor needs a frame to
  // actually apply the new settings, otherwise you may get a stale/half
  // res frame on the very first capture after a resolution switch.
  camera_fb_t* warm = esp_camera_fb_get();
  if (warm) esp_camera_fb_return(warm);

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    server.send(500, "text/plain", "Camera capture failed");
    return;
  }

  server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  server.setContentLength(fb->len);
  server.send(200, "image/jpeg", "");
  WiFiClient client = server.client();
  client.write(fb->buf, fb->len);

  esp_camera_fb_return(fb);
}

// ---------- /status handler: simple health check ----------
void handleStatus() {
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!initCamera()) {
    Serial.println("FATAL: camera init failed, halting.");
    while (true) delay(1000);
  }
  Serial.println("Camera initialized.");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());
  Serial.println("--> Note this IP, the receiver needs it.");

}