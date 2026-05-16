#include "web_server.h"
#include "debbie.h"
#include "settings.h"
#include "storage_manager.h"
#include "wifi_manager.h"
#include "camera_manager.h"
#include "memory_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";

/* --------------------------------------------------------------------------
 * Full configuration portal — tabbed single-page app
 * Tabs: Network | AI & LLM | Personality | Notifications | Advanced
 * -------------------------------------------------------------------------- */
static const char SETUP_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Debbie Config ✨</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#0d0d1a;color:#e0e0f0;min-height:100vh}"
".hero{background:linear-gradient(135deg,#0f3460 0%,#533483 60%,#9b2bba 100%);"
"padding:28px 20px 20px;text-align:center}"
".hero h1{font-size:1.9rem;margin-bottom:4px;letter-spacing:.5px}"
".hero p{opacity:.8;font-size:.9rem}"
".badge{display:inline-block;background:rgba(255,255,255,.15);border-radius:20px;"
"padding:2px 10px;font-size:.75rem;margin-top:6px}"

/* Tab bar */
".tabs{display:flex;overflow-x:auto;background:#12122a;border-bottom:2px solid #1e1e3f}"
".tab{flex:0 0 auto;padding:11px 16px;cursor:pointer;font-size:.83rem;color:#aaa;"
"white-space:nowrap;border-bottom:3px solid transparent;transition:.2s}"
".tab.active{color:#06d6a0;border-bottom-color:#06d6a0;font-weight:600}"
".tab:hover{color:#eee}"

/* Content */
".page{display:none;padding:0 12px 80px}"
".page.active{display:block}"
".card{background:#12122a;border-radius:14px;padding:20px;margin:14px 0;"
"box-shadow:0 4px 20px rgba(0,0,0,.35)}"
".card-title{color:#06d6a0;font-size:.95rem;font-weight:600;margin-bottom:14px;"
"display:flex;align-items:center;gap:8px}"
"label{display:block;margin-bottom:3px;font-size:.8rem;color:#999}"
"input,select,textarea{width:100%;padding:9px 11px;border-radius:8px;"
"border:1px solid #2a2a4a;background:#0d0d1a;color:#e0e0f0;"
"font-size:.9rem;margin-bottom:10px}"
"input:focus,textarea:focus,select:focus{outline:none;border-color:#06d6a0}"
"textarea{resize:vertical;min-height:90px;font-family:inherit}"
"select option{background:#0d0d1a}"
".row2{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
".toggle-row{display:flex;justify-content:space-between;align-items:center;"
"padding:8px 0;border-bottom:1px solid #1e1e3f}"
".toggle-row:last-child{border:none}"
".toggle-label{font-size:.88rem;color:#ccc}"
".toggle-sub{font-size:.75rem;color:#777;margin-top:2px}"
"input[type=checkbox]{width:auto;margin:0;accent-color:#06d6a0}"
".switch{position:relative;width:44px;height:24px}"
".switch input{opacity:0;width:0;height:0}"
".slider{position:absolute;cursor:pointer;inset:0;background:#333;border-radius:24px;transition:.3s}"
".slider:before{content:'';position:absolute;height:18px;width:18px;left:3px;bottom:3px;"
"background:#fff;border-radius:50%;transition:.3s}"
"input:checked+.slider{background:#06d6a0}"
"input:checked+.slider:before{transform:translateX(20px)}"

/* Provider cards */
".llm-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:8px;margin-bottom:12px}"
".llm-card{border:2px solid #2a2a4a;border-radius:10px;padding:10px 8px;cursor:pointer;"
"text-align:center;transition:.2s;background:#0d0d1a}"
".llm-card:hover{border-color:#5e548e}"
".llm-card.sel{border-color:#06d6a0;background:#0a2a1e}"
".llm-icon{font-size:1.5rem;margin-bottom:4px}"
".llm-name{font-size:.75rem;font-weight:600;color:#ccc}"
".llm-desc{font-size:.68rem;color:#666;margin-top:2px}"

/* Buttons */
".btn-bar{position:fixed;bottom:0;left:0;right:0;background:#12122a;"
"border-top:1px solid #1e1e3f;padding:10px 16px;display:flex;gap:8px;z-index:100}"
"button{padding:10px 16px;border-radius:8px;border:none;font-size:.88rem;cursor:pointer;transition:.2s}"
".btn-save{background:linear-gradient(90deg,#06d6a0,#5e548e);color:#fff;font-weight:700;flex:1}"
".btn-reset{background:#3a1010;color:#e74c3c;}"
"button:hover{opacity:.88;transform:translateY(-1px)}"
".msg{padding:10px 14px;border-radius:8px;margin-top:10px;display:none;font-size:.88rem}"
".msg.ok{background:#0d3a28;color:#06d6a0;display:block}"
".msg.err{background:#3a0d0d;color:#e74c3c;display:block}"
".status-bar{display:flex;gap:12px;flex-wrap:wrap;padding:10px 0;font-size:.8rem;color:#888}"
".dot{display:inline-block;width:7px;height:7px;border-radius:50%;margin-right:4px}"
".green{background:#06d6a0}.red{background:#e74c3c}.blue{background:#3a8fd6}"
".hint{font-size:.72rem;color:#555;margin-top:-6px;margin-bottom:8px}"
".section-sep{border:none;border-top:1px solid #1e1e3f;margin:14px 0}"
"</style></head><body>"

