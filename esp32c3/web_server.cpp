#include "web_server.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_system.h>
#include <stdio.h>
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
  int statusCode = 400;
  uint32_t expectedSize = 0;
  uint32_t expectedCrc = 0;
  uint32_t startedAt = 0;
  uint32_t crcState = 0xFFFFFFFFu;
  size_t receivedSize = 0;
  size_t nextProgressLog = 0;
  String error;
};

struct GatewayFirmwareUploadState {
  bool started = false;
  bool uartLocked = false;
  bool ready = false;
  bool succeeded = false;
  uint32_t expectedSize = 0;
  uint32_t expectedCrc = 0;
  uint32_t crcState = 0xFFFFFFFFu;
  size_t receivedSize = 0;
  size_t nextProgressLog = 16 * 1024;
  int statusCode = 400;
  String error;
};

FirmwareUploadState firmwareUpload;
GatewayFirmwareUploadState c3FirmwareUpload;
GatewayFirmwareUploadState c6FirmwareUpload;
constexpr uint32_t UPDATE_SESSION_DURATION_MS = 60u * 60u * 1000u;
constexpr uint8_t MAX_UPDATE_LOGIN_FAILURES = 5;
constexpr uint32_t UPDATE_LOGIN_LOCKOUT_MS = 60u * 1000u;
constexpr char UPDATE_SESSION_COOKIE_NAME[] = "opentag_update";
String updateSessionToken;
uint32_t updateSessionExpiresAt = 0;
uint32_t updateLoginBlockedUntil = 0;
uint8_t updateLoginFailures = 0;
const char* firmwareHeaders[] = {
    "X-Tag-MAC", "X-Firmware-Size", "X-Firmware-CRC32", "Authorization", "Cookie"};

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

bool authorizedWithSavedApiToken() {
  String token;
  if (!configurationReadApiToken(token)) return false;
  return !token.isEmpty() && server.header("Authorization") == String("Bearer ") + token;
}

String requestUpdateSessionCookie() {
  const String cookies = server.header("Cookie");
  const String cookiePrefix = String(UPDATE_SESSION_COOKIE_NAME) + "=";
  size_t position = 0;
  while (position < cookies.length()) {
    while (position < cookies.length() &&
           (cookies[position] == ';' || isspace(static_cast<unsigned char>(cookies[position]))))
      ++position;
    const int separator = cookies.indexOf(';', position);
    const size_t end = separator < 0 ? cookies.length() : static_cast<size_t>(separator);
    const String pair = cookies.substring(position, end);
    if (pair.startsWith(cookiePrefix)) return pair.substring(cookiePrefix.length());
    if (separator < 0) break;
    position = end + 1;
  }
  return "";
}

bool updateSessionAuthorized() {
  if (updateSessionToken.isEmpty()) return false;
  if (static_cast<int32_t>(millis() - updateSessionExpiresAt) >= 0) {
    updateSessionToken = "";
    updateSessionExpiresAt = 0;
    return false;
  }
  return requestUpdateSessionCookie() == updateSessionToken;
}

void clearUpdateSession() {
  updateSessionToken = "";
  updateSessionExpiresAt = 0;
}

String createUpdateSessionToken() {
  char token[33];
  for (size_t i = 0; i < 4; ++i)
    snprintf(token + i * 8, 9, "%08lX", static_cast<unsigned long>(esp_random()));
  return String(token);
}

void setGatewayFirmwareError(GatewayFirmwareUploadState& state, int statusCode, const String& error) {
  state.statusCode = statusCode;
  state.error = error;
}

