/*
  RECEIVER FIRMWARE — ESP32-C3 SuperMini
  ---------------------------------------
  Role: Connects to home WiFi, pulls the live MJPEG stream from the
  transmitter and shows it on the ST7735 TFT. On button press:
    - Capture: requests a full-res JPEG from transmitter, saves to SD
    - Gallery: cycles through saved JPEGs on SD, displays them
    - Delete:  deletes the currently-viewed gallery image

  Board: ESP32-C3 SuperMini
  Libraries required (install via Library Manager):
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - ArduinoJson (not required here, omitted)
    - SD (built-in) 

  Pin map (finalized earlier in the project):
    SPI_SCK   -> GPIO4   (shared TFT + SD)
    SPI_MOSI  -> GPIO6   (shared TFT + SD)
    SPI_MISO  -> GPIO5   (SD only)
    TFT_CS    -> GPIO7
    SD_CS     -> GPIO10
    TFT_DC    -> GPIO3
    TFT_RST   -> GPIO1
    BTN_CAPTURE -> GPIO11
    BTN_GALLERY -> GPIO2
    BTN_DELETE  -> GPIO9
*/

#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ---------- WiFi credentials (must match transmitter's network) ----------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ---------- Transmitter address ----------
// Set this to the IP address printed by the transmitter's Serial Monitor
// on boot. For a permanent build, consider giving the transmitter a
// static IP/DHCP reservation on your router so this never changes.
const char* TRANSMITTER_IP = "192.168.1.50";
const uint16_t TRANSMITTER_PORT = 80;

// ---------- Pin map ----------
#define TFT_CS    7
#define TFT_DC    3
#define TFT_RST   1
#define SD_CS     10
#define SPI_SCK   4
#define SPI_MOSI  6
#define SPI_MISO  5

#define BTN_CAPTURE 11
#define BTN_GALLERY 2
#define BTN_DELETE  9

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// ---------- App state ----------
enum Mode { MODE_LIVE, MODE_GALLERY };
Mode currentMode = MODE_LIVE;

int galleryIndex = 0;
String galleryFiles[200]; // adjust size if you expect more photos
int galleryCount = 0;

unsigned long lastDebounce[3] = {0, 0, 0};
const unsigned long DEBOUNCE_MS = 250;

// ---------- Forward declarations ----------
void connectWiFi();
bool initSD();
void showLiveFrame();
void doCapture();
void loadGalleryFileList();
void showGalleryImage(int index);
void doDelete();
bool buttonPressed(uint8_t pin, int idx);

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(BTN_CAPTURE, INPUT_PULLUP);
  pinMode(BTN_GALLERY, INPUT_PULLUP);
  pinMode(BTN_DELETE, INPUT_PULLUP);

  // Shared SPI bus setup
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB); // common default for 128x160 ST7735 modules;
                              // if colors look wrong/inverted, try
                              // INITR_GREENTAB or INITR_REDTAB instead
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(0, 0);
  tft.println("Booting...");

  if (!initSD()) {
    tft.println("SD init FAILED");
    Serial.println("SD init failed — check wiring/card");
  } else {
    tft.println("SD OK");
  }

  connectWiFi();

  tft.fillScreen(ST77XX_BLACK);
}

void loop() {
  // ---------- Button handling ----------
  if (buttonPressed(BTN_CAPTURE, 0)) {
    doCapture();
  }

  if (buttonPressed(BTN_GALLERY, 1)) {
    if (currentMode == MODE_LIVE) {
      loadGalleryFileList();
      if (galleryCount > 0) {
        currentMode = MODE_GALLERY;
        galleryIndex = galleryCount - 1; // show most recent first
        showGalleryImage(galleryIndex);
      } else {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(0, 0);
        tft.println("No photos saved");
        delay(1000);
      }
    } else {
      // cycle to next image in gallery mode
      galleryIndex++;
      if (galleryIndex >= galleryCount) galleryIndex = 0;
      showGalleryImage(galleryIndex);
    }
  }

  if (buttonPressed(BTN_DELETE, 2)) {
    if (currentMode == MODE_GALLERY && galleryCount > 0) {
      doDelete();
    }
  }

  // ---------- Live preview ----------
  if (currentMode == MODE_LIVE) {
    showLiveFrame();
  }
}

// ---------- Button debounce helper ----------
bool buttonPressed(uint8_t pin, int idx) {
  if (digitalRead(pin) == LOW) {
    unsigned long now = millis();
    if (now - lastDebounce[idx] > DEBOUNCE_MS) {
      lastDebounce[idx] = now;
      return true;
    }
  }
  return false;
}

// ---------- WiFi ----------
void connectWiFi() {
  tft.println("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    tft.println("WiFi OK");
  } else {
    Serial.println();
    Serial.println("WiFi connect FAILED");
    tft.println("WiFi FAILED");
  }
}

// ---------- SD ----------
bool initSD() {
  // SD shares the SPI bus with the TFT; SD.begin needs its own CS pin.
  return SD.begin(SD_CS);
}