/* Hero */
"<div class='hero'>"
"<h1>✨ Debbie Config</h1>"
"<p>Portable Personal AI Companion</p>"
"<div class='badge' id='fw_badge'>Firmware v" DEBBIE_FW_VERSION "</div>"
"</div>"

/* Tab bar */
"<div class='tabs'>"
"<div class='tab active' onclick='showTab(0)'>📶 Network</div>"
"<div class='tab' onclick='showTab(1)'>🤖 AI & LLM</div>"
"<div class='tab' onclick='showTab(2)'>🎀 Personality</div>"
"<div class='tab' onclick='showTab(3)'>🔔 Notifications</div>"
"<div class='tab' onclick='showTab(4)'>⚙️ Advanced</div>"
"<div class='tab' onclick='showTab(5)'>🧠 Memory</div>"
"</div>"

/* ── Page 0: Network ───────────────────────────────────────── */
"<div class='page active' id='p0'>"

"<div class='card'>"
"<div class='card-title'>📶 Wi-Fi Networks</div>"
"<p style='font-size:.8rem;color:#777;margin-bottom:12px'>"
"Save up to 3 networks. Debbie tries them in order on boot.</p>"

"<label>Primary SSID</label><input id='ssid' placeholder='Your home WiFi'>"
"<label>Password</label><input id='pass' type='password' placeholder='Password'>"
"<hr class='section-sep'>"
"<label>Secondary SSID <span style='color:#555'>(fallback 1)</span></label>"
"<input id='ssid2' placeholder='Office / mobile hotspot'>"
"<label>Password</label><input id='pass2' type='password' placeholder='Password'>"
"<hr class='section-sep'>"
"<label>Tertiary SSID <span style='color:#555'>(fallback 2)</span></label>"
"<input id='ssid3' placeholder='Another network'>"
"<label>Password</label><input id='pass3' type='password' placeholder='Password'>"
"</div>"

"<div class='card'>"
"<div class='card-title'>🔵 Bluetooth (BLE)</div>"
"<p style='font-size:.8rem;color:#777;margin-bottom:12px'>"
"BLE lets you configure Debbie wirelessly from any phone using a "
"BLE Serial app (e.g. nRF Toolbox, Serial Bluetooth Terminal) — "
"no WiFi setup required. Send JSON config commands to the Nordic UART Service.</p>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>Enable Bluetooth</div>"
"<div class='toggle-sub'>BLE advertising · Nordic UART Service</div></div>"
"<label class='switch'><input type='checkbox' id='bt_en'>"
"<span class='slider'></span></label></div>"
"<br>"
"<label>BLE Device Name</label>"
"<input id='ble_name' placeholder='Debbie'>"
"<p class='hint'>Name shown when scanning for BLE devices on your phone.</p>"
"</div>"

"<div class='card'>"
"<div class='card-title'>📡 Companion Server</div>"
"<label>Companion Server URL</label>"
"<input id='companion_url' placeholder='ws://192.168.1.10:3001'>"
"<p class='hint'>Node.js companion server for WhatsApp, Email, Spotify &amp; agent tools.</p>"
"</div>"

"</div>"

/* ── Page 1: AI & LLM ─────────────────────────────────────── */
"<div class='page' id='p1'>"

"<div class='card'>"
"<div class='card-title'>🧠 LLM Provider</div>"
"<p style='font-size:.8rem;color:#777;margin-bottom:12px'>"
"Choose which AI service powers Debbie. Local providers (Ollama, LM Studio) "
"work entirely on your own hardware — great for privacy and offline use.</p>"
"<div class='llm-grid'>"
"<div class='llm-card sel' id='pcard_openai' onclick='selectProvider(\"openai\")'>"
"<div class='llm-icon'>🌐</div><div class='llm-name'>OpenAI</div>"
"<div class='llm-desc'>GPT-4o, o1, Realtime</div></div>"
"<div class='llm-card' id='pcard_anthropic' onclick='selectProvider(\"anthropic\")'>"
"<div class='llm-icon'>🧬</div><div class='llm-name'>Anthropic</div>"
"<div class='llm-desc'>Claude 3.5+</div></div>"
"<div class='llm-card' id='pcard_groq' onclick='selectProvider(\"groq\")'>"
"<div class='llm-icon'>⚡</div><div class='llm-name'>Groq</div>"
"<div class='llm-desc'>Ultra-fast inference</div></div>"
"<div class='llm-card' id='pcard_openrouter' onclick='selectProvider(\"openrouter\")'>"
"<div class='llm-icon'>🔀</div><div class='llm-name'>OpenRouter</div>"
"<div class='llm-desc'>100+ models</div></div>"
"<div class='llm-card' id='pcard_ollama' onclick='selectProvider(\"ollama\")'>"
"<div class='llm-icon'>🏠</div><div class='llm-name'>Ollama</div>"
"<div class='llm-desc'>Local · private</div></div>"
"</div>"
"<input type='hidden' id='llm_provider' value='openai'>"
"</div>"

