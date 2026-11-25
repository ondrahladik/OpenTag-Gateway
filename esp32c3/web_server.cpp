#include "web_server.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "api_handler.h"
#include "config.h"
#include "config_manager.h"
#include "uart_handler.h"

namespace {
WebServer server(80);

struct FirmwareUploadState {
  bool uartLocked = false;
  bool ready = false;
  bool complete = false;
  bool succeeded = false;
  uint32_t expectedSize = 0;
  uint32_t expectedCrc = 0;
  uint32_t startedAt = 0;
  uint32_t crcState = 0xFFFFFFFFu;
  size_t receivedSize = 0;
  size_t nextProgressLog = 0;
  String error;
};

FirmwareUploadState firmwareUpload;
const char* firmwareHeaders[] = {"X-Tag-MAC", "X-Firmware-Size", "X-Firmware-CRC32"};

void logFirmwareUploadError(const String& error) {
  if (error.isEmpty()) return;
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[OTA tag] ERROR: %s\n", error.c_str());
#endif
}

bool parseHeaderNumber(const String& text, int base, uint32_t& value) {
  if (text.isEmpty()) return false;
  char* end = nullptr;
  const unsigned long parsed = strtoul(text.c_str(), &end, base);
  if (end == text.c_str() || *end != '\0' || parsed > UINT32_MAX) return false;
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool validMac(const String& mac) {
  if (mac.length() != 23) return false;
  for (size_t i = 0; i < 23; ++i) {
    if (i % 3 == 2) {
      if (mac[i] != ':') return false;
    } else if (!isxdigit(static_cast<unsigned char>(mac[i]))) {
      return false;
    }
  }
  return true;
}

uint32_t updateCrc32(uint32_t crc, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
  }
  return crc;
}

static const char ADMIN_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101113"><title>OpenTag Gateway</title>
<style>
:root{color-scheme:dark;--bg:#101113;--panel:#16171b;--line:#2c2d33;--text:#f2f5fa;--muted:#aab4c3;--accent:#f04755;--ok:#52d39a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh;padding:24px 16px;display:grid;place-items:center}
main{width:min(100%,480px);margin:auto}.brand{font-weight:800;font-size:1.3rem;letter-spacing:.02em;text-align:center;margin:0 0 24px}.brand span{color:var(--accent)}
.section{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:clamp(20px,5vw,36px);box-shadow:0 18px 48px #0003}
.field{display:grid;gap:8px;margin:18px 0}label{font-weight:650}input[type=password]{width:100%;border:1px solid #465164;border-radius:10px;background:#0e1219;color:var(--text);padding:13px 14px;font:inherit;outline:none}input:focus{border-color:#f04755;box-shadow:0 0 0 3px #f0475526}.hint{font-size:.875rem;color:var(--muted);margin:0}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap}button{border:0;border-radius:10px;background:var(--accent);color:white;padding:12px 18px;font:700 1rem inherit;cursor:pointer}button:disabled{opacity:.6;cursor:wait}.check{display:flex;gap:10px;align-items:center;color:var(--muted);font-size:.95rem}.check input{width:18px;height:18px;accent-color:var(--accent)}
.notice{min-height:1.5em;margin:12px 0 0;font-size:.92rem;color:var(--ok)}.notice.error{color:#ff8790}
@media(max-width:480px){body{padding:18px 12px}.section{border-radius:14px}.row button{width:100%}}
</style></head>
<body><main><section class="section"><div class="brand">Open<span>Tag</span> Gateway</div>
<form id="form"><div class="field"><label for="token">API token</label><input id="token" name="token" type="password" autocomplete="new-password" maxlength="512" placeholder="Enter a new token"><p class="hint" id="tokenState">Checking token status… An empty field will leave the current token unchanged.</p></div>
<label class="check"><input id="clear" type="checkbox"> Remove saved token</label>
<div class="row" style="margin-top:22px"><button id="save" type="submit">Save settings</button></div><div class="notice" id="notice" role="status" aria-live="polite"></div></form>
<p class="hint" style="margin-top:18px;text-align:center"><a href="/update" style="color:#f04755">Firmware update</a></p>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
async function refresh(){try{const r=await fetch('/api/config',{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();$('tokenState').textContent=d.configured?'Token is saved. It cannot be displayed for security reasons. An empty field will leave the current token unchanged.':'No token is set yet. An empty field will leave it unset.'}catch{$('tokenState').textContent='Device status is unavailable.'}}
$('clear').addEventListener('change',()=>{$('token').disabled=$('clear').checked});
$('form').addEventListener('submit',async e=>{e.preventDefault();const button=$('save'),notice=$('notice');button.disabled=true;notice.className='notice';notice.textContent='Saving…';try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:$('token').value,clear:$('clear').checked})});const d=await r.json();if(!r.ok)throw Error(d.error||'Failed to save settings.');$('token').value='';$('clear').checked=false;$('token').disabled=false;notice.textContent='Settings saved.';await refresh()}catch(err){notice.className='notice error';notice.textContent=err.message}finally{button.disabled=false}});
refresh();setInterval(refresh,5000);
</script></body></html>
)HTML";

static const char UPDATE_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101113"><title>Firmware Update | OpenTag Gateway</title>
<style>
:root{color-scheme:dark;--bg:#101113;--panel:#16171b;--line:#2c2d33;--text:#f2f5fa;--muted:#aab4c3;--accent:#f04755;--ok:#52d39a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh;padding:24px 16px;display:grid;place-items:center}
main{width:min(100%,520px);margin:auto}.section{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:clamp(20px,5vw,36px);box-shadow:0 18px 48px #0003}.brand{font-weight:800;font-size:1.3rem;letter-spacing:.02em;text-align:center;margin:0 0 24px}.brand span{color:var(--accent)}
h1{font-size:1.35rem;margin:0 0 8px}p{color:var(--muted);margin:0 0 18px}.field{display:grid;gap:8px;margin:18px 0}label{font-weight:650}select,input[type=file]{width:100%;border:1px solid #465164;border-radius:10px;background:#0e1219;color:var(--text);padding:13px 14px;font:inherit}select:focus,input:focus{outline:2px solid #f0475580;outline-offset:1px}.hint{font-size:.875rem;color:var(--muted)}button{width:100%;border:0;border-radius:10px;background:var(--accent);color:white;padding:13px 18px;font:700 1rem inherit;cursor:pointer}button:disabled{opacity:.6;cursor:wait}.notice{min-height:1.5em;margin:12px 0 0;color:var(--ok);overflow-wrap:anywhere}.notice.error{color:#ff8790}.back{display:block;color:var(--accent);text-align:center;margin-top:18px}
</style></head>
<body><main><section class="section"><div class="brand">Open<span>Tag</span> Gateway</div>
<h1>Firmware Update</h1><p>Select a tag and upload an OTA file (*-ota.bin). The update will be transferred to the tag at its next check-in.</p>
<form id="updateForm">
<div class="field"><label for="tag">Target tag</label><select id="tag" required><option value="">Loading tags from C6…</option></select></div>
<div class="field"><label for="firmware">OTA firmware (.bin)</label><input id="firmware" type="file" accept=".bin,application/octet-stream" required></div>
<button id="submit" type="submit">Upload and queue update</button><div class="notice" id="notice" role="status" aria-live="polite"></div>
</form><a class="back" href="/">Back to home page</a>
</section></main>
<script>
const $=id=>document.getElementById(id);
function displayTagMac(mac){return String(mac??'').split(':').slice(0,6).join(':')}
function show(message,error=false){$('notice').textContent=message;$('notice').className=error?'notice error':'notice'}
async function loadTags(){const select=$('tag');try{const response=await fetch('/api/config',{cache:'no-store'});if(!response.ok)throw Error();const data=await response.json();select.replaceChildren();if(!data.tags_known){select.add(new Option('C6 has not sent the tag list yet',''));return}if(!data.tags||!data.tags.length){select.add(new Option('C6 has not detected any tags yet',''));return}select.add(new Option('Select a tag',''));for(const tag of data.tags)if(tag.mac)select.add(new Option(displayTagMac(tag.mac),tag.mac))}catch{select.replaceChildren(new Option('Tag list is unavailable',''))}}
function crc32(bytes){let crc=0xffffffff;for(const value of bytes){crc^=value;for(let bit=0;bit<8;bit++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}return(crc^0xffffffff)>>>0}
$('updateForm').addEventListener('submit',async event=>{event.preventDefault();const file=$('firmware').files[0],mac=$('tag').value,button=$('submit');if(!file||!mac)return;const min=8192,max=180224;if(!file.name.toLowerCase().endsWith('.bin')){show('Select a .bin file.',true);return}if(file.size<min||file.size>max){show('OTA BIN size must be between '+min+' and '+max+' B.',true);return}button.disabled=true;show('Verifying file and preparing transfer…');try{const checksum=crc32(new Uint8Array(await file.arrayBuffer())).toString(16).padStart(8,'0');const request=new XMLHttpRequest();request.open('POST','/api/firmware');request.setRequestHeader('Content-Type','application/octet-stream');request.setRequestHeader('X-Tag-MAC',mac);request.setRequestHeader('X-Firmware-Size',String(file.size));request.setRequestHeader('X-Firmware-CRC32',checksum);request.upload.onprogress=e=>{if(e.lengthComputable)show('Uploading to GW: '+Math.round(e.loaded*100/e.total)+' %.')};request.onload=()=>{let data={};try{data=JSON.parse(request.responseText)}catch{}if(request.status<200||request.status>=300)show(data.error||'Failed to queue firmware.',true);else show(data.message||'Firmware queued. The tag will download it at its next check-in.');button.disabled=false};request.onerror=()=>{show('Connection to GW was interrupted during upload.',true);button.disabled=false};request.send(file)}catch(error){show(error.message||'Failed to read the file.',true);button.disabled=false}});
loadTags();
</script></body></html>
)HTML";

void sendJson(int statusCode, JsonDocument& document) {
  String body;
  serializeJson(document, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

void finishFirmwareUpload(bool bodyComplete) {
  firmwareUpload.complete = bodyComplete;
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[OTA tag] HTTP body %s: %lu/%lu bytes received.\n",
                bodyComplete ? "complete" : "aborted",
                static_cast<unsigned long>(firmwareUpload.receivedSize),
                static_cast<unsigned long>(firmwareUpload.expectedSize));
#endif
  if (!firmwareUpload.uartLocked) {
    if (!firmwareUpload.error.isEmpty()) logFirmwareUploadError(firmwareUpload.error);
    return;
  }

  DynamicJsonDocument response(512);
  const bool gotResponse = c6UartWaitResponseLocked(C6_REPLY_IMAGE_QUEUED, response, 65000);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.printf("[OTA tag] C6 queue response: %s after %lu ms.\n",
                gotResponse ? "received" : "timeout",
                static_cast<unsigned long>(millis() - firmwareUpload.startedAt));
#endif
  if (!gotResponse) {
    if (firmwareUpload.error.isEmpty()) firmwareUpload.error = "ESP32-C6 did not confirm the firmware transfer.";
  } else if (!response["ok"].as<bool>()) {
    firmwareUpload.error = String("ESP32-C6 rejected the firmware: ") + String(response["error"] | "unknown reason");
  } else if (!firmwareUpload.error.isEmpty()) {
    firmwareUpload.succeeded = false;
  } else if (!bodyComplete || firmwareUpload.receivedSize != firmwareUpload.expectedSize) {
    firmwareUpload.error = "Firmware upload was incomplete.";
  } else if ((firmwareUpload.crcState ^ 0xFFFFFFFFu) != firmwareUpload.expectedCrc) {
    firmwareUpload.error = "Firmware upload checksum mismatch.";
  } else {
    firmwareUpload.succeeded = true;
  }
  c6UartUnlock();
  firmwareUpload.uartLocked = false;
  firmwareUpload.ready = false;
  if (firmwareUpload.succeeded) {
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA tag] Firmware queued successfully in %lu ms.\n",
                  static_cast<unsigned long>(millis() - firmwareUpload.startedAt));
#endif
  } else {
    logFirmwareUploadError(firmwareUpload.error);
  }
}

void handleFirmwareRaw() {
  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    firmwareUpload = FirmwareUploadState{};
    firmwareUpload.startedAt = millis();
    const String mac = server.header("X-Tag-MAC");
    const int contentLength = server.clientContentLength();
    uint32_t requestedSize = 0;
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA tag] HTTP upload started; content length=%d bytes.\n", contentLength);
#endif
    if (!validMac(mac) || !parseHeaderNumber(server.header("X-Firmware-Size"), 10, requestedSize) ||
        !parseHeaderNumber(server.header("X-Firmware-CRC32"), 16, firmwareUpload.expectedCrc)) {
      firmwareUpload.error = "Firmware metadata is missing or invalid.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
    if (requestedSize < MIN_TAG_FIRMWARE_BYTES || requestedSize > MAX_TAG_FIRMWARE_BYTES ||
        contentLength < 0 || static_cast<uint32_t>(contentLength) != requestedSize) {
      firmwareUpload.error = "Invalid OTA BIN size.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
    firmwareUpload.expectedSize = requestedSize;
    firmwareUpload.nextProgressLog = 16 * 1024;
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA tag] Metadata valid; firmware size=%lu bytes, waiting for C6 UART.\n",
                  static_cast<unsigned long>(requestedSize));
#endif

    DynamicJsonDocument header(192);
    header["cmd"] = "put_firmware";
    header["mac"] = mac;
    header["size"] = requestedSize;
    header["crc32"] = firmwareUpload.expectedCrc;
    String headerLine;
    serializeJson(header, headerLine);

    if (!c6UartLock(30000)) {
      firmwareUpload.error = "ESP32-C6 UART is currently busy.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
    firmwareUpload.uartLocked = true;
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.println("[OTA tag] UART locked; sending put_firmware command.");
#endif
    DynamicJsonDocument response(512);
    if (!c6UartCommandLocked(headerLine, C6_REPLY_IMAGE_READY, response, 30000)) {
      firmwareUpload.error = "ESP32-C6 did not respond to the OTA request.";
      logFirmwareUploadError(firmwareUpload.error);
      c6UartUnlock();
      firmwareUpload.uartLocked = false;
      return;
    }
    if (!response["ok"].as<bool>() || response["state"] != "ready") {
      firmwareUpload.error = String("ESP32-C6 rejected OTA: ") + String(response["error"] | "unknown reason");
      logFirmwareUploadError(firmwareUpload.error);
      c6UartUnlock();
      firmwareUpload.uartLocked = false;
      return;
    }
    firmwareUpload.ready = true;
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA tag] C6 ready; streaming %lu firmware bytes.\n",
                  static_cast<unsigned long>(firmwareUpload.expectedSize));
#endif
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!firmwareUpload.ready || firmwareUpload.error.length()) return;
    if (firmwareUpload.receivedSize > firmwareUpload.expectedSize ||
        raw.currentSize > firmwareUpload.expectedSize - firmwareUpload.receivedSize) {
      firmwareUpload.error = "Received data exceeded the declared size.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
    firmwareUpload.crcState = updateCrc32(firmwareUpload.crcState, raw.buf, raw.currentSize);
    firmwareUpload.receivedSize += raw.currentSize;
    if (!c6UartSendImageLocked(raw.buf, raw.currentSize)) {
      firmwareUpload.error = "Failed to forward data to ESP32-C6.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
#if ARDUINO_USB_CDC_ON_BOOT
    if (firmwareUpload.receivedSize >= firmwareUpload.nextProgressLog ||
        firmwareUpload.receivedSize == firmwareUpload.expectedSize) {
      Serial.printf("[OTA tag] UART transfer: %lu/%lu bytes (%u%%), %lu ms.\n",
                    static_cast<unsigned long>(firmwareUpload.receivedSize),
                    static_cast<unsigned long>(firmwareUpload.expectedSize),
                    static_cast<unsigned>(
                        firmwareUpload.receivedSize * 100u / firmwareUpload.expectedSize),
                    static_cast<unsigned long>(millis() - firmwareUpload.startedAt));
      firmwareUpload.nextProgressLog = firmwareUpload.receivedSize + 16 * 1024;
    }
#endif
    return;
  }

  if (raw.status == RAW_END) {
    finishFirmwareUpload(true);
    return;
  }
  if (raw.status == RAW_ABORTED) {
    if (firmwareUpload.error.isEmpty()) firmwareUpload.error = "Connection was interrupted during firmware upload.";
    logFirmwareUploadError(firmwareUpload.error);
    finishFirmwareUpload(false);
  }
}

void handleFirmwarePost() {
  DynamicJsonDocument response(384);
  if (!firmwareUpload.succeeded) {
    response["error"] = firmwareUpload.error.isEmpty() ? "Failed to forward the firmware." : firmwareUpload.error;
    sendJson(400, response);
    return;
  }
  response["ok"] = true;
  response["message"] = "Firmware was queued on C6. The tag will download it at its next check-in.";
  sendJson(200, response);
}

void handleConfigurationGet() {
  DynamicJsonDocument response(4096);
  response["configured"] = configurationHasApiToken();
  response["masked"] = configurationHasApiToken() ? "••••••••" : "";
  apiHandlerAppendStatus(response);
  response["wifi"] = WiFi.status() == WL_CONNECTED;
  response["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "disconnected";
  if (WiFi.status() == WL_CONNECTED) response["wifi_rssi_dbm"] = WiFi.RSSI();
  else response["wifi_rssi_dbm"] = nullptr;
  sendJson(200, response);
}

void handleConfigurationPost() {
  DynamicJsonDocument request(1024);
  if (deserializeJson(request, server.arg("plain"))) {
    DynamicJsonDocument response(128);
    response["error"] = "Invalid request JSON.";
    sendJson(400, response);
    return;
  }
  const String token = request["token"] | "";
  const bool clearToken = request["clear"] | false;
  String error;
  if (!configurationSetApiToken(token, clearToken, error)) {
    DynamicJsonDocument response(192);
    response["error"] = error;
    sendJson(400, response);
    return;
  }
  DynamicJsonDocument response(128);
  response["ok"] = true;
  response["configured"] = configurationHasApiToken();
  sendJson(200, response);
}

void webServerTask(void*) {
  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}

bool webServerBegin() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", ADMIN_PAGE);
  });
  server.on("/update", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", UPDATE_PAGE);
  });
  server.on("/api/config", HTTP_GET, handleConfigurationGet);
  server.on("/api/config", HTTP_POST, handleConfigurationPost);
  server.collectHeaders(firmwareHeaders, sizeof(firmwareHeaders) / sizeof(firmwareHeaders[0]));
  server.on("/api/firmware", HTTP_POST, handleFirmwarePost, handleFirmwareRaw);
  server.onNotFound([]() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(404, "text/plain; charset=utf-8", "Not found.");
  });
  server.begin();
  return xTaskCreate(webServerTask, "web-server", 12288, nullptr, 1, nullptr) == pdPASS;
}
