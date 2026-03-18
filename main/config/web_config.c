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
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_wifi.h"
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
"<!-- 阻止浏览器自动请求 favicon 和 apple-touch-icon，修复 404 和 104 报错 -->\n"
"<link rel=\"icon\" href=\"data:,\">\n"
"<link rel=\"apple-touch-icon\" href=\"data:,\">\n"
"<style>\n"
":root{"
" --bg-body:#0f172a;--bg-card:#1e293b;--bg-input:#0b1120;"
" --border:#334155;--border-hover:#475569;--border-focus:#3b82f6;"
" --text-main:#f8fafc;--text-muted:#94a3b8;"
" --primary:#3b82f6;--primary-hover:#2563eb;"
" --ok-text:#34d399;--ok-bg:rgba(52,211,153,0.1);--ok-border:#059669;"
" --warn-text:#fbbf24;--warn-bg:rgba(251,191,36,0.1);--warn-border:#d97706;"
" --radius-sm:6px;--radius-md:10px;--radius-lg:16px"
"}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{font-family:system-ui,-apple-system,sans-serif;background-color:var(--bg-body);color:var(--text-main);line-height:1.5;min-height:100vh;padding-bottom:40px}\n"
"main{max-width:760px;margin:0 auto;padding:32px 20px}\n"
"header{margin-bottom:32px;border-bottom:1px solid var(--border);padding-bottom:20px;display:flex;justify-content:space-between;align-items:flex-start;flex-wrap:wrap;gap:16px}\n"
"header h1{font-size:28px;font-weight:700;letter-spacing:-.5px}\n"
".subtitle{font-size:14px;color:var(--text-muted);margin-top:4px}\n"
"#status{padding:6px 14px;border-radius:99px;font-size:13px;font-weight:600;background:var(--bg-card);border:1px solid var(--border);transition:all .3s}\n"
"#status.ok{background:var(--ok-bg);color:var(--ok-text);border-color:var(--ok-border)}\n"
"#status.warn{background:var(--warn-bg);color:var(--warn-text);border-color:var(--warn-border)}\n"
".card{background:var(--bg-card);border:1px solid var(--border);border-radius:var(--radius-lg);padding:24px;margin-bottom:24px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1),0 2px 4px -1px rgba(0,0,0,.06);transition:border-color .2s}\n"
".card:hover{border-color:var(--border-hover)}\n"
".card-title{font-size:18px;font-weight:600;display:flex;align-items:center;gap:8px;margin-bottom:20px;color:var(--text-main)}\n"
".card-title svg{width:20px;height:20px;color:var(--primary)}\n"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:20px}\n"
".field{display:flex;flex-direction:column;gap:8px}\n"
".field label{font-size:13px;font-weight:500;color:var(--text-muted)}\n"
"input,select{width:100%;padding:10px 14px;border-radius:var(--radius-sm);border:1px solid var(--border);background:var(--bg-input);color:var(--text-main);font-size:14px;outline:none;transition:all .2s}\n"
"input::placeholder{color:#475569}\n"
"input:focus,select:focus{border-color:var(--border-focus);box-shadow:0 0 0 3px rgba(59,130,246,.2)}\n"
"select{cursor:pointer;appearance:none;background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' fill='none' viewBox='0 0 24 24' stroke='%2394a3b8'%3E%3Cpath stroke-linecap='round' stroke-linejoin='round' stroke-width='2' d='M19 9l-7 7-7-7'/%3E%3C/svg%3E\");background-repeat:no-repeat;background-position:right 12px center;background-size:16px;padding-right:36px}\n"
".pwd-wrap{position:relative;display:flex;align-items:center}\n"
".pwd-wrap input{padding-right:40px}\n"
".pwd-toggle{position:absolute;right:10px;background:none;border:none;cursor:pointer;color:var(--text-muted);display:flex;align-items:center;padding:4px;border-radius:4px;transition:color .2s}\n"
".pwd-toggle:hover{color:var(--text-main)}\n"
".pwd-toggle svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}\n"
".badge{display:inline-flex;align-items:center;padding:2px 10px;border-radius:99px;font-size:11px;font-weight:600;margin-top:8px;align-self:flex-start}\n"
".badge.ok{background:var(--ok-bg);color:var(--ok-text);border:1px solid var(--ok-border)}\n"
".badge.warn{background:var(--warn-bg);color:var(--warn-text);border:1px solid var(--warn-border)}\n"
".check-row{display:inline-flex;align-items:center;gap:8px;margin-top:12px;font-size:13px;color:var(--text-muted);cursor:pointer;user-select:none}\n"
".check-row input[type=checkbox]{width:16px;height:16px;cursor:pointer;accent-color:var(--primary)}\n"
".actions{display:flex;gap:12px;margin-top:32px;flex-wrap:wrap}\n"
"button{padding:10px 24px;border-radius:var(--radius-sm);border:none;font-size:14px;font-weight:600;cursor:pointer;transition:all .2s;display:inline-flex;align-items:center;gap:8px;justify-content:center}\n"
"button:active{transform:translateY(1px)}\n"
".btn-primary{background:var(--primary);color:#fff}\n"
".btn-primary:hover{background:var(--primary-hover);box-shadow:0 4px 12px rgba(59,130,246,.3)}\n"
".btn-secondary{background:transparent;color:var(--text-main);border:1px solid var(--border)}\n"
".btn-secondary:hover{background:var(--border)}\n"
"button:disabled{opacity:.5;cursor:not-allowed;transform:none;box-shadow:none}\n"
".scan-row{display:flex;align-items:center;gap:12px;margin-top:16px;flex-wrap:wrap}\n"
".scan-status{font-size:13px;color:var(--text-muted)}\n"
".hint{font-size:12px;color:var(--text-muted);margin-top:12px;background:rgba(0,0,0,.2);padding:8px 12px;border-radius:var(--radius-sm);border-left:3px solid var(--border)}\n"
"@keyframes spin{to{transform:rotate(360deg)}}\n"
".spinner{display:none;width:18px;height:18px;border:2px solid var(--border);border-top-color:var(--primary);border-radius:50%;animation:spin .6s linear infinite}\n"
".spinner.active{display:inline-block}\n"
".wifi-list{margin-top:12px;border:1px solid var(--border);border-radius:var(--radius-sm);overflow:hidden}\n"
".wifi-item{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;border-bottom:1px solid var(--border);background:var(--bg-input)}\n"
".wifi-item:last-child{border-bottom:none}\n"
".wifi-item-name{font-size:14px;color:var(--text-main);display:flex;align-items:center;gap:8px}\n"
".wifi-item-name svg{width:16px;height:16px;color:var(--primary)}\n"
".wifi-item-actions{display:flex;gap:8px;align-items:center}\n"
".sig-bars{display:flex;align-items:flex-end;gap:1.5px;height:12px;width:14px}\n"
".sig-bars span{background:var(--text-muted);width:2.5px;border-radius:1px}\n"
".sig-bars span.active{background:var(--primary)}\n"
".sig-1{height:30%}.sig-2{height:55%}.sig-3{height:80%}.sig-4{height:100%}\n"
".btn-icon{padding:4px 8px;border-radius:4px;font-size:12px;cursor:pointer;transition:all .2s;background:transparent;border:1px solid var(--border);color:var(--text-muted)}\n"
".btn-icon:hover{background:var(--border);color:var(--text-main)}\n"
".btn-icon.danger:hover{background:rgba(239,68,68,.1);border-color:#ef4444;color:#ef4444}\n"
".wifi-empty{padding:16px;text-align:center;color:var(--text-muted);font-size:13px}\n"
"@media(max-width:600px){.grid{grid-template-columns:1fr}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<main>\n"
"<header>\n"
"<div><h1>&#x1F990; MiniShrimp Config</h1><div class=\"subtitle\">Update settings below. Restart device if WiFi or Bot credentials change.</div></div>\n"
"<div id=\"status\">Idle</div>\n"
"</header>\n"
"\n"
"<div class=\"card\">\n"
"<div class=\"card-title\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M8.111 16.404a5.5 5.5 0 017.778 0M12 20h.01m-7.08-7.071c3.904-3.905 10.236-3.905 14.141 0M1.394 9.393c5.857-5.857 15.355-5.857 21.213 0\"/></svg>WiFi Settings</div>\n"
"<label style=\"font-size:13px;font-weight:500;color:var(--text-muted)\">Saved Networks</label>\n"
"<div id=\"wifi_saved_list\" class=\"wifi-list\"><div class=\"wifi-empty\">Loading...</div></div>\n"
"<div class=\"grid\" style=\"margin-top:20px\"><div class=\"field\"><label>Nearby Networks</label><div id=\"wifi_scan_list\" class=\"wifi-list\"><div class=\"wifi-empty\">Click 'Scan WiFi' to discover available networks</div></div></div></div>\n"
"<div class=\"grid\" style=\"margin-top:20px\">\n"
"<div class=\"field\"><label>SSID (Manual / Hidden)</label><input id=\"wifi_ssid\" placeholder=\"Enter SSID\"></div>\n"
"<div class=\"field\"><label>Password</label><div class=\"pwd-wrap\"><input id=\"wifi_pass\" type=\"password\" placeholder=\"WiFi password\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div></div>\n"
"</div>\n"
"<div class=\"scan-row\"><button id=\"wifi_scan_btn\" class=\"btn-secondary\">Scan WiFi</button><div id=\"wifi_spinner\" class=\"spinner\"></div><span id=\"wifi_scan_status\" class=\"scan-status\"></span></div>\n"
"<label class=\"check-row\"><input type=\"checkbox\" id=\"wifi_clear\"> Clear ALL WiFi credentials</label>\n"
"</div>\n"
"\n"
"<div class=\"card\">\n"
"<div class=\"card-title\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M8 10h.01M12 10h.01M16 10h.01M9 16H5a2 2 0 01-2-2V6a2 2 0 012-2h14a2 2 0 012 2v8a2 2 0 01-2 2h-5l-5 5v-5z\"/></svg>Bot Configurations</div>\n"
"<div class=\"grid\">\n"
"<div class=\"field\"><label>Telegram Bot Token</label><div class=\"pwd-wrap\"><input id=\"tg_token\" type=\"password\" placeholder=\"Enter token\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div><div id=\"tg_status\" class=\"badge\">unknown</div><label class=\"check-row\"><input type=\"checkbox\" id=\"tg_clear\"> Clear Telegram token</label></div>\n"
"<div class=\"field\" style=\"border-left:1px solid var(--border);padding-left:20px\"><label>Feishu App ID</label><input id=\"feishu_app_id\" placeholder=\"cli_xxx\"><label style=\"margin-top:12px\">Feishu App Secret</label><div class=\"pwd-wrap\"><input id=\"feishu_app_secret\" type=\"password\" placeholder=\"Enter secret\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div><div id=\"feishu_status\" class=\"badge\">unknown</div><label class=\"check-row\"><input type=\"checkbox\" id=\"feishu_clear\"> Clear Feishu credentials</label></div>\n"
"</div>\n"
"</div>\n"
"\n"
"<div class=\"card\">\n"
"<div class=\"card-title\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z\"/></svg>LLM Settings</div>\n"
"<div class=\"grid\">\n"
"<div class=\"field\"><label>Provider</label><select id=\"provider\"><option value=\"anthropic\">Anthropic</option><option value=\"openai\">OpenAI</option><option value=\"qwen\">Qwen</option><option value=\"gemini\">Gemini</option><option value=\"deepseek\">DeepSeek</option><option value=\"zhipu\">Zhipu</option><option value=\"moonshot\">Moonshot</option><option value=\"minimax\">MiniMax</option><option value=\"yi\">Yi (01.AI)</option><option value=\"doubao\">Doubao</option><option value=\"hunyuan\">Hunyuan</option><option value=\"baichuan\">Baichuan</option><option value=\"qianfan\">Qianfan</option><option value=\"spark\">Spark</option><option value=\"custom\">Custom</option></select></div>\n"
"<div class=\"field\"><label>Model Name</label><input id=\"model\" placeholder=\"e.g. gpt-4o-mini\"></div>\n"
"<div class=\"field\"><label>API Key</label><div class=\"pwd-wrap\"><input id=\"api_key\" type=\"password\" placeholder=\"Enter API key\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div><div id=\"api_key_status\" class=\"badge\">unknown</div><label class=\"check-row\"><input type=\"checkbox\" id=\"api_key_clear\"> Clear API key</label></div>\n"
"</div>\n"
"<div class=\"grid\" style=\"margin-top:20px;padding-top:20px;border-top:1px dashed var(--border)\">\n"
"<div class=\"field\"><label>Custom API URL</label><input id=\"custom_url\" placeholder=\"https://api.example.com/v1/...\"></div>\n"
"<div class=\"field\"><label>Custom Header</label><input id=\"custom_header\" placeholder=\"Authorization\"></div>\n"
"<div class=\"field\"><label>Custom Prefix</label><input id=\"custom_prefix\" placeholder=\"Bearer \"></div>\n"
"</div>\n"
"<div class=\"hint\">For Custom provider, please configure the Custom URL and Auth details above.</div>\n"
"</div>\n"
"\n"
"<div class=\"card\">\n"
"<div class=\"card-title\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M21 12a9 9 0 01-9 9m9-9a9 9 0 00-9-9m9 9H3m9 9a9 9 0 01-9-9m9 9c1.657 0 3-4.03 3-9s-1.343-9-3-9m0 18c-1.657 0-3-4.03-3-9s1.343-9 3-9\"/></svg>Proxy Configuration</div>\n"
"<div class=\"grid\">\n"
"<div class=\"field\"><label>Host</label><input id=\"proxy_host\" placeholder=\"192.168.1.2\"></div>\n"
"<div class=\"field\"><label>Port</label><input id=\"proxy_port\" type=\"number\" placeholder=\"7890\"></div>\n"
"<div class=\"field\"><label>Type</label><select id=\"proxy_type\"><option value=\"http\">HTTP</option><option value=\"socks5\">SOCKS5</option></select></div>\n"
"</div>\n"
"<label class=\"check-row\"><input type=\"checkbox\" id=\"proxy_clear\"> Clear proxy configuration</label>\n"
"</div>\n"
"\n"
"<div class=\"card\">\n"
"<div class=\"card-title\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M21 21l-6-6m2-5a7 7 0 11-14 0 7 7 0 0114 0z\"/></svg>Web Search Engine</div>\n"
"<div class=\"grid\">\n"
"<div class=\"field\"><label>Brave Search Key</label><div class=\"pwd-wrap\"><input id=\"search_key\" type=\"password\" placeholder=\"Enter key\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div><div id=\"search_status\" class=\"badge\">unknown</div><label class=\"check-row\"><input type=\"checkbox\" id=\"search_clear\"> Clear Brave key</label></div>\n"
"<div class=\"field\" style=\"border-left:1px solid var(--border);padding-left:20px\"><label>Tavily Key</label><div class=\"pwd-wrap\"><input id=\"tavily_key\" type=\"password\" placeholder=\"Enter key\"><button type=\"button\" class=\"pwd-toggle\" onclick=\"togglePwd(this)\"><svg viewBox=\"0 0 24 24\"><path d=\"M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z\"></path></svg></button></div><div id=\"tavily_status\" class=\"badge\">unknown</div><label class=\"check-row\"><input type=\"checkbox\" id=\"tavily_clear\"> Clear Tavily key</label></div>\n"
"</div>\n"
"</div>\n"
"\n"
"<div class=\"actions\">\n"
"<button id=\"save\" class=\"btn-primary\">Save Settings</button>\n"
"<button id=\"saveRestart\" class=\"btn-secondary\">Save & Restart</button>\n"
"</div>\n"
"</main>\n"
"<script>\n"
"const statusEl=document.getElementById('status');\n"
"function togglePwd(btn){\n"
" const input=btn.previousElementSibling;const path=btn.querySelector('path');\n"
" if(input.type==='password'){input.type='text';path.setAttribute('d','M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-10-7-10-7a18.45 18.45 0 0 1 5.06-5.94M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 10 7 10 7a18.14 18.14 0 0 1-2.16 3.19m-6.72-1.07a3 3 0 1 1-4.24-4.24M1 1l22 22');}\n"
" else{input.type='password';path.setAttribute('d','M2 12s3-7 10-7 10 7 10 7-3 7-10 7-10-7-10-7Z M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6Z');}\n"
"}\n"
"function setBadge(el,ok){\n"
" if(!el)return;\n"
" el.className='badge '+(ok?'ok':'warn');\n"
" el.textContent=ok?'\\u2713 Configured':'Empty';\n"
"}\n"
"async function loadSavedWiFi(){\n"
" try{\n"
"  const r=await fetch('/api/wifi/saved');const list=await r.json();\n"
"  const el=document.getElementById('wifi_saved_list');\n"
"  if(!list.length){el.innerHTML='<div class=\"wifi-empty\">No saved networks</div>';return;}\n"
"  el.innerHTML=list.map(w=>{var esc=w.ssid.split(\"'\").join(\"\\\\'\");return '<div class=\"wifi-item\"><span class=\"wifi-item-name\"><svg fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"2\" d=\"M8.111 16.404a5.5 5.5 0 017.778 0M12 20h.01m-7.08-7.071c3.904-3.905 10.236-3.905 14.141 0M1.394 9.393c5.857-5.857 15.355-5.857 21.213 0\"/></svg>'+w.ssid+'</span><span class=\"wifi-item-actions\"><button class=\"btn-icon\" onclick=\"useWiFi(\\''+esc+'\\')\" >Use</button><button class=\"btn-icon danger\" onclick=\"deleteWiFi(\\''+esc+'\\')\" >Delete</button></span></div>';}).join('');\n"
" }catch(e){document.getElementById('wifi_saved_list').innerHTML='<div class=\"wifi-empty\">Failed to load</div>';}\n"
"}\n"
"function useWiFi(ssid){\n"
" var st=document.getElementById('wifi_scan_status');\n"
" st.textContent='Connecting to '+ssid+'...';\n"
" fetch('/api/wifi/use',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid})})\n"
"  .then(function(r){return r.json();}).then(function(d){\n"
"   if(d.ok){st.textContent='\\u2713 Connecting to '+ssid+'... Device will get new IP.';}\n"
"   else{st.textContent='\\u2717 Failed: '+(d.error||'unknown');}\n"
"  }).catch(function(e){st.textContent='\\u2717 Error: '+e;});\n"
"}\n"
"function selectWiFi(ssid){\n"
" document.getElementById('wifi_ssid').value=ssid;\n"
" document.getElementById('wifi_pass').value='';\n"
" document.getElementById('wifi_pass').focus();\n"
"}\n"
"async function deleteWiFi(ssid){\n"
" if(!confirm('Delete '+ssid+'?'))return;\n"
" try{\n"
"  const r=await fetch('/api/wifi/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid})});\n"
"  if(r.ok){loadSavedWiFi();}\n"
" }catch(e){}\n"
"}\n"
"async function loadConfig(){\n"
" try{\n"
"  const r=await fetch('/api/config');const c=await r.json();\n"
"  document.getElementById('wifi_ssid').value=c.wifi_ssid||'';\n"
"  document.getElementById('feishu_app_id').value=c.feishu_app_id||'';\n"
"  document.getElementById('provider').value=c.provider||'anthropic';\n"
"  document.getElementById('model').value=c.model||'';\n"
"  document.getElementById('custom_url').value=c.custom_url||'';\n"
"  document.getElementById('custom_header').value=c.custom_header||'';\n"
"  document.getElementById('custom_prefix').value=c.custom_prefix||'';\n"
"  document.getElementById('proxy_host').value=c.proxy_host||'';\n"
"  document.getElementById('proxy_port').value=c.proxy_port||'';\n"
"  document.getElementById('proxy_type').value=c.proxy_type||'http';\n"
"  document.getElementById('tg_token').value=c.tg_token||'';\n"
"  document.getElementById('feishu_app_secret').value=c.feishu_app_secret||'';\n"
"  document.getElementById('api_key').value=c.api_key||'';\n"
"  document.getElementById('wifi_pass').value=c.wifi_pass||'';\n"
"  document.getElementById('search_key').value=c.search_key||'';\n"
"  document.getElementById('tavily_key').value=c.tavily_key||'';\n"
"  setBadge(document.getElementById('tg_status'),c.tg_token_set);\n"
"  setBadge(document.getElementById('feishu_status'),c.feishu_secret_set);\n"
"  setBadge(document.getElementById('api_key_status'),c.api_key_set);\n"
"  setBadge(document.getElementById('search_status'),c.search_key_set);\n"
"  setBadge(document.getElementById('tavily_status'),c.tavily_key_set);\n"
"  loadSavedWiFi();\n"
" }catch(e){\n"
"  statusEl.className='status warn';statusEl.textContent='Load failed';\n"
" }\n"
"}\n"
"function hexToBytes(h){for(var b=[],i=0;i<h.length;i+=2)b.push(parseInt(h.substr(i,2),16));return new Uint8Array(b);}\n"
"function bytesToUtf8(bytes){const d=new TextDecoder('utf-8');let s=d.decode(bytes);if(s.includes('\\ufffd'))s=new TextDecoder('gbk').decode(bytes);return s;}\n"
"async function scanWiFi(){\n"
" const btn=document.getElementById('wifi_scan_btn');const st=document.getElementById('wifi_scan_status');\n"
" const sp=document.getElementById('wifi_spinner');const listEl=document.getElementById('wifi_scan_list');\n"
" btn.disabled=true;sp.className='spinner active';st.textContent='Scanning nearby networks...';\n"
" try{\n"
"  const r=await fetch('/api/wifi/scan');const list=await r.json();\n"
"  if(!list.length){listEl.innerHTML='<div class=\"wifi-empty\">No networks found</div>';}else{\n"
"  list.sort((a,b)=>b.rssi-a.rssi);\n"
"  listEl.innerHTML=list.map(ap=>{\n"
"   const bytes=hexToBytes(ap.ssid_hex);const name=bytesToUtf8(bytes);\n"
"   const bars=ap.rssi>-50?4:ap.rssi>-65?3:ap.rssi>-75?2:ap.rssi>-85?1:0;\n"
"   let barHtml='<div class=\"sig-bars\">';for(let i=1;i<=4;i++)barHtml+='<span class=\"sig-'+i+' '+(i<=bars?'active':'')+'\"></span>';barHtml+='</div>';\n"
"   return '<div class=\"wifi-item\"><span class=\"wifi-item-name\">'+barHtml+'<b>'+name+'</b> <small style=\"color:var(--text-muted);font-size:11px\">('+ap.rssi+'dBm)</small></span><button class=\"btn-icon\" onclick=\"selectWiFiFromScan(\\''+ap.ssid_hex+'\\')\">Select</button></div>';\n"
"  }).join('');\n"
"  }\n"
"  st.textContent='Found '+list.length+' networks.';\n"
" }catch(e){st.textContent='Scan failed. Retry in a moment.';listEl.innerHTML='<div class=\"wifi-empty\">Failed to scan</div>';}\n"
" btn.disabled=false;sp.className='spinner';\n"
"}\n"
"function selectWiFiFromScan(hex){\n"
" const bytes=hexToBytes(hex);const name=bytesToUtf8(bytes);\n"
" document.getElementById('wifi_ssid').value=name;\n"
" document.getElementById('wifi_pass').value='';\n"
" document.getElementById('wifi_pass').focus();\n"
"}\n"
"document.getElementById('wifi_scan_btn').addEventListener('click',e=>{e.preventDefault();scanWiFi();});\n"
"async function saveConfig(restart){\n"
" statusEl.className='';statusEl.textContent='Saving...';\n"
" const p={\n"
"  wifi_ssid:document.getElementById('wifi_ssid').value.trim(),\n"
"  wifi_pass:document.getElementById('wifi_pass').value,\n"
"  wifi_clear:document.getElementById('wifi_clear').checked,\n"
"  tg_token:document.getElementById('tg_token').value,\n"
"  tg_clear:document.getElementById('tg_clear').checked,\n"
"  feishu_app_id:document.getElementById('feishu_app_id').value.trim(),\n"
"  feishu_app_secret:document.getElementById('feishu_app_secret').value,\n"
"  feishu_clear:document.getElementById('feishu_clear').checked,\n"
"  provider:document.getElementById('provider').value,\n"
"  model:document.getElementById('model').value.trim(),\n"
"  api_key:document.getElementById('api_key').value,\n"
"  api_key_clear:document.getElementById('api_key_clear').checked,\n"
"  custom_url:document.getElementById('custom_url').value.trim(),\n"
"  custom_header:document.getElementById('custom_header').value.trim(),\n"
"  custom_prefix:document.getElementById('custom_prefix').value,\n"
"  proxy_host:document.getElementById('proxy_host').value.trim(),\n"
"  proxy_port:document.getElementById('proxy_port').value,\n"
"  proxy_type:document.getElementById('proxy_type').value,\n"
"  proxy_clear:document.getElementById('proxy_clear').checked,\n"
"  search_key:document.getElementById('search_key').value,\n"
"  search_clear:document.getElementById('search_clear').checked,\n"
"  tavily_key:document.getElementById('tavily_key').value,\n"
"  tavily_clear:document.getElementById('tavily_clear').checked,\n"
"  restart:!!restart\n"
" };\n"
" const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});\n"
" if(r.ok){\n"
"  statusEl.className='ok';statusEl.textContent=restart?'Saved! Restarting...':'Saved!';\n"
"  await loadConfig();\n"
" }else{\n"
"  statusEl.className='warn';statusEl.textContent='Save failed';\n"
" }\n"
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