"<div class='card' id='card_openai'>"
"<div class='card-title'>🌐 OpenAI Settings</div>"
"<label>API Key</label>"
"<input id='oai_key' type='password' placeholder='sk-...'>"
"<label>Model</label>"
"<select id='oai_model'>"
"<option value='gpt-4o'>gpt-4o (recommended)</option>"
"<option value='gpt-4o-mini'>gpt-4o-mini (faster / cheaper)</option>"
"<option value='o1-preview'>o1-preview (reasoning)</option>"
"<option value='o1-mini'>o1-mini</option>"
"</select>"
"<p class='hint'>Realtime voice always uses gpt-4o-realtime-preview regardless of model selection above.</p>"
"</div>"

"<div class='card' id='card_anthropic' style='display:none'>"
"<div class='card-title'>🧬 Anthropic Settings</div>"
"<label>API Key</label>"
"<input id='anthropic_key' type='password' placeholder='sk-ant-...'>"
"<label>Model</label>"
"<select id='anthropic_model'>"
"<option value='claude-3-5-sonnet-20241022'>Claude 3.5 Sonnet (recommended)</option>"
"<option value='claude-3-5-haiku-20241022'>Claude 3.5 Haiku (fast)</option>"
"<option value='claude-3-opus-20240229'>Claude 3 Opus (most capable)</option>"
"</select>"
"</div>"

"<div class='card' id='card_groq' style='display:none'>"
"<div class='card-title'>⚡ Groq Settings</div>"
"<label>API Key</label>"
"<input id='groq_key' type='password' placeholder='gsk_...'>"
"<label>Model</label>"
"<select id='groq_model'>"
"<option value='llama-3.3-70b-versatile'>Llama 3.3 70B Versatile (recommended)</option>"
"<option value='llama-3.1-8b-instant'>Llama 3.1 8B Instant (fastest)</option>"
"<option value='mixtral-8x7b-32768'>Mixtral 8x7B</option>"
"<option value='gemma2-9b-it'>Gemma 2 9B</option>"
"</select>"
"</div>"

"<div class='card' id='card_openrouter' style='display:none'>"
"<div class='card-title'>🔀 OpenRouter Settings</div>"
"<label>API Key</label>"
"<input id='or_key' type='password' placeholder='sk-or-...'>"
"<label>Model (OpenRouter model ID)</label>"
"<input id='or_model' placeholder='anthropic/claude-3.5-sonnet'>"
"<p class='hint'>Any model available on openrouter.ai. Examples:<br>"
"• anthropic/claude-3.5-sonnet<br>• google/gemini-2.0-flash<br>• meta-llama/llama-3.3-70b-instruct</p>"
"</div>"

"<div class='card' id='card_ollama' style='display:none'>"
"<div class='card-title'>🏠 Local LLM (Ollama / LM Studio)</div>"
"<label>Server URL</label>"
"<input id='local_llm_url' placeholder='http://192.168.1.100:11434'>"
"<p class='hint'>Ollama default: http://&lt;your-PC-IP&gt;:11434 &nbsp;|&nbsp; LM Studio: http://&lt;your-PC-IP&gt;:1234</p>"
"<label>Model Name</label>"
"<input id='local_llm_model' placeholder='llama3'>"
"<p class='hint'>Run <code style='color:#06d6a0'>ollama list</code> to see installed models. "
"Examples: llama3, mistral, phi3, gemma2, qwen2.5</p>"
"</div>"

"<div class='card'>"
"<div class='card-title'>🔗 Custom Agent (optional)</div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>Use Custom Agent Endpoint</div>"
"<div class='toggle-sub'>Route all AI calls through your own agent server</div></div>"
"<label class='switch'><input type='checkbox' id='use_agent'>"
"<span class='slider'></span></label></div>"
"<br>"
"<label>Agent WebSocket URL</label>"
"<input id='agent_url' placeholder='ws://your-agent:8080'>"
"<p class='hint'>Your agent receives function calls and returns results. "
"Must implement the Debbie agent protocol.</p>"
"</div>"
"</div>"

/* ── Page 2: Personality ──────────────────────────────────── */
"<div class='page' id='p2'>"

"<div class='card'>"
"<div class='card-title'>🎀 Identity</div>"
"<label>Name</label>"
"<input id='name' placeholder='Debbie'>"
"<p class='hint'>What your AI companion calls herself and how she introduces herself.</p>"
"</div>"