// ---------- Live preview: pull one frame from /stream-like single-shot ----------
// NOTE: True MJPEG multipart parsing (continuously reading the boundary-
// delimited stream) is more involved. For a first working version, this
// requests a single JPEG frame at a time from the transmitter's /capture
// endpoint at reduced size, which is simpler to implement reliably on a
// resource-constrained C3. This trades a little smoothness for reliability.
// Once this is working end-to-end, swap in true multipart stream parsing
// for smoother live video if desired.
void showLiveFrame() {
  HTTPClient http;
  String url = String("http://") + TRANSMITTER_IP + ":" + TRANSMITTER_PORT + "/capture";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    int len = http.getSize();
    WiFiClient* stream = http.getStreamPtr();

    // Adafruit_ST7735 has no built-in JPEG decoder — you need a JPEG
    // decode library (e.g. JPEGDecoder or TJpg_Decoder) to actually draw
    // this onto the TFT. That decode step is the missing piece to fill
    // in during the next testing pass, once your hardware is in hand
    // (decoder buffer sizing depends on real free RAM you observe).
    //
    // Placeholder for now: read+discard the stream so the connection
    // completes cleanly, and show a status line instead of the image.
    uint8_t buff[512];
    int totalRead = 0;
    while (http.connected() && totalRead < len) {
      size_t avail = stream->available();
      if (avail) {
        int c = stream->readBytes(buff, min((size_t)sizeof(buff), avail));
        totalRead += c;
      }
      delay(1);
    }

    tft.fillRect(0, 0, 160, 10, ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(ST77XX_GREEN);
    tft.print("Live: frame ");
    tft.print(totalRead);
    tft.println(" bytes");
  } else {
    tft.fillRect(0, 0, 160, 10, ST77XX_BLACK);
    tft.setCursor(0, 0);
    tft.setTextColor(ST77XX_RED);
    tft.print("Stream err ");
    tft.println(httpCode);
  }
  http.end();
}

// ---------- Capture: fetch full-res JPEG, save to SD ----------
void doCapture() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("Capturing...");

  HTTPClient http;
  String url = String("http://") + TRANSMITTER_IP + ":" + TRANSMITTER_PORT + "/capture";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    tft.println("Capture failed");
    Serial.printf("Capture HTTP error: %d\n", httpCode);
    http.end();
    delay(1000);
    return;
  }

  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();

  // Build a simple incrementing filename: img_0001.jpg, img_0002.jpg, ...
  int fileIndex = 1;
  String filename;
  do {
    filename = "/img_" + String(fileIndex) + ".jpg";
    fileIndex++;
  } while (SD.exists(filename));

  File f = SD.open(filename, FILE_WRITE);
  if (!f) {
    tft.println("SD open failed");
    http.end();
    delay(1000);
    return;
  }

  uint8_t buff[512];
  int totalWritten = 0;
  while (http.connected() && totalWritten < len) {
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buff, min((size_t)sizeof(buff), avail));
      f.write(buff, c);
      totalWritten += c;
    }
    delay(1);
  }
  f.close();
  http.end();

  tft.println("Saved:");
  tft.println(filename);
  tft.print(totalWritten);
  tft.println(" bytes");
  delay(1000);
  tft.fillScreen(ST77XX_BLACK);
}

// ---------- Gallery: list files ----------
void loadGalleryFileList() {
  galleryCount = 0;
  File root = SD.open("/");
  if (!root) return;

  File entry = root.openNextFile();
  while (entry && galleryCount < 200) {
    String name = entry.name();
    if (name.endsWith(".jpg") || name.endsWith(".JPG")) {
      galleryFiles[galleryCount] = "/" + name;
      galleryCount++;
    }
    entry = root.openNextFile();
  }
  root.close();
}

// ---------- Gallery: show a saved image ----------
// NOTE: same as live preview, actually decoding+drawing the JPEG onto the
// TFT requires a JPEG decoder library (e.g. JPEGDecoder). This shows the
// filename/size as a placeholder until that decode step is wired in.
void showGalleryImage(int index) {
  if (index < 0 || index >= galleryCount) return;

  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("Gallery:");
  tft.println(galleryFiles[index]);

  File f = SD.open(galleryFiles[index]);
  if (f) {
    tft.print(f.size());
    tft.println(" bytes");
    f.close();
  }

  tft.setCursor(0, 100);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print(index + 1);
  tft.print(" / ");
  tft.println(galleryCount);
}

// ---------- Delete current gallery image ----------
void doDelete() {
  if (galleryIndex < 0 || galleryIndex >= galleryCount) return;

  String toDelete = galleryFiles[galleryIndex];
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextColor(ST77XX_RED);
  tft.println("Deleting:");
  tft.println(toDelete);

  if (SD.remove(toDelete)) {
    tft.println("Deleted.");
  } else {
    tft.println("Delete failed.");
  }
  delay(800);