static const char ADMIN_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101113"><title>OpenTag Gateway</title>
<style>
:root{color-scheme:dark;--bg:#101113;--panel:#16171b;--line:#2c2d33;--text:#f2f5fa;--muted:#aab4c3;--accent:#f04755;--ok:#52d39a}
*{box-sizing:border-box}[hidden]{display:none!important}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh;padding:24px 16px;display:grid;place-items:center}
main{width:min(100%,520px);margin:auto}.brand{font-weight:800;font-size:1.3rem;letter-spacing:.02em;text-align:center;margin:0 0 24px}.brand span{color:var(--accent)}
.section{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:clamp(20px,5vw,36px);box-shadow:0 18px 48px #0003}
.field{display:grid;gap:8px;margin:18px 0}.field[hidden]{display:none}label{font-weight:650}input[type=password],select,input[type=file]{width:100%;border:1px solid #465164;border-radius:10px;background:#0e1219;color:var(--text);padding:13px 14px;font:inherit}
input:focus,select:focus{outline:none;border-color:var(--accent)}.hint{font-size:.875rem;color:var(--muted);margin:0}h1{font-size:1.35rem;margin:0 0 8px}p{color:var(--muted);margin:0 0 18px}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:18px}button{width:100%;border:0;border-radius:10px;background:var(--accent);color:white;padding:12px 18px;font:700 1rem system-ui,-apple-system,"Segoe UI",sans-serif;cursor:pointer}button:disabled{opacity:.6;cursor:wait}.check{display:flex;gap:10px;align-items:center;color:var(--muted);font-size:.95rem}.check input{width:18px;height:18px;accent-color:var(--accent)}
.notice{min-height:1.5em;margin:12px 0 0;font-size:.92rem;color:var(--ok);overflow-wrap:anywhere}.notice.error{color:#ff8790}.back{display:block;color:var(--accent);text-align:center;margin-top:18px;font-size:.875rem}
@media(max-width:520px){body{padding:18px 12px}.section{border-radius:14px}}
</style></head>
<body><main><section class="section"><div class="brand">Open<span>Tag</span> Gateway</div>
<p class="hint" id="settingsLoading">Loading settings…</p>
<form id="form" hidden><div class="field"><label for="token">API token</label><input id="token" name="token" type="password" autocomplete="new-password" maxlength="512" placeholder="Enter API token"><p class="hint" id="tokenState"></p></div>
<section id="updatePasswordFields" hidden>
<div class="field"><label for="updatePassword">New update password</label><input id="updatePassword" type="password" autocomplete="new-password" minlength="8" maxlength="128"><p class="hint">Use 8 to 128 characters.</p></div>
<div class="field"><label for="updatePasswordConfirm">Confirm update password</label><input id="updatePasswordConfirm" type="password" autocomplete="new-password" minlength="8" maxlength="128"></div>
<p class="hint" id="updatePasswordState"></p>
</section>
<div class="row"><button id="save" type="submit">Save settings</button></div><div class="notice" id="notice" role="status" aria-live="polite"></div></form>
<p class="hint" style="margin-top:18px;text-align:center"><a href="/update" style="color:#f04755">Firmware update</a></p>
</section>
</main>
<script>
const $=id=>document.getElementById(id);
let settingsLoaded=false;
async function refresh(){try{const r=await fetch('/api/config',{cache:'no-store'});if(!r.ok)throw Error();const d=await r.json();$('tokenState').textContent=d.configured?'Token is saved. Enter a new token to replace it, leave the field blank to keep it unchanged.':'No token is set yet. Enter a token to configure it.';$('updatePasswordFields').hidden=!d.configured;$('updatePasswordState').textContent=(d.update_password_configured?'An update password is configured. You can set a new one below. ':'No update password is set yet. ')+'To save an update password, enter the saved API token above. Leave the field blank to keep it unchanged.';$('form').hidden=false;$('settingsLoading').hidden=true;settingsLoaded=true}catch{if(!settingsLoaded)$('settingsLoading').textContent='Device status is unavailable.'}}
$('form').addEventListener('submit',async e=>{e.preventDefault();const button=$('save'),notice=$('notice'),token=$('token').value.trim(),password=$('updatePassword').value,confirmPassword=$('updatePasswordConfirm').value;button.disabled=true;notice.className='notice';notice.textContent='Saving…';let tokenSaved=false;try{if(password!==confirmPassword)throw Error('The update passwords do not match.');const tokenResponse=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token})});const tokenResult=await tokenResponse.json();if(!tokenResponse.ok)throw Error(tokenResult.error||'Failed to save API token.');tokenSaved=Boolean(token);if(password){const headers={'Content-Type':'application/json'};if(token)headers.Authorization='Bearer '+token;const passwordResponse=await fetch('/api/update/password',{method:'POST',headers,body:JSON.stringify({password})});const passwordResult=await passwordResponse.json();if(!passwordResponse.ok)throw Error(passwordResult.error||'Failed to save update password.')}$('token').value='';$('updatePassword').value='';$('updatePasswordConfirm').value='';notice.textContent='Settings saved.';await refresh()}catch(err){notice.className='notice error';notice.textContent=(tokenSaved?'API token saved, but update password was not: ':'')+err.message}finally{button.disabled=false}});
refresh();setInterval(refresh,5000);
</script></body></html>
)HTML";