"<div class='card'>"
"<div class='card-title'>💬 Voice &amp; Response Style</div>"
"<label>Voice Style</label>"
"<select id='voice_style'>"
"<option value='friendly'>😊 Friendly (warm, casual, approachable)</option>"
"<option value='professional'>💼 Professional (formal, concise, business-like)</option>"
"<option value='playful'>🎉 Playful (fun, energetic, uses humour)</option>"
"<option value='calm'>🧘 Calm (soothing, measured, thoughtful)</option>"
"</select>"
"<label>Response Length</label>"
"<select id='resp_len'>"
"<option value='brief'>⚡ Brief (1–2 sentences max)</option>"
"<option value='normal'>📝 Normal (natural conversational length)</option>"
"<option value='detailed'>📚 Detailed (thorough explanations)</option>"
"</select>"
"</div>"

"<div class='card'>"
"<div class='card-title'>📜 System Prompt</div>"
"<p style='font-size:.8rem;color:#777;margin-bottom:10px'>"
"The system prompt defines your AI companion's core personality, knowledge "
"and behaviour. The defaults work great — only change this if you want to "
"significantly alter Debbie's persona.</p>"
"<label>System Prompt</label>"
"<textarea id='sys_prompt' rows='8'></textarea>"
"<button class='btn-save' style='width:auto;padding:8px 16px;font-size:.8rem' "
"onclick='resetPrompt()'>↩ Reset to Default</button>"
"</div>"
"</div>"

/* ── Page 3: Notifications ───────────────────────────────── */
"<div class='page' id='p3'>"

"<div class='card'>"
"<div class='card-title'>🔔 Notification Sources</div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>All Notifications</div>"
"<div class='toggle-sub'>Master switch</div></div>"
"<label class='switch'><input type='checkbox' id='notifs_en' checked>"
"<span class='slider'></span></label></div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>💬 WhatsApp</div>"
"<div class='toggle-sub'>New messages via companion server</div></div>"
"<label class='switch'><input type='checkbox' id='notif_wa' checked>"
"<span class='slider'></span></label></div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>📧 Email</div>"
"<div class='toggle-sub'>Unread emails via companion server</div></div>"
"<label class='switch'><input type='checkbox' id='notif_em' checked>"
"<span class='slider'></span></label></div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>🎵 Spotify</div>"
"<div class='toggle-sub'>Now playing &amp; playback controls</div></div>"
"<label class='switch'><input type='checkbox' id='notif_sp' checked>"
"<span class='slider'></span></label></div>"
"</div>"
"</div>"

/* ── Page 4: Advanced ────────────────────────────────────── */
"<div class='page' id='p4'>"

"<div class='card'>"
"<div class='card-title'>🔊 Audio</div>"
"<label>Speaker Volume (0–100)</label>"
"<input id='volume' type='number' min='0' max='100' value='75'>"
"<label>VAD Threshold (100–2000)</label>"
"<input id='vad_thresh' type='number' min='100' max='2000' value='300'>"
"<p class='hint'>Voice activity detection sensitivity. Lower = more sensitive. "
"Default 300. Increase if Debbie triggers on background noise.</p>"
"</div>"

"<div class='card'>"
"<div class='card-title'>📷 Camera</div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>Enable Camera</div>"
"<div class='toggle-sub'>OV2640 — vision queries use gpt-4o</div></div>"
"<label class='switch'><input type='checkbox' id='cam_en' checked>"
"<span class='slider'></span></label></div>"
"<br>"
"<a href='/snapshot' target='_blank' style='color:#06d6a0;font-size:.85rem'>"
"📸 Test Camera Snapshot →</a>"
"</div>"

"<div class='card'>"
"<div class='card-title'>ℹ️ Device Status</div>"
"<div class='status-bar' id='status_bar'>Loading...</div>"
"</div>"

"<div class='card'>"
"<div class='card-title'>🗑️ Factory Reset</div>"
"<p style='font-size:.8rem;color:#777;margin-bottom:12px'>"
"Erase all saved settings and reboot. You will need to reconfigure everything.</p>"
"<button class='btn-reset' onclick='doReset()'>🗑️ Factory Reset</button>"
"</div>"
"</div>"

/* ── Page 5: Memory & RAG ────────────────────────────────── */
"<div class='page' id='p5'>"

"<div class='card'>"
"<div class='card-title'>🧠 Conversation Memory</div>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>Enable Memory</div>"
"<div class='toggle-sub'>Remember recent conversations and facts across sessions</div></div>"
"<label class='switch'><input type='checkbox' id='mem_en' checked>"
"<span class='slider'></span></label></div><br>"
"<div class='toggle-row'>"
"<div><div class='toggle-label'>Companion RAG</div>"
"<div class='toggle-sub'>Query companion server for richer context retrieval</div></div>"
"<label class='switch'><input type='checkbox' id='mem_rag' checked>"
"<span class='slider'></span></label></div><br>"
"<label>Recent turns to keep in memory (5–50)</label>"
"<input id='mem_turns' type='number' min='5' max='50' value='20'>"
"<p class='hint'>Stored in NVS flash; survives reboots. Companion server stores unlimited history.</p>"
"</div>"