static esp_err_t handle_wifi_saved(httpd_req_t *req)
{
    char *json_str = NULL;
    esp_err_t err = wifi_manager_get_saved_list(&json_str);
    if (err == ESP_OK && json_str) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
        free(json_str);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to get saved list");
    return ESP_FAIL;
}

static esp_err_t handle_wifi_delete(httpd_req_t *req)
{
    if (req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
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

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    esp_err_t err = ESP_FAIL;
    if (ssid && cJSON_IsString(ssid) && ssid->valuestring[0]) {
        err = wifi_manager_delete_saved(ssid->valuestring);
    }
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return ESP_OK;
}

static esp_err_t handle_wifi_use(httpd_req_t *req)
{
    if (req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
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

    cJSON *ssid_json = cJSON_GetObjectItem(root, "ssid");
    if (!ssid_json || !cJSON_IsString(ssid_json) || !ssid_json->valuestring[0]) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    /* Copy SSID to local buffer BEFORE freeing cJSON to avoid use-after-free */
    char ssid_buf[33] = {0};
    strncpy(ssid_buf, ssid_json->valuestring, sizeof(ssid_buf) - 1);
    cJSON_Delete(root);

    /* Find password from saved WiFi list in NVS */
    nvs_handle_t nvs;
    char password[65] = {0};
    bool found = false;

    if (nvs_open(SHRIMP_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = 0;
        char *json_str = NULL;
        if (nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len) == ESP_OK && len > 0) {
            json_str = malloc(len);
            if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
                cJSON *list = cJSON_Parse(json_str);
                if (list && cJSON_IsArray(list)) {
                    int sz = cJSON_GetArraySize(list);
                    for (int i = 0; i < sz; i++) {
                        cJSON *item = cJSON_GetArrayItem(list, i);
                        cJSON *item_ssid = cJSON_GetObjectItem(item, "ssid");
                        cJSON *item_pass = cJSON_GetObjectItem(item, "password");
                        if (item_ssid && cJSON_IsString(item_ssid) &&
                            strcmp(item_ssid->valuestring, ssid_buf) == 0) {
                            if (item_pass && cJSON_IsString(item_pass)) {
                                strncpy(password, item_pass->valuestring, sizeof(password) - 1);
                            }
                            found = true;
                            break;
                        }
                    }
                    cJSON_Delete(list);
                }
            }
            if (json_str) free(json_str);
        }
        nvs_close(nvs);
    }

    if (!found) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"WiFi not found in saved list\"}");
        return ESP_OK;
    }

    /* Connect using the new connect_to API (handles disconnect + reconnect safely) */
    esp_err_t err = wifi_manager_connect_to(ssid_buf, password);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Connection failed to start\"}");
    }
    return ESP_OK;
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

    httpd_uri_t wifi_saved = {
        .uri = "/api/wifi/saved",
        .method = HTTP_GET,
        .handler = handle_wifi_saved,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_delete = {
        .uri = "/api/wifi/delete",
        .method = HTTP_POST,
        .handler = handle_wifi_delete,
        .user_ctx = NULL
    };

    httpd_uri_t wifi_use = {
        .uri = "/api/wifi/use",
        .method = HTTP_POST,
        .handler = handle_wifi_use,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get_cfg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post_cfg));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_saved));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_delete));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_use));

    ESP_LOGI(TAG, "Config UI available at /config");
    return ESP_OK;
}