static const char UPDATE_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#101113"><title>Firmware Update</title>
<style>
:root{color-scheme:dark;--bg:#101113;--panel:#16171b;--line:#2c2d33;--text:#f2f5fa;--muted:#aab4c3;--accent:#f04755;--ok:#52d39a}
*{box-sizing:border-box}[hidden]{display:none!important}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;min-height:100vh;padding:24px 16px;display:grid;place-items:center}
main{width:min(100%,520px);margin:auto}.brand{font-weight:800;font-size:1.3rem;letter-spacing:.02em;text-align:center;margin:0 0 24px}.brand span{color:var(--accent)}
.section{background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:clamp(20px,5vw,36px);box-shadow:0 18px 48px #0003}
.field{display:grid;gap:8px;margin:18px 0}.field[hidden]{display:none}label{font-weight:650}input[type=password],select,input[type=file]{width:100%;border:1px solid #465164;border-radius:10px;background:#0e1219;color:var(--text);padding:13px 14px;font:inherit}
input:focus,select:focus{outline:none;border-color:var(--accent)}.hint{font-size:.875rem;color:var(--muted);margin:0}h1{font-size:1.35rem;margin:0 0 8px}p{color:var(--muted);margin:0 0 18px}
.row{display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-top:18px}button{width:100%;border:0;border-radius:10px;background:var(--accent);color:white;padding:12px 18px;font:700 1rem system-ui,-apple-system,"Segoe UI",sans-serif;cursor:pointer}button:disabled{opacity:.6;cursor:wait}.check{display:flex;gap:10px;align-items:center;color:var(--muted);font-size:.95rem}.check input{width:18px;height:18px;accent-color:var(--accent)}
.notice{min-height:1.5em;margin:12px 0 0;font-size:.92rem;color:var(--ok);overflow-wrap:anywhere}.notice.error{color:#ff8790}.back{display:block;color:var(--accent);text-align:center;margin-top:18px;font-size:.875rem}.footer-links{display:flex;justify-content:center;gap:18px;margin-top:18px}.footer-links .back{margin-top:0}
@media(max-width:520px){body{padding:18px 12px}.section{border-radius:14px}}
</style></head>
<body><main><section class="section"><div class="brand">Open<span>Tag</span> Gateway</div>
<div id="loginPanel"><h1>Firmware Update</h1><p>Sign in with the update password configured on the home page.</p>
<form id="loginForm"><div class="field"><label for="loginPassword">Update password</label><input id="loginPassword" type="password" autocomplete="current-password" minlength="8" maxlength="128" required></div>
<button id="loginSubmit" type="submit">Sign in</button><div class="notice error" id="loginNotice" role="status" aria-live="polite"></div></form>
<a class="back" href="/">Back to home page</a></div>
<div id="updatePanel" hidden><h1>Firmware Update</h1>
<form id="c3UpdateForm">
<div class="field"><label for="c3Firmware">ESP32-C3 firmware (.bin)</label><input id="c3Firmware" type="file" accept=".bin,application/octet-stream" required></div>
<button id="c3Submit" type="submit">Update C3 firmware</button><div class="notice" id="c3Notice" role="status" aria-live="polite"></div>
</form>
<form id="c6UpdateForm">
<div class="field"><label for="c6Firmware">ESP32-C6 firmware (.bin)</label><input id="c6Firmware" type="file" accept=".bin,application/octet-stream" required></div>
<button id="c6Submit" type="submit">Update C6 firmware</button><div class="notice" id="c6Notice" role="status" aria-live="polite"></div>
</form>
<form id="updateForm">
<div class="field"><label for="tag">Target tag</label><select id="tag" required><option value="">Loading tags from C6…</option></select></div>
<div class="field"><label for="firmware">Tag firmware (.bin)</label><input id="firmware" type="file" accept=".bin,application/octet-stream" required></div>
<button id="submit" type="submit">Upload and queue update</button><div class="notice" id="notice" role="status" aria-live="polite"></div>
</form><div class="footer-links"><a class="back" href="#" id="logout">Sign out</a><a class="back" href="/">Back to home page</a></div>
</div>
</section></main>
<script>
const $=id=>document.getElementById(id);
function displayTagMac(mac){return String(mac??'').split(':').slice(0,6).join(':')}
function show(message,error=false){$('notice').textContent=message;$('notice').className=error?'notice error':'notice'}
async function checkUpdateSession(){try{const response=await fetch('/api/update/session',{cache:'no-store'});if(!response.ok)throw Error();const state=await response.json();$('loginPanel').hidden=state.authenticated;if(state.authenticated){$('updatePanel').hidden=false;if(!$('tag').dataset.loaded){$('tag').dataset.loaded='true';loadTags()}}else{$('updatePanel').hidden=true;$('loginNotice').textContent=state.password_configured?'':'Set an update password on the home page before signing in.'}}catch{$('loginPanel').hidden=false;$('updatePanel').hidden=true;$('loginNotice').textContent='Unable to check update access. Try again.'}}
async function loadTags(){const select=$('tag');try{const response=await fetch('/api/config',{cache:'no-store'});if(!response.ok)throw Error();const data=await response.json();select.replaceChildren();if(!data.tags_known){select.add(new Option('C6 has not sent the tag list yet',''));return}if(!data.tags||!data.tags.length){select.add(new Option('C6 has not detected any tags yet',''));return}select.add(new Option('Select a tag',''));for(const tag of data.tags)if(tag.mac)select.add(new Option(displayTagMac(tag.mac),tag.mac))}catch{select.replaceChildren(new Option('Tag list is unavailable',''))}}
function crc32(bytes){let crc=0xffffffff;for(const value of bytes){crc^=value;for(let bit=0;bit<8;bit++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}return(crc^0xffffffff)>>>0}
$('loginForm').addEventListener('submit',async event=>{event.preventDefault();const button=$('loginSubmit'),notice=$('loginNotice');button.disabled=true;notice.textContent='Signing in…';try{const response=await fetch('/api/update/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:$('loginPassword').value})});const data=await response.json();if(!response.ok)throw Error(data.error||'Sign-in failed.');$('loginPassword').value='';notice.textContent='';await checkUpdateSession()}catch(error){notice.textContent=error.message||'Sign-in failed.'}finally{button.disabled=false}});
$('logout').addEventListener('click',async event=>{event.preventDefault();try{await fetch('/api/update/logout',{method:'POST'});}finally{$('tag').dataset.loaded='';$('loginPassword').value='';await checkUpdateSession()}});
$('updateForm').addEventListener('submit',async event=>{event.preventDefault();const file=$('firmware').files[0],mac=$('tag').value,button=$('submit');if(!file||!mac)return;const min=8192,max=180224;if(!file.name.toLowerCase().endsWith('.bin')){show('Select a .bin file.',true);return}if(file.size<min||file.size>max){show('OTA BIN size must be between '+min+' and '+max+' B.',true);return}button.disabled=true;show('Verifying file and preparing transfer…');try{const checksum=crc32(new Uint8Array(await file.arrayBuffer())).toString(16).padStart(8,'0');const request=new XMLHttpRequest();request.open('POST','/api/firmware');request.setRequestHeader('Content-Type','application/octet-stream');request.setRequestHeader('X-Tag-MAC',mac);request.setRequestHeader('X-Firmware-Size',String(file.size));request.setRequestHeader('X-Firmware-CRC32',checksum);request.upload.onprogress=e=>{if(e.lengthComputable)show('Uploading to GW: '+Math.round(e.loaded*100/e.total)+' %.')};request.onload=()=>{let data={};try{data=JSON.parse(request.responseText)}catch{}if(request.status<200||request.status>=300)show(data.error||'Failed to queue firmware.',true);else show(data.message||'Firmware queued. The tag will download it at its next check-in.');button.disabled=false};request.onerror=()=>{show('Connection to GW was interrupted during upload.',true);button.disabled=false};request.send(file)}catch(error){show(error.message||'Failed to read the file.',true);button.disabled=false}});
function uploadGatewayFirmware(board){const prefix=board.toUpperCase(),file=$(''+board+'Firmware').files[0],button=$(board+'Submit'),notice=$(board+'Notice');const report=(message,error=false)=>{notice.textContent=message;notice.className=error?'notice error':'notice'};if(!file){report('Select a firmware .bin file.',true);return}if(!file.name.toLowerCase().endsWith('.bin')||file.size<8192){report('Select a valid firmware .bin file.',true);return}button.disabled=true;let uploadComplete=false;report('Preparing firmware upload…');(async()=>{try{const request=new XMLHttpRequest();request.open('POST','/api/gateway-firmware/'+board);request.setRequestHeader('Content-Type','application/octet-stream');if(board==='c6'){const checksum=crc32(new Uint8Array(await file.arrayBuffer())).toString(16).padStart(8,'0');request.setRequestHeader('X-Firmware-CRC32',checksum)}request.timeout=300000;request.upload.onprogress=event=>{if(event.lengthComputable)report('Uploading to ESP32-'+prefix+': '+Math.round(event.loaded*100/event.total)+' %.')};request.upload.onload=()=>{uploadComplete=true};request.onload=()=>{let data={};try{data=JSON.parse(request.responseText)}catch{}if(request.status<200||request.status>=300)report(data.error||'Gateway firmware upload failed.',true);else report(data.message||'Gateway firmware updated.');button.disabled=false};request.onerror=()=>{if(board==='c3'&&uploadComplete)report('C3 upload was sent. The Gateway may be restarting. Wait a few seconds, then reconnect to verify.',false);else report('Connection to the Gateway was interrupted.',true);button.disabled=false};request.ontimeout=()=>{report('Gateway firmware upload timed out.',true);button.disabled=false};request.send(file)}catch(error){report(error.message||'Failed to read the firmware file.',true);button.disabled=false}})()}
$('c3UpdateForm').addEventListener('submit',event=>{event.preventDefault();uploadGatewayFirmware('c3')});
$('c6UpdateForm').addEventListener('submit',event=>{event.preventDefault();uploadGatewayFirmware('c6')});
checkUpdateSession();setInterval(checkUpdateSession,30000);
</script></body></html>
)HTML";

void sendJson(int statusCode, JsonDocument& document) {
  String body;
  serializeJson(document, body);
  server.sendHeader("Cache-Control", "no-store");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

void handleUpdateSessionGet() {
  DynamicJsonDocument response(128);
  response["authenticated"] = updateSessionAuthorized();
  response["password_configured"] = configurationHasUpdatePassword();
  sendJson(200, response);
}

void handleUpdateLogin() {
  DynamicJsonDocument request(512);
  if (deserializeJson(request, server.arg("plain"))) {
    DynamicJsonDocument response(128);
    response["error"] = "Invalid login request.";
    sendJson(400, response);
    return;
  }
  if (!configurationHasUpdatePassword()) {
    DynamicJsonDocument response(160);
    response["error"] = "Set an update password on the home page first.";
    sendJson(409, response);
    return;
  }
  const uint32_t now = millis();
  if (updateLoginBlockedUntil &&
      static_cast<int32_t>(now - updateLoginBlockedUntil) < 0) {
    DynamicJsonDocument response(128);
    response["error"] = "Too many failed attempts. Try again in one minute.";
    sendJson(429, response);
    return;
  }
  updateLoginBlockedUntil = 0;
  const String password = request["password"] | "";
  if (!configurationVerifyUpdatePassword(password)) {
    if (++updateLoginFailures >= MAX_UPDATE_LOGIN_FAILURES) {
      updateLoginFailures = 0;
      updateLoginBlockedUntil = now + UPDATE_LOGIN_LOCKOUT_MS;
    }
    DynamicJsonDocument response(128);
    response["error"] = "Incorrect update password.";
    sendJson(401, response);
    return;
  }

  updateLoginFailures = 0;
  clearUpdateSession();
  updateSessionToken = createUpdateSessionToken();
  updateSessionExpiresAt = now + UPDATE_SESSION_DURATION_MS;
  server.sendHeader("Set-Cookie",
                    String(UPDATE_SESSION_COOKIE_NAME) + "=" + updateSessionToken +
                        "; Path=/api; Max-Age=3600; HttpOnly; SameSite=Strict");
  DynamicJsonDocument response(128);
  response["ok"] = true;
  sendJson(200, response);
}

void handleUpdateLogout() {
  clearUpdateSession();
  server.sendHeader("Set-Cookie", String(UPDATE_SESSION_COOKIE_NAME) +
                                      "=; Path=/api; Max-Age=0; HttpOnly; SameSite=Strict");
  DynamicJsonDocument response(64);
  response["ok"] = true;
  sendJson(200, response);
}

void handleUpdatePasswordPost() {
  if (!authorizedWithSavedApiToken() && !updateSessionAuthorized()) {
    DynamicJsonDocument response(128);
    response["error"] = "Enter the saved API token or sign in to updates to set the update password.";
    sendJson(401, response);
    return;
  }
  DynamicJsonDocument request(512);
  if (deserializeJson(request, server.arg("plain"))) {
    DynamicJsonDocument response(128);
    response["error"] = "Invalid update password request.";
    sendJson(400, response);
    return;
  }
  String error;
  if (!configurationSetUpdatePassword(request["password"] | "", error)) {
    DynamicJsonDocument response(192);
    response["error"] = error;
    sendJson(400, response);
    return;
  }
  clearUpdateSession();
  DynamicJsonDocument response(128);
  response["ok"] = true;
  response["configured"] = true;
  sendJson(200, response);
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
    if (!updateSessionAuthorized()) {
      firmwareUpload.statusCode = 401;
      firmwareUpload.error = "Log in before uploading firmware.";
      logFirmwareUploadError(firmwareUpload.error);
      return;
    }
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
    sendJson(firmwareUpload.statusCode, response);
    return;
  }
  response["ok"] = true;
  response["message"] = "Firmware was queued on C6. The tag will download it at its next check-in.";
  sendJson(200, response);
}

void handleC3GatewayFirmwareRaw() {
  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    c3FirmwareUpload = GatewayFirmwareUploadState{};
    if (!updateSessionAuthorized()) {
      setGatewayFirmwareError(c3FirmwareUpload, 401, "Sign in before uploading firmware.");
      return;
    }
    const int contentLength = server.clientContentLength();
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA C3] Upload started; content length=%d bytes.\n", contentLength);
#endif
    if (contentLength < 8192) {
      setGatewayFirmwareError(c3FirmwareUpload, 400, "Invalid or missing firmware file.");
      return;
    }
    c3FirmwareUpload.expectedSize = static_cast<uint32_t>(contentLength);
    if (!Update.begin(c3FirmwareUpload.expectedSize, U_FLASH)) {
      const String error = String("Unable to begin C3 OTA update: ") + Update.errorString();
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.printf("[OTA C3] ERROR: %s\n", error.c_str());
#endif
      setGatewayFirmwareError(c3FirmwareUpload, 400, error);
      return;
    }
    c3FirmwareUpload.started = true;
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA C3] OTA partition ready; receiving %lu bytes.\n",
                  static_cast<unsigned long>(c3FirmwareUpload.expectedSize));
#endif
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!c3FirmwareUpload.started || !c3FirmwareUpload.error.isEmpty()) return;
    if (c3FirmwareUpload.receivedSize > c3FirmwareUpload.expectedSize ||
        raw.currentSize > c3FirmwareUpload.expectedSize - c3FirmwareUpload.receivedSize) {
      Update.abort();
      c3FirmwareUpload.started = false;
      setGatewayFirmwareError(c3FirmwareUpload, 400, "Received data exceeded the declared firmware size.");
      return;
    }
    if (Update.write(raw.buf, raw.currentSize) != raw.currentSize) {
      const String error = String("Failed to write C3 firmware: ") + Update.errorString();
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.printf("[OTA C3] ERROR after %lu/%lu bytes: %s\n",
                    static_cast<unsigned long>(c3FirmwareUpload.receivedSize),
                    static_cast<unsigned long>(c3FirmwareUpload.expectedSize), error.c_str());
#endif
      Update.abort();
      c3FirmwareUpload.started = false;
      setGatewayFirmwareError(c3FirmwareUpload, 400, error);
      return;
    }
    c3FirmwareUpload.receivedSize += raw.currentSize;
#if ARDUINO_USB_CDC_ON_BOOT
    if (c3FirmwareUpload.receivedSize >= c3FirmwareUpload.nextProgressLog ||
        c3FirmwareUpload.receivedSize == c3FirmwareUpload.expectedSize) {
      Serial.printf("[OTA C3] Received %lu/%lu bytes (%u%%).\n",
                    static_cast<unsigned long>(c3FirmwareUpload.receivedSize),
                    static_cast<unsigned long>(c3FirmwareUpload.expectedSize),
                    static_cast<unsigned>(c3FirmwareUpload.receivedSize * 100u /
                                          c3FirmwareUpload.expectedSize));
      c3FirmwareUpload.nextProgressLog = c3FirmwareUpload.receivedSize + 16 * 1024;
    }
#endif
    return;
  }

  if (raw.status == RAW_END) {
    if (!c3FirmwareUpload.started) return;
    if (c3FirmwareUpload.receivedSize != c3FirmwareUpload.expectedSize) {
      Update.abort();
      setGatewayFirmwareError(c3FirmwareUpload, 400, "C3 firmware upload was incomplete.");
    } else if (!Update.end()) {
      const String error = String("C3 firmware image validation failed: ") + Update.errorString();
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.printf("[OTA C3] ERROR: %s\n", error.c_str());
#endif
      setGatewayFirmwareError(c3FirmwareUpload, 400, error);
    } else {
      c3FirmwareUpload.succeeded = true;
#if ARDUINO_USB_CDC_ON_BOOT
      Serial.println("[OTA C3] Firmware written and validated; sending response before restart.");
#endif
    }
    c3FirmwareUpload.started = false;
    return;
  }

  if (raw.status == RAW_ABORTED) {
    if (c3FirmwareUpload.started) Update.abort();
    c3FirmwareUpload.started = false;
    if (c3FirmwareUpload.error.isEmpty())
      setGatewayFirmwareError(c3FirmwareUpload, 400,
                              "Connection was interrupted during C3 firmware upload.");
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.printf("[OTA C3] Upload connection aborted after %lu/%lu bytes.\n",
                  static_cast<unsigned long>(c3FirmwareUpload.receivedSize),
                  static_cast<unsigned long>(c3FirmwareUpload.expectedSize));
#endif
  }
}

void handleC3GatewayFirmwarePost() {
  DynamicJsonDocument response(256);
  if (!c3FirmwareUpload.succeeded) {
    response["error"] = c3FirmwareUpload.error.isEmpty()
                            ? "C3 firmware update failed."
                            : c3FirmwareUpload.error;
    sendJson(c3FirmwareUpload.statusCode, response);
    return;
  }
  response["ok"] = true;
  response["message"] = "C3 firmware installed. The Gateway is restarting.";
  sendJson(200, response);
  delay(1500);
  ESP.restart();
}

void finishC6GatewayFirmwareUpload(bool bodyComplete) {
  if (bodyComplete && c6FirmwareUpload.ready &&
      c6FirmwareUpload.error.isEmpty() &&
      c6FirmwareUpload.receivedSize == c6FirmwareUpload.expectedSize) {
    DynamicJsonDocument response(256);
    if (!c6UartWaitResponseLocked(C6_REPLY_GATEWAY_FW_UPDATED, response, 30000)) {
      setGatewayFirmwareError(c6FirmwareUpload, 502, "C6 did not confirm the firmware update.");
    } else if (!response["ok"].as<bool>()) {
      setGatewayFirmwareError(c6FirmwareUpload, 400,
                              String("C6 firmware update failed: ") +
                                  String(response["error"] | "unknown reason"));
    } else if ((c6FirmwareUpload.crcState ^ 0xFFFFFFFFu) != c6FirmwareUpload.expectedCrc) {
      setGatewayFirmwareError(c6FirmwareUpload, 400, "C6 firmware upload checksum mismatch.");
    } else {
      c6FirmwareUpload.succeeded = true;
    }
  } else if (c6FirmwareUpload.error.isEmpty()) {
    setGatewayFirmwareError(c6FirmwareUpload, 400, "C6 firmware upload was incomplete.");
  }

  if (c6FirmwareUpload.uartLocked) {
    c6UartUnlock();
    c6FirmwareUpload.uartLocked = false;
  }
  c6FirmwareUpload.ready = false;
}

void handleC6GatewayFirmwareRaw() {
  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    c6FirmwareUpload = GatewayFirmwareUploadState{};
    if (!updateSessionAuthorized()) {
      setGatewayFirmwareError(c6FirmwareUpload, 401, "Sign in before uploading firmware.");
      return;
    }
    const int contentLength = server.clientContentLength();
    if (contentLength < 8192 ||
        !parseHeaderNumber(server.header("X-Firmware-CRC32"), 16, c6FirmwareUpload.expectedCrc)) {
      setGatewayFirmwareError(c6FirmwareUpload, 400, "C6 firmware metadata is missing or invalid.");
      return;
    }
    c6FirmwareUpload.expectedSize = static_cast<uint32_t>(contentLength);
    if (!c6UartLock(30000)) {
      setGatewayFirmwareError(c6FirmwareUpload, 503, "C6 UART is currently busy.");
      return;
    }
    c6FirmwareUpload.uartLocked = true;

    DynamicJsonDocument request(192);
    request["cmd"] = "put_gateway_firmware";
    request["size"] = c6FirmwareUpload.expectedSize;
    request["crc32"] = c6FirmwareUpload.expectedCrc;
    String line;
    serializeJson(request, line);

    DynamicJsonDocument response(256);
    if (!c6UartCommandLocked(line, C6_REPLY_IMAGE_READY, response, 30000)) {
      setGatewayFirmwareError(c6FirmwareUpload, 502, "C6 did not respond to the firmware update request.");
    } else if (!response["ok"].as<bool>()) {
      setGatewayFirmwareError(c6FirmwareUpload, 400,
                              String("C6 rejected the firmware: ") +
                                  String(response["error"] | "unknown reason"));
    } else {
      c6FirmwareUpload.ready = true;
      return;
    }
    c6UartUnlock();
    c6FirmwareUpload.uartLocked = false;
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!c6FirmwareUpload.ready || !c6FirmwareUpload.error.isEmpty()) return;
    if (c6FirmwareUpload.receivedSize > c6FirmwareUpload.expectedSize ||
        raw.currentSize > c6FirmwareUpload.expectedSize - c6FirmwareUpload.receivedSize) {
      setGatewayFirmwareError(c6FirmwareUpload, 400, "Received data exceeded the declared firmware size.");
      return;
    }
    c6FirmwareUpload.crcState =
        updateCrc32(c6FirmwareUpload.crcState, raw.buf, raw.currentSize);
    String forwardError;
    if (!c6UartSendGatewayFirmwareLocked(raw.buf, raw.currentSize,
                                         c6FirmwareUpload.receivedSize, forwardError)) {
      setGatewayFirmwareError(c6FirmwareUpload, 502,
                              String("Failed to forward firmware to C6: ") + forwardError);
      return;
    }
    c6FirmwareUpload.receivedSize += raw.currentSize;
    return;
  }

  if (raw.status == RAW_END) {
    finishC6GatewayFirmwareUpload(true);
    return;
  }

  if (raw.status == RAW_ABORTED) {
    setGatewayFirmwareError(c6FirmwareUpload, 400, "Connection was interrupted during C6 firmware upload.");
    finishC6GatewayFirmwareUpload(false);
  }
}