"<div class='card'>"
"<div class='card-title'>📊 Memory Stats</div>"
"<div class='status-bar' id='mem_stats'>Loading...</div>"
"<br><button class='btn' style='background:#c0392b;color:#fff' onclick='clearMem()'>"
"🗑️ Clear All Memory</button>"
"</div>"

"<div class='card'>"
"<div class='card-title'>💡 How Debbie's Memory Works</div>"
"<p style='font-size:.8rem;color:#999;line-height:1.6'>"
"<b style='color:#06d6a0'>Short-term:</b> Last 20 turns (user + AI) kept in RAM "
"and injected into every new conversation as context, so Debbie always knows what "
"was just discussed.<br><br>"
"<b style='color:#06d6a0'>Long-term facts:</b> You can say <em>'Remember that my name is Alice'</em> "
"or <em>'Remember I have a cat named Pixel'</em> and Debbie will save it to NVS flash "
"— it persists across reboots.<br><br>"
"<b style='color:#06d6a0'>Companion RAG:</b> When a companion server is configured, "
"all conversation history is stored there in a searchable SQLite database. When you ask "
"something, relevant past conversations are retrieved and added to the AI context "
"— making Debbie smarter over time.<br><br>"
"<b style='color:#06d6a0'>Voice commands:</b> <em>'Remember that...'</em> / "
"<em>'What do you know about me?'</em> / <em>'Forget everything'</em>"
"</p>"
"</div>"
"</div>"

/* Sticky save bar */
"<div class='btn-bar'>"
"<button class='btn-save' onclick='save()'>💾 Save All Settings</button>"
"</div>"
"<div id='msg' class='msg' style='position:fixed;bottom:64px;left:12px;right:12px;z-index:200'></div>"

"<script>"
/* ── Provider selection ─── */
"var PROVIDERS=['openai','anthropic','groq','openrouter','ollama'];"
"var MODELS={"
" openai:'gpt-4o',"
" anthropic:'claude-3-5-sonnet-20241022',"
" groq:'llama-3.3-70b-versatile',"
" openrouter:'',"
" ollama:'llama3'"
"};"
"function selectProvider(p){"
" PROVIDERS.forEach(function(x){"
"  var c=document.getElementById('pcard_'+x);"
"  if(c)c.className='llm-card'+(x===p?' sel':'');"
"  var d=document.getElementById('card_'+x);"
"  if(d)d.style.display=(x===p?'':'none');"
" });"
" document.getElementById('llm_provider').value=p;"
"}"

/* ── Tab switching ─── */
"var tabs=document.querySelectorAll('.tab');"
"var pages=document.querySelectorAll('.page');"
"function showTab(i){"
" tabs.forEach(function(t,j){t.className='tab'+(i===j?' active':'');});"
" pages.forEach(function(p,j){p.className='page'+(i===j?' active':'');});"
" if(i===4)loadStatus();"
" if(i===5)loadMemStats();"
"}"

/* ── Helpers ─── */
"function g(id){var el=document.getElementById(id);if(!el)return '';return el.value||'';}"
"function gc(id){var el=document.getElementById(id);return el?el.checked:false;}"
"function s(id,v){var el=document.getElementById(id);if(el&&v!==undefined&&v!==null)el.value=v;}"
"function sc(id,v){var el=document.getElementById(id);if(el)el.checked=!!v;}"

/* ── Get model string based on provider ─── */
"function getModel(){"
" var p=g('llm_provider');"
" if(p==='openai')return g('oai_model');"
" if(p==='anthropic')return g('anthropic_model');"
" if(p==='groq')return g('groq_model');"
" if(p==='openrouter')return g('or_model');"
" if(p==='ollama')return g('local_llm_model');"
" return '';"
"}"

/* ── Save ─── */
"function save(){"
" var p=g('llm_provider');"
" var d={"
"  ssid:g('ssid'),pass:g('pass'),"
"  ssid2:g('ssid2'),pass2:g('pass2'),"
"  ssid3:g('ssid3'),pass3:g('pass3'),"
"  bt_en:gc('bt_en'),ble_name:g('ble_name'),"
"  companion_url:g('companion_url'),"
"  llm_provider:p,llm_model:getModel(),"
"  oai_key:g('oai_key'),"
"  anthropic_key:g('anthropic_key'),"
"  groq_key:g('groq_key'),"
"  or_key:g('or_key'),"
"  local_llm_url:g('local_llm_url'),"
"  local_llm_model:g('local_llm_model'),"
"  agent_url:g('agent_url'),use_agent:gc('use_agent'),"
"  name:g('name'),sys_prompt:g('sys_prompt'),"
"  voice_style:g('voice_style'),resp_len:g('resp_len'),"
"  notifs_en:gc('notifs_en'),notif_wa:gc('notif_wa'),"
"  notif_em:gc('notif_em'),notif_sp:gc('notif_sp'),"
"  volume:parseInt(g('volume'))||75,"
"  vad_threshold:parseInt(g('vad_thresh'))||300,"
"  cam_en:gc('cam_en'),"
"  mem_en:gc('mem_en'),"
"  mem_rag:gc('mem_rag'),"
"  mem_turns:parseInt(g('mem_turns'))||20"
" };"
" fetch('/configure',{method:'POST',"
"  headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})"
" .then(function(r){return r.json();})"
" .then(function(j){showMsg(j.ok?'ok':'err',j.msg||'Saved!');loadStatus();})"
" .catch(function(e){showMsg('err',''+e);});"
"}"

