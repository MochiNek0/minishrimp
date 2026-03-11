#include "web_config.h"
#include "shrimp_config.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "proxy/http_proxy.h"
#include "tools/tool_web_search.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_config";

static const char *CONFIG_HTML =
"<!doctype html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>MiniShrimp Config</title>\n"
"<style>\n"
":root{"
"  --bg-start:#0a0e14;--bg-end:#111927;--panel:rgba(22,28,38,0.75);"
"  --border:#1e293b;--border-hover:#334155;"
"  --muted:#8896a8;--text:#e2e8f0;--text-bright:#f1f5f9;"
"  --accent:#38bdf8;--accent-glow:rgba(56,189,248,0.15);"
"  --ok:#34d399;--ok-bg:rgba(52,211,153,0.1);--ok-border:#065f46;"
"  --warn:#fbbf24;--warn-bg:rgba(251,191,36,0.1);--warn-border:#92400e;"
"  --input-bg:#0f172a;--input-border:#1e293b;--input-focus:#38bdf8;"
"  --radius:10px;--radius-lg:14px"
"}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"  background:linear-gradient(160deg,var(--bg-start),var(--bg-end));color:var(--text);"
"  min-height:100vh;line-height:1.5}\n"
"main{max-width:720px;margin:0 auto;padding:24px 16px 48px}\n"
/* ---- Header ---- */
"header{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;"
"  padding:20px 0 12px;border-bottom:1px solid var(--border);margin-bottom:20px}\n"
"header h1{font-size:22px;font-weight:700;color:var(--text-bright);"
"  display:flex;align-items:center;gap:8px}\n"
".subtitle{font-size:13px;color:var(--muted);margin-top:2px}\n"
"#status{padding:4px 12px;border-radius:999px;font-size:12px;font-weight:600;"
"  background:#1e293b;color:var(--muted);transition:all .3s}\n"
"#status.ok{background:var(--ok-bg);color:var(--ok);border:1px solid var(--ok-border)}\n"
"#status.warn{background:var(--warn-bg);color:var(--warn);border:1px solid var(--warn-border)}\n"
/* ---- Card ---- */
".card{background:var(--panel);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);"
"  border:1px solid var(--border);border-radius:var(--radius-lg);"
"  padding:20px;margin-bottom:16px;transition:border-color .25s}\n"
".card:hover{border-color:var(--border-hover)}\n"
".card-title{font-size:15px;font-weight:600;color:var(--text-bright);"
"  margin-bottom:14px;display:flex;align-items:center;gap:8px;"
"  padding-bottom:10px;border-bottom:1px solid var(--border)}\n"
/* ---- Grid / Form ---- */
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:14px}\n"
".field{display:flex;flex-direction:column;gap:5px}\n"
".field label{font-size:12px;font-weight:500;color:var(--muted);letter-spacing:.3px;text-transform:uppercase}\n"
"input,select{width:100%;padding:9px 12px;border-radius:var(--radius);border:1px solid var(--input-border);"
"  background:var(--input-bg);color:var(--text);font-size:14px;transition:border-color .2s,box-shadow .2s;"
"  outline:none}\n"
"input:focus,select:focus{border-color:var(--input-focus);box-shadow:0 0 0 3px var(--accent-glow)}\n"
"input::placeholder{color:#475569}\n"
"select{cursor:pointer;appearance:none;"
"  background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 12 12'%3E%3Cpath d='M3 4.5L6 7.5L9 4.5' stroke='%236b7280' fill='none' stroke-width='1.5'/%3E%3C/svg%3E\");"
"  background-repeat:no-repeat;background-position:right 10px center}\n"
/* ---- Badge ---- */
".badge{display:inline-block;padding:2px 10px;border-radius:999px;font-size:11px;font-weight:600;"
"  margin-top:6px;background:#1e293b;color:var(--muted);border:1px solid transparent}\n"
".badge.ok{color:var(--ok);background:var(--ok-bg);border-color:var(--ok-border)}\n"
".badge.warn{color:var(--warn);background:var(--warn-bg);border-color:var(--warn-border)}\n"
/* ---- Checkbox ---- */
".check-row{display:flex;align-items:center;gap:6px;margin-top:8px;font-size:13px;color:var(--muted);cursor:pointer}\n"
".check-row input[type=checkbox]{width:15px;height:15px;accent-color:var(--accent);cursor:pointer}\n"
/* ---- Buttons ---- */
".actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:8px}\n"
"button{padding:10px 20px;border-radius:var(--radius);border:none;font-size:14px;font-weight:600;"
"  cursor:pointer;transition:all .2s;outline:none}\n"
"button:active{transform:scale(0.97)}\n"
".btn-primary{background:linear-gradient(135deg,#38bdf8,#818cf8);color:#0f172a}"
".btn-primary:hover{box-shadow:0 4px 20px rgba(56,189,248,0.35)}\n"
".btn-secondary{background:#1e293b;color:#94a3b8;border:1px solid var(--border)}"
".btn-secondary:hover{background:#263043;border-color:var(--border-hover);color:var(--text)}\n"
"button:disabled{opacity:.5;cursor:not-allowed;transform:none}\n"
/* ---- Scan row ---- */
".scan-row{display:flex;align-items:center;gap:10px;margin-top:12px;flex-wrap:wrap}\n"
".scan-status{font-size:13px;color:var(--muted)}\n"
/* ---- Spinner ---- */
"@keyframes spin{to{transform:rotate(360deg)}}\n"
".spinner{display:none;width:16px;height:16px;border:2px solid var(--border);"
"  border-top-color:var(--accent);border-radius:50%;animation:spin .6s linear infinite}\n"
".spinner.active{display:inline-block}\n"
/* ---- Hint ---- */
".hint{font-size:12px;color:var(--muted);margin-top:10px;line-height:1.4}\n"
/* ---- Responsive ---- */
"@media(max-width:480px){main{padding:16px 10px 32px}"
"  .grid{grid-template-columns:1fr}"
"  header h1{font-size:18px}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<main>\n"
"<header>\n"
"  <div>\n"
"    <h1>&#x1F990; MiniShrimp Config</h1>\n"
"    <div class=\"subtitle\">Save settings, then restart if WiFi/bot credentials changed.</div>\n"
"  </div>\n"
"  <div id=\"status\">Idle</div>\n"
"</header>\n"
"\n"
/* ---- WiFi Card ---- */
"<div class=\"card\">\n"
"  <div class=\"card-title\">&#x1F4F6; WiFi</div>\n"
"  <div class=\"grid\">\n"
"    <div class=\"field\"><label>Select Network</label>\n"
"      <select id=\"wifi_select\">\n"
"        <option value=\"\">-- Scan to discover --</option>\n"
"      </select>\n"
"    </div>\n"
"  </div>\n"
"  <div class=\"grid\" style=\"margin-top:12px\">\n"
"    <div class=\"field\"><label>Custom / Hex SSID</label>"
"      <input id=\"wifi_ssid\" placeholder=\"Manual SSID (if hidden)\"></div>\n"
"    <div class=\"field\"><label>Password</label>"
"      <input id=\"wifi_pass\" type=\"password\" placeholder=\"WiFi password\"></div>\n"
"  </div>\n"
"  <div class=\"scan-row\">\n"
"    <button id=\"wifi_scan_btn\" class=\"btn-secondary\">Scan WiFi</button>\n"
"    <div id=\"wifi_spinner\" class=\"spinner\"></div>\n"
"    <span id=\"wifi_scan_status\" class=\"scan-status\"></span>\n"
"  </div>\n"
"  <label class=\"check-row\"><input type=\"checkbox\" id=\"wifi_clear\"> Clear ALL WiFi credentials</label>\n"
"</div>\n"
"\n"
/* ---- Bots Card ---- */
"<div class=\"card\">\n"
"  <div class=\"card-title\">&#x1F916; Bots</div>\n"
"  <div class=\"grid\">\n"
"    <div class=\"field\">\n"
"      <label>Telegram Bot Token</label>\n"
"      <input id=\"tg_token\" type=\"password\" placeholder=\"Enter token\">\n"
"      <div id=\"tg_status\" class=\"badge\">unknown</div>\n"
"      <label class=\"check-row\"><input type=\"checkbox\" id=\"tg_clear\"> Clear Telegram token</label>\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>Feishu App ID</label>\n"
"      <input id=\"feishu_app_id\" placeholder=\"cli_xxx\">\n"
"      <label style=\"margin-top:8px\">Feishu App Secret</label>\n"
"      <input id=\"feishu_app_secret\" type=\"password\" placeholder=\"Enter secret\">\n"
"      <div id=\"feishu_status\" class=\"badge\">unknown</div>\n"
"      <label class=\"check-row\"><input type=\"checkbox\" id=\"feishu_clear\"> Clear Feishu credentials</label>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
/* ---- LLM Card ---- */
"<div class=\"card\">\n"
"  <div class=\"card-title\">&#x1F9E0; LLM</div>\n"
"  <div class=\"grid\">\n"
"    <div class=\"field\">\n"
"      <label>Provider</label>\n"
"      <select id=\"provider\">\n"
"        <option value=\"anthropic\">Anthropic</option>\n"
"        <option value=\"openai\">OpenAI</option>\n"
"        <option value=\"qwen\">Qwen</option>\n"
"        <option value=\"gemini\">Gemini</option>\n"
"        <option value=\"deepseek\">DeepSeek</option>\n"
"        <option value=\"zhipu\">Zhipu</option>\n"
"        <option value=\"moonshot\">Moonshot</option>\n"
"        <option value=\"minimax\">MiniMax</option>\n"
"        <option value=\"yi\">Yi</option>\n"
"        <option value=\"doubao\">Doubao</option>\n"
"        <option value=\"hunyuan\">Hunyuan</option>\n"
"        <option value=\"baichuan\">Baichuan</option>\n"
"        <option value=\"qianfan\">Qianfan</option>\n"
"        <option value=\"spark\">Spark</option>\n"
"        <option value=\"custom\">Custom</option>\n"
"      </select>\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>Model</label>\n"
"      <input id=\"model\" placeholder=\"e.g. gpt-4o-mini\">\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>API Key</label>\n"
"      <input id=\"api_key\" type=\"password\" placeholder=\"Enter API key\">\n"
"      <div id=\"api_key_status\" class=\"badge\">unknown</div>\n"
"      <label class=\"check-row\"><input type=\"checkbox\" id=\"api_key_clear\"> Clear API key</label>\n"
"    </div>\n"
"  </div>\n"
"  <div class=\"grid\" style=\"margin-top:14px\">\n"
"    <div class=\"field\">\n"
"      <label>Custom API URL</label>\n"
"      <input id=\"custom_url\" placeholder=\"https://api.example.com/v1/...\">\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>Custom Auth Header</label>\n"
"      <input id=\"custom_header\" placeholder=\"Authorization\">\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>Custom Auth Prefix</label>\n"
"      <input id=\"custom_prefix\" placeholder=\"Bearer \">\n"
"    </div>\n"
"  </div>\n"
"  <div class=\"hint\">For custom/hunyuan/baichuan/qianfan/spark, set Custom API URL + Auth Header/Prefix.</div>\n"
"</div>\n"
"\n"
/* ---- Proxy Card ---- */
"<div class=\"card\">\n"
"  <div class=\"card-title\">&#x1F310; Proxy</div>\n"
"  <div class=\"grid\">\n"
"    <div class=\"field\"><label>Host</label><input id=\"proxy_host\" placeholder=\"192.168.1.2\"></div>\n"
"    <div class=\"field\"><label>Port</label><input id=\"proxy_port\" type=\"number\" placeholder=\"7890\"></div>\n"
"    <div class=\"field\"><label>Type</label>\n"
"      <select id=\"proxy_type\">\n"
"        <option value=\"http\">HTTP</option>\n"
"        <option value=\"socks5\">SOCKS5</option>\n"
"      </select>\n"
"    </div>\n"
"  </div>\n"
"  <label class=\"check-row\"><input type=\"checkbox\" id=\"proxy_clear\"> Clear proxy</label>\n"
"</div>\n"
"\n"
/* ---- Search Card ---- */
"<div class=\"card\">\n"
"  <div class=\"card-title\">&#x1F50D; Search</div>\n"
"  <div class=\"grid\">\n"
"    <div class=\"field\">\n"
"      <label>Brave Search Key</label>\n"
"      <input id=\"search_key\" type=\"password\" placeholder=\"Enter key\">\n"
"      <div id=\"search_status\" class=\"badge\">unknown</div>\n"
"      <label class=\"check-row\"><input type=\"checkbox\" id=\"search_clear\"> Clear Brave key</label>\n"
"    </div>\n"
"    <div class=\"field\">\n"
"      <label>Tavily Key</label>\n"
"      <input id=\"tavily_key\" type=\"password\" placeholder=\"Enter key\">\n"
"      <div id=\"tavily_status\" class=\"badge\">unknown</div>\n"
"      <label class=\"check-row\"><input type=\"checkbox\" id=\"tavily_clear\"> Clear Tavily key</label>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"\n"
/* ---- Actions ---- */
"<div class=\"actions\">\n"
"  <button id=\"save\" class=\"btn-primary\">Save</button>\n"
"  <button id=\"saveRestart\" class=\"btn-secondary\">Save &amp; Restart</button>\n"
"</div>\n"
"</main>\n"
"<script>\n"
"const statusEl=document.getElementById('status');\n"
"function setBadge(el,ok){\n"
"  if(!el)return;\n"
"  el.className='badge '+(ok?'ok':'warn');\n"
"  el.textContent=ok?'configured':'empty';\n"
"}\n"
"async function loadConfig(){\n"
"  try{\n"
"    const r=await fetch('/api/config');\n"
"    const c=await r.json();\n"
"    document.getElementById('wifi_ssid').value=c.wifi_ssid||'';\n"
"    document.getElementById('feishu_app_id').value=c.feishu_app_id||'';\n"
"    document.getElementById('provider').value=c.provider||'anthropic';\n"
"    document.getElementById('model').value=c.model||'';\n"
"    document.getElementById('custom_url').value=c.custom_url||'';\n"
"    document.getElementById('custom_header').value=c.custom_header||'';\n"
"    document.getElementById('custom_prefix').value=c.custom_prefix||'';\n"
"    document.getElementById('proxy_host').value=c.proxy_host||'';\n"
"    document.getElementById('proxy_port').value=c.proxy_port||'';\n"
"    document.getElementById('proxy_type').value=c.proxy_type||'http';\n"
"    document.getElementById('tg_token').value=c.tg_token||'';\n"
"    document.getElementById('feishu_app_secret').value=c.feishu_app_secret||'';\n"
"    document.getElementById('api_key').value=c.api_key||'';\n"
"    document.getElementById('wifi_pass').value=c.wifi_pass||'';\n"
"    document.getElementById('search_key').value=c.search_key||'';\n"
"    document.getElementById('tavily_key').value=c.tavily_key||'';\n"
"    setBadge(document.getElementById('tg_status'),c.tg_token_set);\n"
"    setBadge(document.getElementById('feishu_status'),c.feishu_secret_set);\n"
"    setBadge(document.getElementById('api_key_status'),c.api_key_set);\n"
"    setBadge(document.getElementById('search_status'),c.search_key_set);\n"
"    setBadge(document.getElementById('tavily_status'),c.tavily_key_set);\n"
"  }catch(e){\n"
"    statusEl.className='status warn';\n"
"    statusEl.textContent='Load failed';\n"
"  }\n"
"}\n"
"function hexToBytes(h){\n"
"  for(var b=[],i=0;i<h.length;i+=2)b.push(parseInt(h.substr(i,2),16));\n"
"  return new Uint8Array(b);\n"
"}\n"
"async function scanWiFi(){\n"
"  const btn=document.getElementById('wifi_scan_btn');\n"
"  const st=document.getElementById('wifi_scan_status');\n"
"  const sp=document.getElementById('wifi_spinner');\n"
"  const sel=document.getElementById('wifi_select');\n"
"  btn.disabled=true;sp.className='spinner active';\n"
"  st.textContent='Scanning nearby networks...';\n"
"  try{\n"
"    const r=await fetch('/api/wifi/scan');\n"
"    const list=await r.json();\n"
"    sel.innerHTML='<option value=\"\">-- Select a network --</option>';\n"
"    const du=new TextDecoder('utf-8'),dg=new TextDecoder('gbk');\n"
"    list.sort((a,b)=>b.rssi-a.rssi);\n"
"    list.forEach(ap=>{\n"
"      const bytes=hexToBytes(ap.ssid_hex);\n"
"      let name=du.decode(bytes);\n"
"      if(name.includes('\\ufffd'))name=dg.decode(bytes);\n"
"      const o=document.createElement('option');\n"
"      o.value=ap.ssid_hex;\n"
"      const bars=ap.rssi>-50?'\\u2587\\u2587\\u2587\\u2587':ap.rssi>-65?'\\u2587\\u2587\\u2587\\u2581':ap.rssi>-75?'\\u2587\\u2587\\u2581\\u2581':'\\u2587\\u2581\\u2581\\u2581';\n"
"      o.textContent=name+' '+bars+' ('+ap.rssi+'dBm)';\n"
"      sel.appendChild(o);\n"
"    });\n"
"    st.textContent='Found '+list.length+' networks.';\n"
"  }catch(e){\n"
"    st.textContent='Scan failed. Retry in a moment.';\n"
"  }\n"
"  btn.disabled=false;sp.className='spinner';\n"
"}\n"
"document.getElementById('wifi_scan_btn').addEventListener('click',e=>{e.preventDefault();scanWiFi();});\n"
"document.getElementById('wifi_select').addEventListener('change',e=>{\n"
"  document.getElementById('wifi_ssid').value='';\n"
"  document.getElementById('wifi_pass').focus();\n"
"});\n"
"async function saveConfig(restart){\n"
"  statusEl.className='';statusEl.textContent='Saving...';\n"
"  const p={\n"
"    wifi_ssid:document.getElementById('wifi_ssid').value.trim(),\n"
"    wifi_ssid_hex:document.getElementById('wifi_select').value,\n"
"    wifi_pass:document.getElementById('wifi_pass').value,\n"
"    wifi_clear:document.getElementById('wifi_clear').checked,\n"
"    tg_token:document.getElementById('tg_token').value,\n"
"    tg_clear:document.getElementById('tg_clear').checked,\n"
"    feishu_app_id:document.getElementById('feishu_app_id').value.trim(),\n"
"    feishu_app_secret:document.getElementById('feishu_app_secret').value,\n"
"    feishu_clear:document.getElementById('feishu_clear').checked,\n"
"    provider:document.getElementById('provider').value,\n"
"    model:document.getElementById('model').value.trim(),\n"
"    api_key:document.getElementById('api_key').value,\n"
"    api_key_clear:document.getElementById('api_key_clear').checked,\n"
"    custom_url:document.getElementById('custom_url').value.trim(),\n"
"    custom_header:document.getElementById('custom_header').value.trim(),\n"
"    custom_prefix:document.getElementById('custom_prefix').value,\n"
"    proxy_host:document.getElementById('proxy_host').value.trim(),\n"
"    proxy_port:document.getElementById('proxy_port').value,\n"
"    proxy_type:document.getElementById('proxy_type').value,\n"
"    proxy_clear:document.getElementById('proxy_clear').checked,\n"
"    search_key:document.getElementById('search_key').value,\n"
"    search_clear:document.getElementById('search_clear').checked,\n"
"    tavily_key:document.getElementById('tavily_key').value,\n"
"    tavily_clear:document.getElementById('tavily_clear').checked,\n"
"    restart:!!restart\n"
"  };\n"
"  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});\n"
"  if(r.ok){\n"
"    statusEl.className='ok';\n"
"    statusEl.textContent=restart?'Saved! Restarting...':'Saved!';\n"
"    await loadConfig();\n"
"  }else{\n"
"    statusEl.className='warn';\n"
"    statusEl.textContent='Save failed';\n"
"  }\n"
"}\n"
"document.getElementById('save').addEventListener('click',e=>{e.preventDefault();saveConfig(false);});\n"
"document.getElementById('saveRestart').addEventListener('click',e=>{e.preventDefault();saveConfig(true);});\n"
"loadConfig();\n"
"</script>\n"
"</body>\n"
"</html>\n"