void handleC6GatewayFirmwarePost() {
  DynamicJsonDocument response(256);
  if (!c6FirmwareUpload.succeeded) {
    response["error"] = c6FirmwareUpload.error.isEmpty()
                            ? "C6 firmware update failed."
                            : c6FirmwareUpload.error;
    sendJson(c6FirmwareUpload.statusCode, response);
    return;
  }
  response["ok"] = true;
  response["message"] = "C6 firmware installed. The board is restarting.";
  sendJson(200, response);
}

void handleConfigurationGet() {
  DynamicJsonDocument response(4096);
  response["configured"] = configurationHasApiToken();
  response["masked"] = configurationHasApiToken() ? "••••••••" : "";
  response["update_password_configured"] = configurationHasUpdatePassword();
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
  String error;
  if (!configurationSetApiToken(token, false, error)) {
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
  server.on("/api/update/session", HTTP_GET, handleUpdateSessionGet);
  server.on("/api/update/login", HTTP_POST, handleUpdateLogin);
  server.on("/api/update/logout", HTTP_POST, handleUpdateLogout);
  server.on("/api/update/password", HTTP_POST, handleUpdatePasswordPost);
  server.collectHeaders(firmwareHeaders, sizeof(firmwareHeaders) / sizeof(firmwareHeaders[0]));
  server.on("/api/firmware", HTTP_POST, handleFirmwarePost, handleFirmwareRaw);
  server.on("/api/gateway-firmware/c3", HTTP_POST, handleC3GatewayFirmwarePost,
            handleC3GatewayFirmwareRaw);
  server.on("/api/gateway-firmware/c6", HTTP_POST, handleC6GatewayFirmwarePost,
            handleC6GatewayFirmwareRaw);
  server.onNotFound([]() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(404, "text/plain; charset=utf-8", "Not found.");
  });
  server.begin();
  return xTaskCreate(webServerTask, "web-server", 12288, nullptr, 1, nullptr) == pdPASS;
}