/* ── Reset ─── */
"function doReset(){"
" if(!confirm('Factory reset Debbie? All settings will be erased.'))return;"
" fetch('/reset',{method:'POST'})"
" .then(function(){showMsg('ok','Resetting and rebooting...');});"
"}"

/* ── Default prompt ─── */
"var DEF_PROMPT='You are Debbie, a warm and knowledgeable AI companion. "
"You have a friendly, upbeat personality and love helping with anything. "
"Speak naturally and concisely.';"
"function resetPrompt(){s('sys_prompt',DEF_PROMPT);}"

/* ── Message ─── */
"function showMsg(cls,txt){"
" var el=document.getElementById('msg');"
" el.className='msg '+cls;el.textContent=txt;"
" setTimeout(function(){el.className='msg';},4000);"
"}"

/* ── Status ─── */
"function loadStatus(){"
" fetch('/status').then(function(r){return r.json();})"
" .then(function(j){"
"  var sb=document.getElementById('status_bar');"
"  if(!sb)return;"
"  sb.innerHTML="
"   '<span><span class=\"dot '+(j.wifi_ok?'green':'red')+'\" ></span>WiFi: '+(j.wifi_ok?'Connected':'Offline')+'</span>'"
"   +'<span><span class=\"dot '+(j.bt_on?'blue':'red')+'\" ></span>BLE: '+(j.bt_on?'On':'Off')+'</span>'"
"   +'<span>State: '+j.state+'</span>'"
"   +'<span>LLM: '+j.llm_provider+'/'+j.llm_model+'</span>';"
"  /* pre-fill fields from config */"
"  if(j.ssid)s('ssid',j.ssid);"
"  if(j.ssid2)s('ssid2',j.ssid2);"
"  if(j.ssid3)s('ssid3',j.ssid3);"
"  if(j.ble_name)s('ble_name',j.ble_name);"
"  sc('bt_en',j.bt_en);"
"  if(j.companion_url)s('companion_url',j.companion_url);"
"  if(j.llm_provider){selectProvider(j.llm_provider);}"
"  if(j.llm_model){"
"   s('oai_model',j.llm_model);"
"   s('anthropic_model',j.llm_model);"
"   s('groq_model',j.llm_model);"
"   s('or_model',j.llm_model);"
"   s('local_llm_model',j.llm_model);"
"  }"
"  if(j.local_llm_url)s('local_llm_url',j.local_llm_url);"
"  if(j.agent_url)s('agent_url',j.agent_url);"
"  sc('use_agent',j.use_agent);"
"  if(j.name)s('name',j.name);"
"  if(j.sys_prompt)s('sys_prompt',j.sys_prompt);"
"  if(j.voice_style)s('voice_style',j.voice_style);"
"  if(j.resp_len)s('resp_len',j.resp_len);"
"  sc('notifs_en',j.notifs_en);"
"  sc('notif_wa',j.notif_wa);"
"  sc('notif_em',j.notif_em);"
"  sc('notif_sp',j.notif_sp);"
"  if(j.volume!=null)s('volume',j.volume);"
"  if(j.vad_threshold!=null)s('vad_thresh',j.vad_threshold);"
"  sc('cam_en',j.cam_en);"
"  sc('mem_en',j.mem_en);"
"  sc('mem_rag',j.mem_rag);"
"  if(j.mem_turns!=null)s('mem_turns',j.mem_turns);"
" }).catch(function(){});"
"}"

/* ── Memory stats & clear ─── */
"function loadMemStats(){"
" var sb=document.getElementById('mem_stats');"
" if(sb)sb.innerHTML='Loading...';"
" fetch('/memory_stats').then(function(r){return r.json();})"
" .then(function(j){"
"  if(!sb)return;"
"  sb.innerHTML="
"   '<span>💬 Turns: '+j.turn_count+'</span>'"
"   +'<span>📌 Facts: '+j.fact_count+'</span>'"
"   +'<span style=\"color:'+(j.companion_rag?'#06d6a0':'#777')+'\">"
"   🔍 Companion RAG: '+(j.companion_rag?'Active':'Off')+'</span>';"
" }).catch(function(){if(sb)sb.innerHTML='Stats unavailable';});"
"}"
"function clearMem(){"
" if(!confirm('Clear all Debbie memory? This cannot be undone.'))return;"
" fetch('/memory_clear',{method:'POST'})"
" .then(function(){showMsg('ok','Memory cleared!');loadMemStats();})"
" .catch(function(e){showMsg('err',''+e);});"
"}"