;

static bool read_nvs_str(const char *ns, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t len = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    if (err == ESP_OK && out[0]) return true;
    out[0] = '\0';
    return false;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    free(payload);
    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    char *json_str = NULL;
    esp_err_t err = wifi_manager_get_scan_results(&json_str);
    if (err == ESP_OK && json_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
        free(json_str);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
    return ESP_FAIL;
}

static esp_err_t handle_config_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, CONFIG_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    char buf[256] = {0};
    cJSON *root = cJSON_CreateObject();

    if (read_nvs_str(SHRIMP_NVS_WIFI, SHRIMP_NVS_KEY_SSID, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "wifi_ssid", buf);
    } else if (SHRIMP_SECRET_WIFI_SSID[0]) {
        cJSON_AddStringToObject(root, "wifi_ssid", SHRIMP_SECRET_WIFI_SSID);
    } else {
        cJSON_AddStringToObject(root, "wifi_ssid", "");
    }

    /* Telegram token */
    if (read_nvs_str(SHRIMP_NVS_TG, SHRIMP_NVS_KEY_TG_TOKEN, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "tg_token", buf);
        cJSON_AddBoolToObject(root, "tg_token_set", true);
    } else if (SHRIMP_SECRET_TG_TOKEN[0] != '\0') {
        cJSON_AddStringToObject(root, "tg_token", SHRIMP_SECRET_TG_TOKEN);
        cJSON_AddBoolToObject(root, "tg_token_set", true);
    } else {
        cJSON_AddStringToObject(root, "tg_token", "");
        cJSON_AddBoolToObject(root, "tg_token_set", false);
    }

    if (read_nvs_str(SHRIMP_NVS_FEISHU, SHRIMP_NVS_KEY_FEISHU_APP_ID, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "feishu_app_id", buf);
    } else if (SHRIMP_SECRET_FEISHU_APP_ID[0]) {
        cJSON_AddStringToObject(root, "feishu_app_id", SHRIMP_SECRET_FEISHU_APP_ID);
    } else {
        cJSON_AddStringToObject(root, "feishu_app_id", "");
    }

    /* Feishu secret */
    if (read_nvs_str(SHRIMP_NVS_FEISHU, SHRIMP_NVS_KEY_FEISHU_APP_SECRET, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "feishu_app_secret", buf);
        cJSON_AddBoolToObject(root, "feishu_secret_set", true);
    } else if (SHRIMP_SECRET_FEISHU_APP_SECRET[0] != '\0') {
        cJSON_AddStringToObject(root, "feishu_app_secret", SHRIMP_SECRET_FEISHU_APP_SECRET);
        cJSON_AddBoolToObject(root, "feishu_secret_set", true);
    } else {
        cJSON_AddStringToObject(root, "feishu_app_secret", "");
        cJSON_AddBoolToObject(root, "feishu_secret_set", false);
    }

    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_PROVIDER, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "provider", buf);
    } else if (SHRIMP_SECRET_MODEL_PROVIDER[0]) {
        cJSON_AddStringToObject(root, "provider", SHRIMP_SECRET_MODEL_PROVIDER);
    } else {
        cJSON_AddStringToObject(root, "provider", "anthropic");
    }

    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_MODEL, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "model", buf);
    } else if (SHRIMP_SECRET_MODEL[0]) {
        cJSON_AddStringToObject(root, "model", SHRIMP_SECRET_MODEL);
    } else {
        cJSON_AddStringToObject(root, "model", SHRIMP_LLM_DEFAULT_MODEL);
    }

    /* API key */
    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_API_KEY, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "api_key", buf);
        cJSON_AddBoolToObject(root, "api_key_set", true);
    } else if (SHRIMP_SECRET_API_KEY[0] != '\0') {
        cJSON_AddStringToObject(root, "api_key", SHRIMP_SECRET_API_KEY);
        cJSON_AddBoolToObject(root, "api_key_set", true);
    } else {
        cJSON_AddStringToObject(root, "api_key", "");
        cJSON_AddBoolToObject(root, "api_key_set", false);
    }

    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_CUSTOM_URL, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "custom_url", buf);
    } else if (SHRIMP_SECRET_CUSTOM_URL[0]) {
        cJSON_AddStringToObject(root, "custom_url", SHRIMP_SECRET_CUSTOM_URL);
    } else {
        cJSON_AddStringToObject(root, "custom_url", "");
    }

    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_CUSTOM_HEADER, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "custom_header", buf);
    } else if (SHRIMP_SECRET_CUSTOM_HEADER[0]) {
        cJSON_AddStringToObject(root, "custom_header", SHRIMP_SECRET_CUSTOM_HEADER);
    } else {
        cJSON_AddStringToObject(root, "custom_header", "");
    }

    if (read_nvs_str(SHRIMP_NVS_LLM, SHRIMP_NVS_KEY_CUSTOM_PREFIX, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "custom_prefix", buf);
    } else if (SHRIMP_SECRET_CUSTOM_PREFIX[0]) {
        cJSON_AddStringToObject(root, "custom_prefix", SHRIMP_SECRET_CUSTOM_PREFIX);
    } else {
        cJSON_AddStringToObject(root, "custom_prefix", "Bearer ");
    }

    if (read_nvs_str(SHRIMP_NVS_PROXY, SHRIMP_NVS_KEY_PROXY_HOST, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "proxy_host", buf);
        nvs_handle_t nvs;
        if (nvs_open(SHRIMP_NVS_PROXY, NVS_READONLY, &nvs) == ESP_OK) {
            uint16_t port = 0;
            if (nvs_get_u16(nvs, SHRIMP_NVS_KEY_PROXY_PORT, &port) == ESP_OK && port) {
                char pbuf[16];
                snprintf(pbuf, sizeof(pbuf), "%u", (unsigned)port);
                cJSON_AddStringToObject(root, "proxy_port", pbuf);
            }
            size_t len = sizeof(buf);
            memset(buf, 0, sizeof(buf));
            if (nvs_get_str(nvs, "proxy_type", buf, &len) == ESP_OK && buf[0]) {
                cJSON_AddStringToObject(root, "proxy_type", buf);
            }
            nvs_close(nvs);
        }
    } else {
        cJSON_AddStringToObject(root, "proxy_host", SHRIMP_SECRET_PROXY_HOST);
        cJSON_AddStringToObject(root, "proxy_port", SHRIMP_SECRET_PROXY_PORT);
        cJSON_AddStringToObject(root, "proxy_type", SHRIMP_SECRET_PROXY_TYPE[0] ? SHRIMP_SECRET_PROXY_TYPE : "http");
    }

    /* Search keys */
    if (read_nvs_str(SHRIMP_NVS_SEARCH, SHRIMP_NVS_KEY_API_KEY, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "search_key", buf);
        cJSON_AddBoolToObject(root, "search_key_set", true);
    } else if (SHRIMP_SECRET_SEARCH_KEY[0] != '\0') {
        cJSON_AddStringToObject(root, "search_key", SHRIMP_SECRET_SEARCH_KEY);
        cJSON_AddBoolToObject(root, "search_key_set", true);
    } else {
        cJSON_AddStringToObject(root, "search_key", "");
        cJSON_AddBoolToObject(root, "search_key_set", false);
    }
    if (read_nvs_str(SHRIMP_NVS_SEARCH, SHRIMP_NVS_KEY_TAVILY_KEY, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "tavily_key", buf);
        cJSON_AddBoolToObject(root, "tavily_key_set", true);
    } else if (SHRIMP_SECRET_TAVILY_KEY[0] != '\0') {
        cJSON_AddStringToObject(root, "tavily_key", SHRIMP_SECRET_TAVILY_KEY);
        cJSON_AddBoolToObject(root, "tavily_key_set", true);
    } else {
        cJSON_AddStringToObject(root, "tavily_key", "");
        cJSON_AddBoolToObject(root, "tavily_key_set", false);
    }

    /* WiFi password */
    if (read_nvs_str(SHRIMP_NVS_WIFI, SHRIMP_NVS_KEY_PASS, buf, sizeof(buf))) {
        cJSON_AddStringToObject(root, "wifi_pass", buf);
    } else if (SHRIMP_SECRET_WIFI_PASS[0]) {
        cJSON_AddStringToObject(root, "wifi_pass", SHRIMP_SECRET_WIFI_PASS);
    } else {
        cJSON_AddStringToObject(root, "wifi_pass", "");
    }

    esp_err_t err = send_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    if (req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "payload too large");
        return ESP_FAIL;
    }

    char *buf = calloc(1, req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    bool restart = cJSON_IsTrue(cJSON_GetObjectItem(root, "restart"));

    cJSON *wifi_clear = cJSON_GetObjectItem(root, "wifi_clear");
    if (cJSON_IsTrue(wifi_clear)) {
        wifi_manager_set_credentials("", "");
        wifi_manager_set_wifi_list("[]"); // clear LRU list too
    } else {
        cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
        cJSON *ssid_hex = cJSON_GetObjectItem(root, "wifi_ssid_hex");
        cJSON *pass = cJSON_GetObjectItem(root, "wifi_pass");
        
        char final_ssid[33] = {0};
        
        if (ssid_hex && cJSON_IsString(ssid_hex) && ssid_hex->valuestring[0]) {
            size_t in_len = strlen(ssid_hex->valuestring);
            size_t out_len = in_len / 2;
            if (out_len > 32) out_len = 32;
            for (size_t i = 0; i < out_len; i++) {
                sscanf(ssid_hex->valuestring + 2 * i, "%2hhx", (uint8_t*)&final_ssid[i]);
            }
        } else if (ssid && cJSON_IsString(ssid) && ssid->valuestring[0]) {
            strncpy(final_ssid, ssid->valuestring, sizeof(final_ssid) - 1);
        }

        if (final_ssid[0] && pass && cJSON_IsString(pass)) {
            wifi_manager_set_credentials(final_ssid, pass->valuestring);
        }
    }

    cJSON *tg_clear = cJSON_GetObjectItem(root, "tg_clear");
    cJSON *tg = cJSON_GetObjectItem(root, "tg_token");
    if (cJSON_IsTrue(tg_clear)) {
        telegram_set_token("");
    } else if (tg && cJSON_IsString(tg) && tg->valuestring[0]) {
        telegram_set_token(tg->valuestring);
    }

    cJSON *feishu_clear = cJSON_GetObjectItem(root, "feishu_clear");
    cJSON *fid = cJSON_GetObjectItem(root, "feishu_app_id");
    cJSON *fsec = cJSON_GetObjectItem(root, "feishu_app_secret");
    if (cJSON_IsTrue(feishu_clear)) {
        feishu_set_credentials("", "");
    } else if (fid && cJSON_IsString(fid) && fsec && cJSON_IsString(fsec) && fid->valuestring[0]) {
        feishu_set_credentials(fid->valuestring, fsec->valuestring);
    }

    cJSON *provider = cJSON_GetObjectItem(root, "provider");
    if (provider && cJSON_IsString(provider) && provider->valuestring[0]) {
        llm_set_provider(provider->valuestring);
    }

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (model && cJSON_IsString(model) && model->valuestring[0]) {
        llm_set_model(model->valuestring);
    }

    cJSON *api_key_clear = cJSON_GetObjectItem(root, "api_key_clear");
    cJSON *api_key = cJSON_GetObjectItem(root, "api_key");
    if (cJSON_IsTrue(api_key_clear)) {
        llm_set_api_key("");
    } else if (api_key && cJSON_IsString(api_key) && api_key->valuestring[0]) {
        llm_set_api_key(api_key->valuestring);
    }

    cJSON *custom_url = cJSON_GetObjectItem(root, "custom_url");
    if (custom_url && cJSON_IsString(custom_url)) {
        llm_set_custom_url(custom_url->valuestring);
    }
    cJSON *custom_header = cJSON_GetObjectItem(root, "custom_header");
    if (custom_header && cJSON_IsString(custom_header)) {
        llm_set_custom_header(custom_header->valuestring);
    }
    cJSON *custom_prefix = cJSON_GetObjectItem(root, "custom_prefix");
    if (custom_prefix && cJSON_IsString(custom_prefix)) {
        llm_set_custom_prefix(custom_prefix->valuestring);
    }

    cJSON *proxy_clear = cJSON_GetObjectItem(root, "proxy_clear");
    if (cJSON_IsTrue(proxy_clear)) {
        http_proxy_clear();
    } else {
        cJSON *ph = cJSON_GetObjectItem(root, "proxy_host");
        cJSON *pp = cJSON_GetObjectItem(root, "proxy_port");
        cJSON *pt = cJSON_GetObjectItem(root, "proxy_type");
        if (ph && cJSON_IsString(ph) && ph->valuestring[0] && pp && cJSON_IsString(pp) && pp->valuestring[0]) {
            uint16_t port = (uint16_t)atoi(pp->valuestring);
            const char *ptype = (pt && cJSON_IsString(pt) && pt->valuestring[0]) ? pt->valuestring : "http";
            http_proxy_set(ph->valuestring, port, ptype);
        }
    }

    cJSON *search_clear = cJSON_GetObjectItem(root, "search_clear");
    cJSON *search_key = cJSON_GetObjectItem(root, "search_key");
    if (cJSON_IsTrue(search_clear)) {
        tool_web_search_set_key("");
    } else if (search_key && cJSON_IsString(search_key) && search_key->valuestring[0]) {
        tool_web_search_set_key(search_key->valuestring);
    }

    cJSON *tavily_clear = cJSON_GetObjectItem(root, "tavily_clear");
    cJSON *tavily_key = cJSON_GetObjectItem(root, "tavily_key");
    if (cJSON_IsTrue(tavily_clear)) {
        tool_web_search_set_tavily_key("");
    } else if (tavily_key && cJSON_IsString(tavily_key) && tavily_key->valuestring[0]) {
        tool_web_search_set_tavily_key(tavily_key->valuestring);
    }

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    if (restart) {
        ESP_LOGI(TAG, "Restart requested from config UI");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    return ESP_OK;
}

esp_err_t web_config_register(httpd_handle_t server)
{
    httpd_uri_t page = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = handle_config_page,
        .user_ctx = NULL
    };

    httpd_uri_t get_cfg = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = handle_get_config,
        .user_ctx = NULL
    };

    httpd_uri_t post_cfg = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = handle_post_config,
        .user_ctx = NULL
    };
    
    httpd_uri_t wifi_scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = handle_wifi_scan,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_cfg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post_cfg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));

    ESP_LOGI(TAG, "Config UI available at /config");
    return ESP_OK;
}