"loadStatus();"
"</script></body></html>";

/* ── Handlers ──────────────────────────────────────────────────────────── */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SETUP_HTML, strlen(SETUP_HTML));
    return ESP_OK;
}

static esp_err_t handler_status(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "name",         g_debbie_config.debbie_name);
    cJSON_AddStringToObject(json, "sys_prompt",   g_debbie_config.system_prompt);
    cJSON_AddStringToObject(json, "agent_url",    g_debbie_config.agent_ws_url);
    cJSON_AddStringToObject(json, "companion_url",g_debbie_config.companion_url);
    cJSON_AddStringToObject(json, "llm_provider", g_debbie_config.llm_provider);
    cJSON_AddStringToObject(json, "llm_model",    g_debbie_config.llm_model);
    cJSON_AddStringToObject(json, "local_llm_url",g_debbie_config.local_llm_url);
    cJSON_AddStringToObject(json, "ssid",         g_debbie_config.wifi_ssid);
    cJSON_AddStringToObject(json, "ssid2",        g_debbie_config.wifi_ssid2);
    cJSON_AddStringToObject(json, "ssid3",        g_debbie_config.wifi_ssid3);
    cJSON_AddStringToObject(json, "ble_name",     g_debbie_config.ble_device_name);
    cJSON_AddStringToObject(json, "voice_style",  g_debbie_config.voice_style);
    cJSON_AddStringToObject(json, "resp_len",     g_debbie_config.response_length);
    cJSON_AddNumberToObject(json, "volume",       g_debbie_config.speaker_volume);
    cJSON_AddNumberToObject(json, "vad_threshold",g_debbie_config.vad_threshold);
    cJSON_AddBoolToObject(json,   "wifi_ok",      wifi_manager_is_connected());
    cJSON_AddBoolToObject(json,   "bt_en",        g_debbie_config.bluetooth_enabled);
    cJSON_AddBoolToObject(json,   "bt_on",        g_debbie_config.bluetooth_enabled);
    cJSON_AddBoolToObject(json,   "use_agent",    g_debbie_config.use_custom_agent);
    cJSON_AddBoolToObject(json,   "notifs_en",    g_debbie_config.notifications_enabled);
    cJSON_AddBoolToObject(json,   "notif_wa",     g_debbie_config.notif_whatsapp);
    cJSON_AddBoolToObject(json,   "notif_em",     g_debbie_config.notif_email);
    cJSON_AddBoolToObject(json,   "notif_sp",     g_debbie_config.notif_spotify);
    cJSON_AddBoolToObject(json,   "cam_en",       g_debbie_config.camera_enabled);
    cJSON_AddBoolToObject(json,   "mem_en",       g_debbie_config.memory_enabled);
    cJSON_AddBoolToObject(json,   "mem_rag",      g_debbie_config.memory_rag_enabled);
    cJSON_AddNumberToObject(json, "mem_turns",    g_debbie_config.memory_max_turns);
    cJSON_AddStringToObject(json, "state",
        g_debbie_state == DEBBIE_STATE_IDLE      ? "idle"      :
        g_debbie_state == DEBBIE_STATE_LISTENING ? "listening" :
        g_debbie_state == DEBBIE_STATE_THINKING  ? "thinking"  :
        g_debbie_state == DEBBIE_STATE_SPEAKING  ? "speaking"  : "other");

    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

static esp_err_t handler_configure(httpd_req_t *req)
{
    char *buf = malloc(req->content_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

#define GET_STR(key, dst) do { \
    cJSON *j = cJSON_GetObjectItem(json, key); \
    if (j && cJSON_IsString(j) && strlen(j->valuestring) > 0) \
        strncpy(dst, j->valuestring, sizeof(dst) - 1); \
} while (0)

#define GET_BOOL(key, dst) do { \
    cJSON *j = cJSON_GetObjectItem(json, key); \
    if (j) dst = cJSON_IsTrue(j); \
} while (0)

    /* Network */
    GET_STR("ssid",          g_debbie_config.wifi_ssid);
    GET_STR("pass",          g_debbie_config.wifi_password);
    GET_STR("ssid2",         g_debbie_config.wifi_ssid2);
    GET_STR("pass2",         g_debbie_config.wifi_password2);
    GET_STR("ssid3",         g_debbie_config.wifi_ssid3);
    GET_STR("pass3",         g_debbie_config.wifi_password3);
    GET_STR("ble_name",      g_debbie_config.ble_device_name);
    GET_STR("companion_url", g_debbie_config.companion_url);
    GET_BOOL("bt_en",        g_debbie_config.bluetooth_enabled);

    /* LLM */
    GET_STR("llm_provider",  g_debbie_config.llm_provider);
    GET_STR("llm_model",     g_debbie_config.llm_model);
    GET_STR("oai_key",       g_debbie_config.openai_api_key);
    GET_STR("anthropic_key", g_debbie_config.anthropic_api_key);
    GET_STR("groq_key",      g_debbie_config.groq_api_key);
    GET_STR("or_key",        g_debbie_config.openrouter_api_key);
    GET_STR("local_llm_url", g_debbie_config.local_llm_url);
    GET_STR("local_llm_model",g_debbie_config.local_llm_model);
    GET_STR("agent_url",     g_debbie_config.agent_ws_url);
    GET_BOOL("use_agent",    g_debbie_config.use_custom_agent);

    /* Personality */
    GET_STR("name",          g_debbie_config.debbie_name);
    GET_STR("sys_prompt",    g_debbie_config.system_prompt);
    GET_STR("voice_style",   g_debbie_config.voice_style);
    GET_STR("resp_len",      g_debbie_config.response_length);

    /* Notifications */
    GET_BOOL("notifs_en",    g_debbie_config.notifications_enabled);
    GET_BOOL("notif_wa",     g_debbie_config.notif_whatsapp);
    GET_BOOL("notif_em",     g_debbie_config.notif_email);
    GET_BOOL("notif_sp",     g_debbie_config.notif_spotify);

    /* Advanced */
    cJSON *vol = cJSON_GetObjectItem(json, "volume");
    if (vol && cJSON_IsNumber(vol))
        g_debbie_config.speaker_volume = (uint8_t)vol->valuedouble;

    cJSON *vad = cJSON_GetObjectItem(json, "vad_threshold");
    if (vad && cJSON_IsNumber(vad))
        g_debbie_config.vad_threshold = (uint16_t)vad->valuedouble;

    GET_BOOL("cam_en",       g_debbie_config.camera_enabled);

    /* Memory */
    GET_BOOL("mem_en",       g_debbie_config.memory_enabled);
    GET_BOOL("mem_rag",      g_debbie_config.memory_rag_enabled);
    {
        cJSON *mt = cJSON_GetObjectItem(json, "mem_turns");
        if (mt && cJSON_IsNumber(mt) && mt->valuedouble >= 5 && mt->valuedouble <= 50)
            g_debbie_config.memory_max_turns = (uint8_t)mt->valuedouble;
    }

    cJSON_Delete(json);
    storage_save_config();

    const char *resp = "{\"ok\":true,\"msg\":\"Settings saved!\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    /* Reconnect with new credentials in background */
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconnect();

    return ESP_OK;
}

static esp_err_t handler_reset(httpd_req_t *req)
{
    storage_factory_reset();
    httpd_resp_send(req, "{\"ok\":true}", strlen("{\"ok\":true}"));
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_snapshot(httpd_req_t *req)
{
    if (!g_debbie_config.camera_enabled) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Camera disabled");
        return ESP_FAIL;
    }
    uint8_t *jpeg = NULL;
    size_t   len  = 0;
    if (camera_manager_capture_jpeg(&jpeg, &len) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (char *)jpeg, len);
    camera_manager_free_frame(jpeg);
    return ESP_OK;
}

static esp_err_t handler_memory_stats(httpd_req_t *req)
{
    int fact_count = 0;
    memory_manager_get_facts(&fact_count);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "turn_count",   memory_manager_turn_count());
    cJSON_AddNumberToObject(json, "fact_count",   fact_count);
    cJSON_AddBoolToObject(json,   "memory_enabled", g_debbie_config.memory_enabled);
    cJSON_AddBoolToObject(json,   "companion_rag",
        g_debbie_config.memory_rag_enabled &&
        strlen(g_debbie_config.companion_url) > 0);
    char *str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, str, strlen(str));
    free(str);
    return ESP_OK;
}

static esp_err_t handler_memory_clear(httpd_req_t *req)
{
    memory_manager_clear();
    const char *resp = "{\"ok\":true,\"msg\":\"Memory cleared\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

static httpd_handle_t s_server = NULL;

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.stack_size       = 12288;   /* increased for BLE + WiFi coexistence overhead */

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = handler_root          },
        { .uri = "/status",       .method = HTTP_GET,    .handler = handler_status        },
        { .uri = "/configure",    .method = HTTP_POST,   .handler = handler_configure     },
        { .uri = "/reset",        .method = HTTP_POST,   .handler = handler_reset         },
        { .uri = "/snapshot",     .method = HTTP_GET,    .handler = handler_snapshot      },
        { .uri = "/memory_stats", .method = HTTP_GET,    .handler = handler_memory_stats  },
        { .uri = "/memory_clear", .method = HTTP_POST,   .handler = handler_memory_clear  },
    };

    for (int i = 0; i < (int)(sizeof(routes) / sizeof(routes[0])); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &routes[i]));
    }

    ESP_LOGI(TAG, "Web server started at http://%s/", DEBBIE_AP_IP);
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
