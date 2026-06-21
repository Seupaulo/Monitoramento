/* * VERSÃO: B1.29 (LOCAL_CONFIG + KEEP_ALIVE)
 * - FIX 1 (B1.21): Tabela de Hardware mostra texto dinâmico (BOM/MED/RUIM).
 * - FIX 2 (B1.21): Guarda de heap antes do BearSSL em sendTelegram().
 * - FIX 3 (B1.22): ultimoDiaEnviado movido pra RTC (evita relatório duplo perto das 9h).
 * - FIX 4 (B1.22): Estado de queda persistido na RTC — sobrevive a reset durante queda.
 * - FIX 5 (B1.22): Retry de ping ACUMULA tentativas em vez de SUBSTITUIR.
 * - B1.22 (Portal): indicador de dados desatualizados, countdown pro reset diário,
 *          badge "quem está ganhando", histórico visual recente (sparkline),
 *          tempo offline e última queda nas estatísticas.
 * - B1.22 (Telegram): quedas + tempo offline no relatório, pico do dia, data no
 *          cabeçalho, pontuação pondera uptime mais que ping/jitter/perda.
 * - FIX 6 (B1.23): histórico recente distingue "Offline" de "Alerta" por cor
 *          (antes os dois usavam o mesmo vermelho).
 * - FIX 7 (B1.23): handleRoot() refatorado de String acumulada (+=) pra snprintf
 *          em buffers fixos nas 3 tabelas + sparkline — reduz fragmentação de
 *          heap. Floats são formatados via fmtFloat() (usa String internamente),
 *          NUNCA via snprintf("%f",...) — isso não funciona por padrão no ESP8266.
 * - B1.24 (Otimizações & SLA):
 *          - Resolvido travamento infinito no sync NTP em setup() com timeout de 15s.
 *          - Corrigido uso de abs() para floats (agora fabs()), normalizando Jitter e placar.
 *          - Struct RTC padronizada com tipos fixos (__attribute__((packed, aligned(4)))) e tamanho múltiplo de 4.
 *          - histV e histT salvos no RTC (histórico recente sobrevive a reboots).
 *          - sendTelegram otimizado para POST no body via stream (evita Strings gigantes na RAM).
 *          - Otimizadas funções de cores e ícones para const char* (reduz uso de heap).
 *          - Evitado travamento do web server com yieldDelay() in vez de delay() no retry de pings.
 *          - Logs do sistema e lag reescritos usando circular buffers.
 *          - Ajustados os limites de SLA (EXCELENTE, BOM, REGULAR, RUIM) realistas para o Brasil.
 *          - Reset diário simplificado imediato após o relatório das 09h.
 *          - Otimizado MQTT para fazer apenas 1 conexão por ciclo se desconectado.
 * - B1.25 (OTA & SLA Fibra):
 *          - Implementada atualização via Web OTA em "/update" com autenticação (admin / WIFI_PASS).
 *          - Adicionado link discreto para OTA no rodapé do portal.
 *          - Ajustados os limites de SLA (Ping/Jitter) otimizados para conexões Fibra de baixa latência.
 * - B1.26 (Custom OTA):
 *          - Removida a biblioteca padrão ESP8266HTTPUpdateServer para poupar Flash.
 *          - Implementado portal OTA customizado em HTML/CSS integrado ao tema escuro.
 *          - Removida a opção redundante de "Filesystem", restando apenas "Firmware" (upload de .bin).
 * - B1.27 (Ampliação Sparkline):
 *          - Aumentado o histórico recente (HIST_SIZE) para 30 itens (30 minutos de log visual).
 *          - Adicionado padding[2] na struct rtcData para preservar o alinhamento de 4 bytes (tamanho total de 172 bytes).
 *          - Reduzidos os blocos para 9px com gap de 2px para garantir responsividade perfeita em todas as telas.
 *          - Alterado o RTC_MAGIC para forçar a re-inicialização limpa das arrays no boot.
 * - B1.29 (Configuração Local Dinâmica):
 *          - Aumentados os slots para 10.
 *          - Adicionados botões interativos Adicionar e Excluir.
 *          - Correção definitiva do salvamento do formulário de configuração.
 *          - Remoção completa de referências e códigos residuais de varredura (IP Scan).
 */

ADC_MODE(ADC_VCC);

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266WebServer.h> 
#include <ESP8266Ping.h> 
#include <time.h> 
#include <math.h> // Para fabs()
#include <Updater.h> // Para atualizações OTA personalizadas
#include <EEPROM.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// ======== 1. CONFIGURAÇÕES (PREENCHA AQUI) ========
const char* WIFI_SSID = "Home";
const char* WIFI_PASS = "1f4456ccd9T$"; // <- troque após revogar credenciais antigas

// VERSÃO E CONFIGURAÇÕES DE ATUALIZAÇÃO ONLINE (NUVEM)
const char* VERSAO = "B1.30";
const char* OTA_VERSION_URL = "https://raw.githubusercontent.com/Seupaulo/monitoramento/main/version.txt";
const char* OTA_BIN_URL     = "https://raw.githubusercontent.com/Seupaulo/monitoramento/main/firmware.bin";

// TELEGRAM
const String BOT_TOKEN = "5429472706:AAF1C5_i4bEPJiiqG5Fyayjv3ynXwgEU9oY"; // <- gerar novo via @BotFather (/revoke)
const String CHAT_ID = "171747541";

// ADAFRUIT IO
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883                   
#define AIO_USERNAME    "seupaulo"
#define AIO_KEY         "aio_XsUR78deKbX5Ss33BvKKeJSGhE5p" // <- gerar nova chave no Adafruit IO

// LINKS DOS DASHBOARDS (Para os Hiperlinks)
#define AIO_DASHBOARD_30D "https://io.adafruit.com/seupaulo/dashboards/links?kiosk=true"
#define AIO_DASHBOARD_24H "https://io.adafruit.com/seupaulo/dashboards/24h?kiosk=true"

// IPs de Teste
const char* IP_VIVO = "8.8.8.8"; 
const char* IP_TIM  = "8.8.4.4"; 

ESP8266WebServer server(80);

// Configuração persistente de IPs Locais (EEPROM)
struct __attribute__((packed, aligned(4))) {
  uint32_t magic;              // 0xEEBB0016
  char label[10][16];          // Nomes dos 10 dispositivos (ex: "Roteador", "Impressora")
  char ip[10][16];             // IPs correspondentes (ex: "192.168.1.1")
  uint8_t alertTelegram[10];   // 1 = Alerta Telegram ativo, 0 = Inativo
} configData;

// Variáveis voláteis do monitor local
float pLocal[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
bool okLocal[10] = {false, false, false, false, false, false, false, false, false, false};
bool bootLocalPingDone = false;

// Cliente MQTT
WiFiClient client;
Adafruit_MQTT_Client mqtt(&client, AIO_SERVER, AIO_SERVERPORT, AIO_USERNAME, AIO_KEY);

// Definição dos Feeds
Adafruit_MQTT_Publish feedVivoPing   = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/vivo-ping");
Adafruit_MQTT_Publish feedVivoJitter = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/vivo-jitter");
Adafruit_MQTT_Publish feedVivoPerda  = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/vivo-perda");

Adafruit_MQTT_Publish feedTimPing    = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/tim-ping");
Adafruit_MQTT_Publish feedTimJitter  = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/tim-jitter");
Adafruit_MQTT_Publish feedTimPerda   = Adafruit_MQTT_Publish(&mqtt, AIO_USERNAME "/feeds/tim-perda");

// ======== 2. ESTRUTURA RTC E VARIÁVEIS ========
#define HIST_SIZE 30

// Struct empacotada e alinhada a 4 bytes (tamanho exato: 172 bytes, múltiplo de 4)
struct __attribute__((packed, aligned(4))) {
  uint32_t magic;
  uint8_t webReset;
  uint8_t resetFeitoHoje;
  uint8_t vivoCaido;
  uint8_t timCaido;
  
  int32_t testesV;
  int32_t onlineV;
  int32_t quedasV;
  int32_t instabV;
  
  int32_t testesT;
  int32_t onlineT;
  int32_t quedasT;
  int32_t instabT;
  
  uint32_t tempoTotalOffV;
  uint32_t tempoTotalOffT;
  int32_t telegramCount;
  
  int32_t pkgEnviadosV;
  int32_t pkgPerdidosV;
  int32_t pkgEnviadosT;
  int32_t pkgPerdidosT;
  
  int32_t ultimoDiaEnviadoRTC;
  uint32_t ultimoCheckOTA; // Guarda a data da última checagem de firmware online
  
  uint64_t inicioQuedaVivo;
  uint64_t inicioQuedaTim;
  
  char hUltimaQuedaV[9];
  char hUltimaQuedaT[9];
  
  int8_t histV[HIST_SIZE];
  int8_t histT[HIST_SIZE];
  uint8_t padding[2]; // Ajusta o alinhamento
} rtcData;

#define RTC_MAGIC 0xFF55AA18 // Alterado para invalidar struct antiga e reinicializar no boot

uint32_t heapNoBoot = 0; 
String horaBoot = "--:--:--"; 
String ultimaAtualizacao = "Aguardando..."; 
String motivoReset = ""; 
bool sistemaPausado = false; 
bool modoNaoPerturbe = false; 
unsigned long ultimoTeste = 0; 

// Variáveis Voláteis 
float pV=0, jV=0, lossPctV=0; int lastLostV=0; 
float pT=0, jT=0, lossPctT=0; int lastLostT=0; 

// Nomes de Status e Cores
String sV="Init", sT="Init";
const char *cV="#fff", *cT="#fff", *cJV="#fff", *cJT="#fff", *cLV="#fff", *cLT="#fff";

// Logs Circulares (evitam cópia e fragmentação de Strings)
String logSys[5] = {"-", "-", "-", "-", "-"};
int logSysIdx = 0;
String logLag[5] = {"-", "-", "-", "-", "-"};
int logLagIdx = 0;

time_t iniV=0, iniT=0; String hIniV="-", hIniT="-";
bool linkVivoOff=false, linkTimOff=false; 

// Estatísticas
float piorPV=0; String hPiorPV="-"; float piorJV=0; String hPiorJV="-";
float piorPT=0; String hPiorPT="-"; float piorJT=0; String hPiorJT="-";

float melhorPV=-1; String hMelhorPV="-"; float melhorJV=-1; String hMelhorJV="-";
float melhorPT=-1; String hMelhorPT="-"; float melhorJT=-1; String hMelhorJT="-";

double somaPV=0, somaJV=0; long contaV=0;
double somaPT=0, somaJT=0; long contaT=0;

String ultimoStatusTelegram = "Pronto"; String corStatusTelegram = "#fff";
String ultimoStatusMQTT = "Aguardando"; String corStatusMQTT = "#fff";

// Definição de Cores e Constantes
const char* COR_BOM="#4CAF50"; 
const char* COR_MED="#FFC107"; 
const char* COR_PES="#F44336"; 

const char* C_EXCELENTE="#00E5FF"; // Ciano
const char* C_BOM="#4CAF50";       // Verde Claro
const char* C_REGULAR="#FFC107";   // Amarelo
const char* C_ALERTA="#FF5252";   // Vermelho Claro
const char* C_OFFLINE="#8B0000";  // Vermelho escuro

// IDs de SLA
#define SLA_OFF 0
#define SLA_EXCELENTE 1
#define SLA_BOM 2
#define SLA_REGULAR 3
#define SLA_ALERTA 4

// Limiares de Comparação do Placar
#define LIMIAR_UPTIME 0.5   // %
#define LIMIAR_PING   5.0   // ms
#define LIMIAR_JITTER 2.0   // ms
#define LIMIAR_PERDA  1.0   // %

// ======== 3. AUXILIARES & LOGS ========
void saveData() { 
  rtcData.magic = RTC_MAGIC; 
  ESP.rtcUserMemoryWrite(0, (uint32_t*)&rtcData, sizeof(rtcData)); 
}

void logSistema(String msg) {
  logSys[logSysIdx] = "[" + ultimaAtualizacao.substring(0,5) + "] " + msg; 
  logSysIdx = (logSysIdx + 1) % 5;
}

void logInstab(String msg) {
  logLag[logLagIdx] = "[" + ultimaAtualizacao.substring(0,5) + "] " + msg;
  logLagIdx = (logLagIdx + 1) % 5;
}

// Funções de cores e ícones otimizadas para const char* (evitam alocação na Heap)
const char* getCorPing(float val) { 
  if(val<=0) return "#fff"; 
  if(val<=28) return C_EXCELENTE; 
  if(val<=35) return C_BOM; 
  if(val<=55) return C_REGULAR; 
  return C_ALERTA; 
}

const char* getCorJitter(float val) { 
  if(val<=0) return "#fff"; 
  if(val<=4) return C_EXCELENTE; 
  if(val<=8) return C_BOM; 
  if(val<=15) return C_REGULAR; 
  return C_ALERTA; 
}

const char* getCorLoss(float pct) { 
  if(pct==0) return C_EXCELENTE; 
  if(pct<=20) return C_REGULAR; 
  return C_ALERTA; 
} 

const char* getCorHeap(uint32_t val) { 
  if(val>20000) return COR_BOM; 
  if(val>15000) return COR_MED; 
  return COR_PES; 
}

const char* getCorFrag(int val) { 
  if(val<20) return COR_BOM; 
  if(val<40) return COR_MED; 
  return COR_PES; 
}

const char* getCorSignal(int pct) { 
  if(pct>70) return COR_BOM; 
  if(pct>40) return COR_MED; 
  return COR_PES; 
}

const char* getCorUptime(float val) { 
  if(val>=99.0) return COR_BOM; 
  if(val>=95.0) return COR_MED; 
  return COR_PES; 
}

const char* getTextoStatus(const char* cor) {
  if (cor == COR_BOM) return "BOM";
  if (cor == COR_MED) return "MED";
  return "RUIM";
}

String traduzirReset(String raw) {
  if (rtcData.magic == RTC_MAGIC) {
    if (rtcData.webReset == 1) return "Portal Web";
    if (rtcData.webReset == 2) return "Atualização OTA";
  }
  if (raw == "Power on") return "Energia (Cabo)";
  return raw;
}

String fmtTempo(unsigned long seg) {
  if(seg == 0) return "0s";
  int h = seg / 3600; int m = (seg % 3600) / 60; int s = seg % 60;
  char b[25]; sprintf(b, "%02dh %02dm %02ds", h, m, s);
  return String(b);
}

String urlEncode(String str) {
  String res = "";
  res.reserve(str.length() * 1.2); // Pre-aloca memória para evitar fragmentação
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) res += c; 
    else if (c == ' ') res += "%20"; 
    else if (c == '\n') res += "%0A";
    else { 
      char buf[4]; 
      sprintf(buf, "%%%02X", (unsigned char)c); 
      res += buf; 
    }
  }
  return res;
}

void fmtFloat(float val, int decimais, char* destino, size_t tamDestino) {
  String(val, decimais).toCharArray(destino, tamDestino);
}

void addHistorico(int8_t* hist, int valorSLA) {
  for (int i = HIST_SIZE - 1; i > 0; i--) hist[i] = hist[i - 1];
  hist[0] = (int8_t)valorSLA;
}

// Delay não bloqueante: mantém o Web Server ativo e responsivo
void yieldDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    server.handleClient();
    delay(1);
  }
}

// ======== 4. LÓGICA DE CLASSIFICAÇÃO (SISTEMA DE IDs) ========
int calcularID_SLA(float p, float j, float loss) {
  if (p <= 0) return SLA_OFF; 
  if (p <= 28 && j <= 4 && loss <= 1.0) return SLA_EXCELENTE;
  if (p <= 35 && j <= 8 && loss <= 2.0) return SLA_BOM;
  if (p <= 55 && j <= 15 && loss <= 20) return SLA_REGULAR;
  return SLA_ALERTA;
}

const char* getSLANome(int id) {
  if (id == SLA_EXCELENTE) return "EXCELENTE";
  if (id == SLA_BOM) return "BOM";
  if (id == SLA_REGULAR) return "REGULAR";
  if (id == SLA_ALERTA) return "RUIM";
  return "OFF";
}

const char* getSLAIcone(int id) {
  if (id == SLA_EXCELENTE) return "🟢";
  if (id == SLA_BOM) return "🔵";
  if (id == SLA_REGULAR) return "🟡";
  if (id == SLA_ALERTA) return "🔴";
  return "🚫";
}

const char* getSLACor(int id) {
  if (id == SLA_EXCELENTE) return C_EXCELENTE;
  if (id == SLA_BOM) return C_BOM;
  if (id == SLA_REGULAR) return C_REGULAR;
  if (id == SLA_ALERTA) return C_ALERTA;
  return C_OFFLINE; 
}

void renderHistorico(int8_t* hist) {
  char buf[100];
  server.sendContent(F("<div style='display:flex;gap:2px;margin:4px 0 8px;'>"));
  for (int i = HIST_SIZE - 1; i >= 0; i--) {
    if (hist[i] >= 0) {
      const char* cor = getSLACor(hist[i]);
      snprintf(buf, sizeof(buf), "<div style='width:9px;height:9px;background:%s;border-radius:1px;'></div>", cor);
    } else {
      snprintf(buf, sizeof(buf), "<div style='width:9px;height:9px;background:var(--no-data-color);border-radius:1px;'></div>");
    }
    server.sendContent(buf);
  }
  server.sendContent(F("</div>"));
}

String calcularVencedor(float upV, float upT, float mPV, float mPT, float mJV, float mJT, float accLossV, float accLossT, int &ptsV, int &ptsT) {
  ptsV = 0; ptsT = 0;

  // Corrigido para fabs() para manter precisão de floats
  if (fabs(upV - upT) > LIMIAR_UPTIME) { if (upV > upT) ptsV += 2; else ptsT += 2; }
  if (fabs(mPV - mPT) > LIMIAR_PING) { if (mPV < mPT) ptsV++; else ptsT++; }
  if (fabs(mJV - mJT) > LIMIAR_JITTER) { if (mJV < mJT) ptsV++; else ptsT++; }
  if (fabs(accLossV - accLossT) > LIMIAR_PERDA) { if (accLossV < accLossT) ptsV++; else ptsT++; }

  if (ptsV == 0 && ptsT == 0) return "⚖️ Empate técnico (sem diferença relevante)";
  if (ptsV > ptsT) return "VIVO melhor hoje (" + String(ptsV) + "x" + String(ptsT) + ")";
  if (ptsT > ptsV) return "TIM melhor hoje (" + String(ptsT) + "x" + String(ptsV) + ")";
  return "⚖️ Empate técnico (" + String(ptsV) + "x" + String(ptsT) + ")";
}

// ======== 5. CONECTIVIDADE ========
bool novaVersaoDisponivel(String remota, String local) {
  remota.replace("B", "");
  local.replace("B", "");
  return remota.toFloat() > local.toFloat();
}

String obterVersaoOnline() {
  if (WiFi.status() != WL_CONNECTED) return "";
  
  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 512); // Pequeno buffer para economia de RAM (lendo apenas texto simples)
  client.setTimeout(4000);
  
  HTTPClient http;
  String payload = "";
  // Adiciona timestamp para evitar cache do GitHub CDN
  String url = String(OTA_VERSION_URL) + "?t=" + String(time(nullptr));
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
      payload.trim();
    }
    http.end();
  }
  return payload;
}

bool executarOTA() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  BearSSL::WiFiClientSecure client;
  client.setInsecure();
  // Removemos o limite fixo de buffer. Executando no boot com Heap limpo,
  // o BearSSL pode alocar buffers dinamicamente sem perigo de Out Of Memory.
  client.setTimeout(30000); // 30 segundos
  
  ESPhttpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = ESPhttpUpdate.update(client, OTA_BIN_URL);
  
  switch(ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("HTTP_UPDATE_FAILED Error (%d): %s\n", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
      return false;
    case HTTP_UPDATE_NO_UPDATES:
      return false;
    case HTTP_UPDATE_OK:
      return true;
  }
  return false;
}


void connectMQTT() {
  if (mqtt.connected()) return;
  // Faz apenas 1 tentativa rápida para não travar o loop do ESP se o Adafruit estiver fora
  if (mqtt.connect() == 0) {
     ultimoStatusMQTT = "OK"; corStatusMQTT = COR_BOM;
  } else {
     mqtt.disconnect();
     ultimoStatusMQTT = "Falha"; corStatusMQTT = COR_PES;
  }
}

// Envio otimizado de Telegram via requisição POST no corpo, escrito em chunks
void sendTelegram(String message) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (modoNaoPerturbe) return;

  if (ESP.getFreeHeap() < 12000) {
    ultimoStatusTelegram = "SemRAM";
    corStatusTelegram = COR_PES;
    logSistema("TG bloq: heap=" + String(ESP.getFreeHeap()));
    return;
  }

  String encodedMsg = urlEncode(message);
  BearSSL::WiFiClientSecure clientSSL; 
  clientSSL.setInsecure(); 
  clientSSL.setBufferSizes(2048, 512); 
  clientSSL.setTimeout(4000);
  
  if (clientSSL.connect("api.telegram.org", 443)) {
    // Calcula o tamanho exato do corpo do POST
    int bodyLen = 60 + CHAT_ID.length() + encodedMsg.length();

    // Envia o cabeçalho em blocos direto na rede
    clientSSL.print(F("POST /bot"));
    clientSSL.print(BOT_TOKEN);
    clientSSL.print(F("/sendMessage HTTP/1.1\r\n"));
    clientSSL.print(F("Host: api.telegram.org\r\n"));
    clientSSL.print(F("Content-Type: application/x-www-form-urlencoded\r\n"));
    clientSSL.print(F("Content-Length: "));
    clientSSL.println(bodyLen);
    clientSSL.print(F("Connection: close\r\n\r\n"));

    // Envia o corpo
    clientSSL.print(F("chat_id="));
    clientSSL.print(CHAT_ID);
    clientSSL.print(F("&parse_mode=HTML&disable_web_page_preview=true&text="));
    clientSSL.print(encodedMsg);

    unsigned long to = millis();
    while (clientSSL.connected() && !clientSSL.available() && (millis()-to < 3500)) delay(1);
    if (clientSSL.available() && clientSSL.readStringUntil('\n').startsWith("HTTP/1.1 200")) {
        rtcData.telegramCount++; saveData();
        ultimoStatusTelegram="OK"; corStatusTelegram=COR_BOM;
    } else {
        ultimoStatusTelegram="Erro"; corStatusTelegram=COR_MED;
    }
    clientSSL.stop();
  } else {
    ultimoStatusTelegram="Falha"; corStatusTelegram=COR_PES;
  }
}

// ======== 6. RELATÓRIOS ========
void enviarRelatorioCompleto(bool manual) {
    time_t agora = time(nullptr); struct tm* t = localtime(&agora);
    char buf[10]; strftime(buf, 10, "%H:%M", t); String horaAtual = String(buf);
    char bufData[12]; strftime(bufData, 12, "%d/%m", t);

    float upV = (rtcData.testesV>0)?(float)rtcData.onlineV*100.0/rtcData.testesV:0;
    float upT = (rtcData.testesT>0)?(float)rtcData.onlineT*100.0/rtcData.testesT:0;
    float mPV = (contaV>0)?somaPV/contaV:0; float mPT = (contaT>0)?somaPT/contaT:0;
    float mJV = (contaV>0)?somaJV/contaV:0; float mJT = (contaT>0)?somaJT/contaT:0;
    float accLossV = (rtcData.pkgEnviadosV > 0) ? (float)rtcData.pkgPerdidosV * 100.0 / rtcData.pkgEnviadosV : 0;
    float accLossT = (rtcData.pkgEnviadosT > 0) ? (float)rtcData.pkgPerdidosT * 100.0 / rtcData.pkgEnviadosT : 0;

    int slaV = calcularID_SLA(mPV, mJV, accLossV);
    int slaT = calcularID_SLA(mPT, mJT, accLossT);

    int ptsV, ptsT;
    String vencedorTxt = calcularVencedor(upV, upT, mPV, mPT, mJV, mJT, accLossV, accLossT, ptsV, ptsT);

    String titulo = manual ? "📊 <b>MONITOR (" + String(bufData) + " " + horaAtual + ")</b>" : "📊 <b>MONITOR (" + String(bufData) + " - DIÁRIO)</b>";

    String r = titulo + "\n\n";
    r += "<b>STATUS ATUAL</b>\n";
    r += "🏆 " + vencedorTxt + "\n\n";
    
    // VIVO
    r += "📡 <b>VIVO</b>\n";
    r += "📶 Uptime: " + String(upV, 1) + "%\n";
    r += "⚡ Média Ping: " + String(mPV, 0) + "ms\n";
    r += "📉 Média Jitter: " + String(mJV, 0) + "ms\n";
    r += "📦 Perda: " + String(rtcData.pkgPerdidosV) + "/" + String(rtcData.pkgEnviadosV) + " (" + String(accLossV, 1) + "%)\n";
    r += "⏳ Offline: " + fmtTempo(rtcData.tempoTotalOffV) + " (" + String(rtcData.quedasV) + " quedas)\n";
    r += "📌 Pico: " + String(piorPV,0) + "ms às " + hPiorPV + "\n";
    r += getSLAIcone(slaV);
    r += " <b>Classificação: ";
    r += getSLANome(slaV);
    r += "</b>\n\n";
    
    // TIM
    r += "📡 <b>TIM</b>\n";
    r += "📶 Uptime: " + String(upT, 1) + "%\n";
    r += "⚡ Média Ping: " + String(mPT, 0) + "ms\n";
    r += "📉 Média Jitter: " + String(mJT, 0) + "ms\n";
    r += "📦 Perda: " + String(rtcData.pkgPerdidosT) + "/" + String(rtcData.pkgEnviadosT) + " (" + String(accLossT, 1) + "%)\n";
    r += "⏳ Offline: " + fmtTempo(rtcData.tempoTotalOffT) + " (" + String(rtcData.quedasT) + " quedas)\n";
    r += "📌 Pico: " + String(piorPT,0) + "ms às " + hPiorPT + "\n";
    r += getSLAIcone(slaT);
    r += " <b>Classificação: ";
    r += getSLANome(slaT);
    r += "</b>\n\n";

    r += "📈 <a href=\"" + String(AIO_DASHBOARD_24H) + "\">Relatorio Grafico 24h</a>\n";
    r += "📅 <a href=\"" + String(AIO_DASHBOARD_30D) + "\">Relatorio Grafico 30 dias</a>";

    sendTelegram(r);
}

// ======== 7. TESTES ========
void rodarTestes() {
  time_t agora = time(nullptr); struct tm* t = localtime(&agora);
  char buf[20]; strftime(buf, 20, "%H:%M:%S", t); ultimaAtualizacao = String(buf);
  
  // --- VIVO ---
  int sv=0; float lv=0, jv=0, lp=0;
  for(int i=0; i<5; i++) { 
    if(Ping.ping(IP_VIVO, 1)){ float c=Ping.averageTime(); sv++; lv+=c; if(i>0&&lp>0)jv+=fabs(c-lp); lp=c; } // Corrigido abs() -> fabs()
  }
  int lostPrimeiraV = 5 - sv;
  bool houveRetryV = false;
  if (sv < 3) { 
    houveRetryV = true;
    logInstab("VIVO: 1a tentativa " + String(sv) + "/5, retentando");
    yieldDelay(3000); sv=0; lv=0; jv=0; lp=0; 
    for(int i=0; i<5; i++) { if(Ping.ping(IP_VIVO, 1)){ float c=Ping.averageTime(); sv++; lv+=c; if(i>0&&lp>0)jv+=fabs(c-lp); lp=c; } } 
  }
  pV=(sv>0)?lv/sv:0; jV=(sv>1)?jv/(sv-1):0; bool okV=(sv>=3);
  lastLostV = 5 - sv; lossPctV = ((float)lastLostV / 5.0) * 100.0;

  int totalEnviadosV = houveRetryV ? 10 : 5;
  int totalPerdidosV = houveRetryV ? (lostPrimeiraV + lastLostV) : lastLostV;
  rtcData.pkgEnviadosV += totalEnviadosV; rtcData.pkgPerdidosV += totalPerdidosV; rtcData.testesV++;
  if (houveRetryV) rtcData.instabV++;

  if(okV) {
    rtcData.onlineV++; cV=getCorPing(pV); cJV=getCorJitter(jV); cLV=getCorLoss(lossPctV); sV="Online";
    if(linkVivoOff) {
      unsigned long d = (unsigned long)(agora - rtcData.inicioQuedaVivo);
      rtcData.tempoTotalOffV += d;
      sendTelegram("✅ <b>VIVO VOLTOU</b>\n🕒 Caiu: "+hIniV+"\n⏳ Fora: "+fmtTempo(d));
      linkVivoOff=false; rtcData.vivoCaido=false; saveData();
      logSistema("VIVO VOLTOU");
    }
    somaPV+=pV; somaJV+=jV; contaV++;
    
    if(pV > piorPV) { piorPV = pV; hPiorPV = ultimaAtualizacao; }
    if(jV > piorJV) { piorJV = jV; hPiorJV = ultimaAtualizacao; }
    if(melhorPV == -1 || pV < melhorPV) { melhorPV = pV; hMelhorPV = ultimaAtualizacao; }
    if(melhorJV == -1 || jV < melhorJV) { melhorJV = jV; hMelhorJV = ultimaAtualizacao; }
    
    bool instavelV = false;
    String motivoV = "";
    if (lastLostV > 0) {
      instavelV = true;
      motivoV += "Perda (" + String(lastLostV) + " pac)";
    }
    if (jV > 15.0) {
      instavelV = true;
      if (motivoV != "") motivoV += " + ";
      motivoV += "Jitter (" + String(jV, 0) + "ms)";
    }
    if (pV > 80.0) {
      instavelV = true;
      if (motivoV != "") motivoV += " + ";
      motivoV += "Ping (" + String(pV, 0) + "ms)";
    }
    if (instavelV) {
      rtcData.instabV++;
      logInstab("VIVO: " + motivoV);
    }

  } else {
    sV="OFFLINE"; cV=COR_PES; cJV=COR_PES; cLV=COR_PES;
    if(!linkVivoOff) {
      linkVivoOff=true; iniV=agora; hIniV=ultimaAtualizacao; rtcData.quedasV++;
      rtcData.vivoCaido=true; rtcData.inicioQuedaVivo=agora;
      strncpy(rtcData.hUltimaQuedaV, ultimaAtualizacao.substring(0,8).c_str(), 8);
      rtcData.hUltimaQuedaV[8]='\0';
      saveData();
      sendTelegram("⚠️ <b>VIVO CAIU</b>"); logSistema("VIVO CAIU");
    }
  }

  int slaHistV = okV ? calcularID_SLA(pV, jV, lossPctV) : SLA_OFF;
  addHistorico(rtcData.histV, slaHistV);

  // --- TIM ---
  int st=0; float lt=0, jt=0, lpt=0;
  for(int i=0; i<5; i++) { 
    if(Ping.ping(IP_TIM, 1)){ float c=Ping.averageTime(); st++; lt+=c; if(i>0&&lpt>0)jt+=fabs(c-lpt); lpt=c; }
  }
  int lostPrimeiraT = 5 - st;
  bool houveRetryT = false;
  if (st < 3) { 
    houveRetryT = true;
    logInstab("TIM: 1a tentativa " + String(st) + "/5, retentando");
    yieldDelay(3000); st=0; lt=0; jt=0; lpt=0; 
    for(int i=0; i<5; i++) { if(Ping.ping(IP_TIM, 1)){ float c=Ping.averageTime(); st++; lt+=c; if(i>0&&lpt>0)jt+=fabs(c-lpt); lpt=c; } } 
  }
  pT=(st>0)?lt/st:0; jT=(st>1)?jt/(st-1):0; bool okT=(st>=3);
  lastLostT = 5 - st; lossPctT = ((float)lastLostT / 5.0) * 100.0;

  int totalEnviadosT = houveRetryT ? 10 : 5;
  int totalPerdidosT = houveRetryT ? (lostPrimeiraT + lastLostT) : lastLostT;
  rtcData.pkgEnviadosT += totalEnviadosT; rtcData.pkgPerdidosT += totalPerdidosT; rtcData.testesT++;
  if (houveRetryT) rtcData.instabT++;

  if(okT) {
    rtcData.onlineT++; cT=getCorPing(pT); cJT=getCorJitter(jT); cLT=getCorLoss(lossPctT); sT="Online";
    if(linkTimOff) {
      unsigned long d = (unsigned long)(agora - rtcData.inicioQuedaTim);
      rtcData.tempoTotalOffT += d;
      sendTelegram("✅ <b>TIM VOLTOU</b>\n🕒 Caiu: "+hIniT+"\n⏳ Fora: "+fmtTempo(d));
      linkTimOff=false; rtcData.timCaido=false; saveData();
      logSistema("TIM VOLTOU");
    }
    somaPT+=pT; somaJT+=jT; contaT++;
    
    if(pT > piorPT) { piorPT = pT; hPiorPT = ultimaAtualizacao; }
    if(jT > piorJT) { piorJT = jT; hPiorJT = ultimaAtualizacao; }
    if(melhorPT == -1 || pT < melhorPT) { melhorPT = pT; hMelhorPT = ultimaAtualizacao; }
    if(melhorJT == -1 || jT < melhorJT) { melhorJT = jT; hMelhorJT = ultimaAtualizacao; }

    bool instavelT = false;
    String motivoT = "";
    if (lastLostT > 0) {
      instavelT = true;
      motivoT += "Perda (" + String(lastLostT) + " pac)";
    }
    if (jT > 15.0) {
      instavelT = true;
      if (motivoT != "") motivoT += " + ";
      motivoT += "Jitter (" + String(jT, 0) + "ms)";
    }
    if (pT > 80.0) {
      instavelT = true;
      if (motivoT != "") motivoT += " + ";
      motivoT += "Ping (" + String(pT, 0) + "ms)";
    }
    if (instavelT) {
      rtcData.instabT++;
      logInstab("TIM: " + motivoT);
    }

  } else {
    sT="OFFLINE"; cT=COR_PES; cJT=COR_PES; cLT=COR_PES;
    if(!linkTimOff) {
      linkTimOff=true; iniT=agora; hIniT=ultimaAtualizacao; rtcData.quedasT++;
      rtcData.timCaido=true; rtcData.inicioQuedaTim=agora;
      strncpy(rtcData.hUltimaQuedaT, ultimaAtualizacao.substring(0,8).c_str(), 8);
      rtcData.hUltimaQuedaT[8]='\0';
      saveData();
      sendTelegram("⚠️ <b>TIM CAIU</b>"); logSistema("TIM CAIU");
    }
  }

  int slaHistT = okT ? calcularID_SLA(pT, jT, lossPctT) : SLA_OFF;
  addHistorico(rtcData.histT, slaHistT);

  // --- MQTT ---
  if (!sistemaPausado) {
    connectMQTT();
    if (mqtt.connected()) {
       bool env1 = feedVivoPing.publish((int)pV); bool env2 = feedVivoPerda.publish((int)lossPctV); bool env3 = feedVivoJitter.publish((int)jV); 
       bool env4 = feedTimPing.publish((int)pT); bool env5 = feedTimPerda.publish((int)lossPctT); bool env6 = feedTimJitter.publish((int)jT); 
       if(env1 && env2 && env3 && env4 && env5 && env6) { ultimoStatusMQTT = "OK"; corStatusMQTT = COR_BOM; } 
       else { ultimoStatusMQTT = "Falha"; corStatusMQTT = COR_MED; }
    }
  }

  // --- PING DISPOSITIVOS LOCAIS (Keep-Alive) ---
  for (int i = 0; i < 10; i++) {
    yield();
    if (configData.ip[i][0] != '\0' && configData.label[i][0] != '\0') {
      bool wasOk = okLocal[i];
      if (Ping.ping(configData.ip[i], 1)) {
        pLocal[i] = Ping.averageTime();
        okLocal[i] = true;
      } else {
        pLocal[i] = 0;
        okLocal[i] = false;
      }
      
      if (bootLocalPingDone && configData.alertTelegram[i] == 1 && wasOk != okLocal[i]) {
        String msgAlert = okLocal[i] ? "✅ <b>" : "⚠️ <b>";
        msgAlert += String(configData.label[i]) + "</b>\n";
        msgAlert += okLocal[i] ? "Está ONLINE" : "Está OFFLINE";
        msgAlert += "\n🌐 IP: " + String(configData.ip[i]);
        if (okLocal[i]) {
          msgAlert += "\n⚡ Ping: " + String(pLocal[i], 0) + "ms";
        }
        sendTelegram(msgAlert);
      }
    } else {
      pLocal[i] = 0;
      okLocal[i] = false;
    }
  }

  bootLocalPingDone = true;
  saveData(); ultimoTeste = millis();
}

// ======== 8. INTERFACE WEB ========
void handleRoot() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", "");
  
  String h = F("<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='60'>"
    "<script>(function(){var t=localStorage.getItem('theme')||'auto';function apply(theme){var actual=theme;if(theme==='auto'){actual=window.matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light';}document.documentElement.setAttribute('data-theme',actual);}apply(t);window.matchMedia('(prefers-color-scheme:dark)').addEventListener('change',function(){if(localStorage.getItem('theme')==='auto'||!localStorage.getItem('theme')){apply('auto');}});})();</script>"
    "<style>"
    ":root{--bg:#1a1a1a;--color:#eee;--card-bg:#252525;--border:#333;--th-bg:#333;--th-color:#bbb;--log-bg:#000;--log-color:#0f0;--log-lag-bg:#220000;--log-lag-color:#FF8A80;--border-bottom:#444;--btn-gray-bg:#424242;--btn-gray-color:#fff;--no-data-color:#444;}"
    "[data-theme=\"light\"]{--bg:#f5f5f5;--color:#222;--card-bg:#ffffff;--border:#ddd;--th-bg:#eaeaea;--th-color:#555;--log-bg:#f0f0f0;--log-color:#1b5e20;--log-lag-bg:#ffebee;--log-lag-color:#c62828;--border-bottom:#ddd;--btn-gray-bg:#e0e0e0;--btn-gray-color:#333;--no-data-color:#ddd;}"
    "body{font-family:sans-serif;background:var(--bg);color:var(--color);text-align:center;margin:0;padding:5px;}"
    ".c{max-width:500px;margin:0 auto;}"
    "h1{margin:15px 0 5px 0;color:#00E5FF;font-size:1.8em;font-weight:300;letter-spacing:1px;}"
    "h3{margin-top:20px;margin-bottom:5px;border-bottom:1px solid var(--border-bottom);padding-bottom:5px;display:flex;align-items:center;justify-content:center;gap:5px;}"
    "table{width:100%;border-collapse:collapse;background:var(--card-bg);margin-bottom:10px;border-radius:5px;box-shadow:0 2px 4px rgba(0,0,0,0.15);}"
    "td,th{padding:10px 5px;border:1px solid var(--border);font-size:0.85em;}"
    "th{background:var(--th-bg);color:var(--th-color);}"
    ".btn-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin:15px 0;}"
    ".btn{padding:12px;width:100%;border:none;border-radius:5px;color:#fff;font-weight:bold;cursor:pointer;font-size:0.9em;height:45px;display:flex;align-items:center;justify-content:center;text-decoration:none;}"
    ".b-blu{background:#0277BD;}"
    ".b-dkblu{background:#1565C0;}"
    ".b-yel{background:#FF8F00;color:#000;}"
    ".b-red{background:#C62828;}"
    ".b-purp{background:#7B1FA2;}"
    ".b-gray{background:var(--btn-gray-bg);color:var(--btn-gray-color);}"
    ".log{text-align:left;background:var(--log-bg);padding:10px;font-family:monospace;font-size:0.8em;border-left:3px solid #4CAF50;margin-bottom:15px;color:var(--log-color);}"
    ".log-lag{text-align:left;background:var(--log-lag-bg);padding:10px;font-family:monospace;font-size:0.8em;border-left:3px solid #FF5252;margin-bottom:15px;color:var(--log-lag-color);}"
    ".bg-bom{background:#4CAF50;color:#fff}"
    ".bg-med{background:#FFC107;color:#000}"
    ".bg-ruim{background:#F44336;color:#fff}"
    "</style></head><body><div class='c'><h1>Monitoramento</h1>");
  server.sendContent(h);
  
  // 1. CABEÇALHO
  server.sendContent("<p style='color:#888;font-size:0.9em;margin-bottom:5px'>Atualizado: " + ultimaAtualizacao + "</p>");
  server.sendContent(F("<p style='color:#888;font-size:0.8em;margin-bottom:10px'>🔄 Reset automático às 09:00</p>"));

  if(sistemaPausado) server.sendContent(F("<div style='background:#F44336;color:#fff;padding:8px;margin-bottom:10px;border-radius:5px;'>⏸️ PAUSADO</div>"));

  // Indicador de dados desatualizados
  if (millis() - ultimoTeste > 90000) {
    server.sendContent(F("<div style='background:#F44336;color:#fff;padding:8px;margin-bottom:10px;border-radius:5px;'>⚠️ DADOS DESATUALIZADOS — verifique o sistema</div>"));
  }

  // Estatísticas do dia
  float upV = (rtcData.testesV>0)?(float)rtcData.onlineV*100.0/rtcData.testesV:0;
  float upT = (rtcData.testesT>0)?(float)rtcData.onlineT*100.0/rtcData.testesT:0;
  float mPV = (contaV>0)?somaPV/contaV:0; float mPT = (contaT>0)?somaPT/contaT:0;
  float mJV = (contaV>0)?somaJV/contaV:0; float mJT = (contaT>0)?somaJT/contaT:0;
  float accLossV = (rtcData.pkgEnviadosV > 0) ? (float)rtcData.pkgPerdidosV * 100.0 / rtcData.pkgEnviadosV : 0;
  float accLossT = (rtcData.pkgEnviadosT > 0) ? (float)rtcData.pkgPerdidosT * 100.0 / rtcData.pkgEnviadosT : 0;

  // Badge "quem está ganhando"
  int ptsV, ptsT;
  String vencedorTxt = calcularVencedor(upV, upT, mPV, mPT, mJV, mJT, accLossV, accLossT, ptsV, ptsT);
  server.sendContent("<div style='background:var(--card-bg);border:1px solid var(--border);padding:10px;border-radius:5px;margin-bottom:10px;font-weight:bold'>🏆 " + vencedorTxt + "</div>");

  // 2. QUADRO 1: ÚLTIMO TESTE
  int slaNowV = calcularID_SLA(pV, jV, lossPctV);
  int slaNowT = calcularID_SLA(pT, jT, lossPctT);
  if(sV == "OFFLINE") slaNowV = SLA_OFF;
  if(sT == "OFFLINE") slaNowT = SLA_OFF;

  server.sendContent(F("<h3>⏱️ ÚLTIMO TESTE</h3>"));
  server.sendContent(F("<table><tr><th>LINK</th><th>PING</th><th>JITTER</th><th>PERDA</th><th>CLASSE</th></tr>"));
  {
    char bufP[10], bufJ[10], linha[320];

    // Formatação VIVO: exibe "-" em cinza quando OFFLINE, sem "ms"
    if (sV == "OFFLINE") {
      strcpy(bufP, "-");
      strcpy(bufJ, "-");
    } else {
      fmtFloat(pV, 0, bufP, sizeof(bufP));
      fmtFloat(jV, 0, bufJ, sizeof(bufJ));
    }
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>VIVO</td><td style='color:%s'>%s%s</td><td style='color:%s'>%s%s</td><td style='color:%s'>%d/5</td><td style='font-weight:bold;color:%s'>%s %s</td></tr>",
      (sV == "OFFLINE") ? "#888" : getCorPing(pV), bufP, (sV == "OFFLINE") ? "" : "ms", 
      (sV == "OFFLINE") ? "#888" : getCorJitter(jV), bufJ, (sV == "OFFLINE") ? "" : "ms", 
      getCorLoss(lossPctV), lastLostV, getSLACor(slaNowV), getSLAIcone(slaNowV), getSLANome(slaNowV));
    server.sendContent(linha);

    // Formatação TIM
    if (sT == "OFFLINE") {
      strcpy(bufP, "-");
      strcpy(bufJ, "-");
    } else {
      fmtFloat(pT, 0, bufP, sizeof(bufP));
      fmtFloat(jT, 0, bufJ, sizeof(bufJ));
    }
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>TIM</td><td style='color:%s'>%s%s</td><td style='color:%s'>%s%s</td><td style='color:%s'>%d/5</td><td style='font-weight:bold;color:%s'>%s %s</td></tr></table>",
      (sT == "OFFLINE") ? "#888" : getCorPing(pT), bufP, (sT == "OFFLINE") ? "" : "ms", 
      (sT == "OFFLINE") ? "#888" : getCorJitter(jT), bufJ, (sT == "OFFLINE") ? "" : "ms", 
      getCorLoss(lossPctT), lastLostT, getSLACor(slaNowT), getSLAIcone(slaNowT), getSLANome(slaNowT));
    server.sendContent(linha);
  }

  // 2.5 INFRAESTRUTURA LOCAL (Keep-Alive)
  bool temLocal = false;
  for (int i = 0; i < 10; i++) {
    if (configData.ip[i][0] != '\0' && configData.label[i][0] != '\0') {
      temLocal = true;
      break;
    }
  }
  if (temLocal) {
    server.sendContent(F("<h3>🛠️ INFRAESTRUTURA LOCAL</h3>"));
    server.sendContent(F("<table><tr><th>EQUIPAMENTO</th><th>IP</th><th>STATUS</th><th>PING</th></tr>"));
    char linha[256];
    for (int i = 0; i < 10; i++) {
      if (configData.ip[i][0] != '\0' && configData.label[i][0] != '\0') {
        const char* statusStr = okLocal[i] ? "Online" : "Offline";
        const char* corStatus = okLocal[i] ? COR_BOM : COR_PES;
        char pingStr[12];
        if (okLocal[i]) {
          snprintf(pingStr, sizeof(pingStr), "%.0fms", pLocal[i]);
        } else {
          strcpy(pingStr, "-");
        }
        snprintf(linha, sizeof(linha),
          "<tr><td style='text-align:left;padding-left:10px'>%s</td><td>%s</td><td style='font-weight:bold;color:%s'>%s</td><td style='color:%s'>%s</td></tr>",
          configData.label[i], configData.ip[i], corStatus, statusStr, okLocal[i] ? C_EXCELENTE : "#888", pingStr);
        server.sendContent(linha);
      }
    }
    server.sendContent(F("</table>"));
  }

  // 3. HISTÓRICO RECENTE
  server.sendContent(F("<h3>📊 HISTÓRICO RECENTE</h3>"));
  server.sendContent(F("<div style='display:flex;justify-content:space-between;align-items:center;background:var(--card-bg);border:1px solid var(--border);padding:10px;border-radius:5px;margin-bottom:10px;'>"));
  
  // Coluna Esquerda: Sparklines
  server.sendContent(F("<div style='text-align:left;flex:1;'>"));
  server.sendContent(F("<p style='margin:0 0 2px;font-size:0.8em;color:#888'>VIVO</p>"));
  renderHistorico(rtcData.histV);
  server.sendContent(F("<p style='margin:6px 0 2px;font-size:0.8em;color:#888'>TIM</p>"));
  renderHistorico(rtcData.histT);
  server.sendContent(F("</div>"));
  
  // Coluna Direita: Legenda
  server.sendContent(F("<div style='font-size:0.68em;text-align:left;border-left:1px solid var(--border);padding-left:10px;margin-left:10px;display:flex;flex-direction:column;gap:4px;white-space:nowrap;'>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:#00E5FF;border-radius:1px;'></div>Excelente</div>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:#4CAF50;border-radius:1px;'></div>Bom</div>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:#FFC107;border-radius:1px;'></div>Regular</div>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:#FF5252;border-radius:1px;'></div>Ruim</div>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:#8B0000;border-radius:1px;'></div>Offline</div>"));
  server.sendContent(F("<div style='display:flex;align-items:center;gap:4px;'><div style='width:8px;height:8px;background:var(--no-data-color);border-radius:1px;'></div>Sem dados</div>"));
  server.sendContent(F("</div></div>"));

  // 4. QUADRO 2: HARDWARE
  long rssi = WiFi.RSSI(); int pct = (rssi >= -50) ? 100 : (rssi <= -100 ? 0 : 2 * (rssi + 100));
  uint32_t freeH = ESP.getFreeHeap(); int frag = ESP.getHeapFragmentation();

  const char* cSig  = getCorSignal(pct);
  const char* cHBoot = getCorHeap(heapNoBoot);
  const char* cHAtual = getCorHeap(freeH);
  const char* cFrag  = getCorFrag(frag);
  
  server.sendContent(F("<h3>🛠 HARDWARE INFO</h3><table><tr><th>ITEM</th><th>VALOR</th><th>STATUS</th></tr>"));
  {
    char linha[220];

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Sinal Wi-Fi</td><td>%d%%</td><td style='font-weight:bold;color:%s'>%s</td></tr>",
      pct, cSig, getTextoStatus(cSig));
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Heap Boot</td><td>%dk</td><td style='font-weight:bold;color:%s'>%s</td></tr>",
      (int)(heapNoBoot/1024), cHBoot, getTextoStatus(cHBoot));
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Heap Atual</td><td>%dk</td><td style='font-weight:bold;color:%s'>%s</td></tr>",
      (int)(freeH/1024), cHAtual, getTextoStatus(cHAtual));
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Frag. RAM</td><td>%d%%</td><td style='font-weight:bold;color:%s'>%s</td></tr></table>",
      frag, cFrag, getTextoStatus(cFrag));
    server.sendContent(linha);
  }

  // 5. QUADRO 3: ESTATÍSTICAS
  const char* cAccLV = (rtcData.pkgPerdidosV > 0) ? COR_MED : COR_BOM;
  const char* cAccLT = (rtcData.pkgPerdidosT > 0) ? COR_MED : COR_BOM;

  server.sendContent(F("<h3>📈 ESTATÍSTICAS (HOJE)</h3><table><tr><th>INFO</th><th>VIVO</th><th>TIM</th></tr>"));
  {
    char linha[300];
    char fA[10], fB[10];

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Quedas</td><td>%ld</td><td>%ld</td></tr>",
      (long)rtcData.quedasV, (long)rtcData.quedasT);
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Última Queda</td><td>%s</td><td>%s</td></tr>",
      rtcData.hUltimaQuedaV, rtcData.hUltimaQuedaT);
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Offline Hoje</td><td>%s</td><td>%s</td></tr>",
      fmtTempo(rtcData.tempoTotalOffV).c_str(), fmtTempo(rtcData.tempoTotalOffT).c_str());
    server.sendContent(linha);

    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Instab.</td><td>%ld</td><td>%ld</td></tr>",
      (long)rtcData.instabV, (long)rtcData.instabT);
    server.sendContent(linha);

    fmtFloat(upV, 1, fA, sizeof(fA)); fmtFloat(upT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Uptime</td><td style='color:%s'>%s%%</td><td style='color:%s'>%s%%</td></tr>",
      getCorUptime(upV), fA, getCorUptime(upT), fB);
    server.sendContent(linha);

    fmtFloat(mPV, 1, fA, sizeof(fA)); fmtFloat(mPT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Média Ping</td><td style='color:%s'>%sms</td><td style='color:%s'>%sms</td></tr>",
      getCorPing(mPV), fA, getCorPing(mPT), fB);
    server.sendContent(linha);

    fmtFloat(mJV, 1, fA, sizeof(fA)); fmtFloat(mJT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Média Jitter</td><td style='color:%s'>%sms</td><td style='color:%s'>%sms</td></tr>",
      getCorJitter(mJV), fA, getCorJitter(mJT), fB);
    server.sendContent(linha);

    fmtFloat(accLossV, 1, fA, sizeof(fA)); fmtFloat(accLossT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Perda Total</td><td style='color:%s'>%ld/%ld (%s%%)</td><td style='color:%s'>%ld/%ld (%s%%)</td></tr>",
      cAccLV, (long)rtcData.pkgPerdidosV, (long)rtcData.pkgEnviadosV, fA, cAccLT, (long)rtcData.pkgPerdidosT, (long)rtcData.pkgEnviadosT, fB);
    server.sendContent(linha);

    fmtFloat(melhorPV, 1, fA, sizeof(fA)); fmtFloat(melhorPT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Melhor Ping</td><td style='color:%s'>%sms (%s)</td><td style='color:%s'>%sms (%s)</td></tr>",
      getCorPing(melhorPV), fA, hMelhorPV.c_str(), getCorPing(melhorPT), fB, hMelhorPT.c_str());
    server.sendContent(linha);

    fmtFloat(melhorJV, 1, fA, sizeof(fA)); fmtFloat(melhorJT, 1, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Melhor Jitter</td><td style='color:%s'>%sms (%s)</td><td style='color:%s'>%sms (%s)</td></tr>",
      getCorJitter(melhorJV), fA, hMelhorJV.c_str(), getCorJitter(melhorJT), fB, hMelhorJT.c_str());
    server.sendContent(linha);

    fmtFloat(piorPV, 0, fA, sizeof(fA)); fmtFloat(piorPT, 0, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Pior Ping</td><td style='color:%s'>%sms (%s)</td><td style='color:%s'>%sms (%s)</td></tr>",
      getCorPing(piorPV), fA, hPiorPV.c_str(), getCorPing(piorPT), fB, hPiorPT.c_str());
    server.sendContent(linha);

    fmtFloat(piorJV, 0, fA, sizeof(fA)); fmtFloat(piorJT, 0, fB, sizeof(fB));
    snprintf(linha, sizeof(linha),
      "<tr><td style='text-align:left;padding-left:10px'>Pior Jitter</td><td style='color:%s'>%sms (%s)</td><td style='color:%s'>%sms (%s)</td></tr></table>",
      getCorJitter(piorJV), fA, hPiorJV.c_str(), getCorJitter(piorJT), fB, hPiorJT.c_str());
    server.sendContent(linha);
  }

  // 6. QUADRO 4: REGISTRO DE INSTABILIDADE (Circular Buffer)
  server.sendContent(F("<h3>📉 REGISTRO DE INSTABILIDADE</h3><div class='log-lag'>"));
  for(int i=0; i<5; i++) { 
    int idx = (logLagIdx - 1 - i + 5) % 5;
    if (logLag[idx] != "-" && logLag[idx] != "") {
      server.sendContent(logLag[idx] + "<br>"); 
    }
  }
  server.sendContent(F("</div>"));

  // 7. QUADRO 5: EVENTOS DO SISTEMA (Circular Buffer)
  server.sendContent(F("<h3>📝 EVENTOS DO SISTEMA</h3><div class='log'>"));
  for(int i=0; i<5; i++) { 
    int idx = (logSysIdx - 1 - i + 5) % 5;
    if (logSys[idx] != "-" && logSys[idx] != "") {
      server.sendContent(logSys[idx] + "<br>"); 
    }
  }
  server.sendContent(F("</div>"));

  // PAINEL DE CONTROLE
  server.sendContent(F("<h3>Painel de Controle</h3>"));
  server.sendContent(F("<div class='btn-grid'>"));
  server.sendContent(F("<form action='/testar' method='POST'><button class='btn b-blu'>⚡ TESTAR</button></form>"));
  server.sendContent(F("<form action='/relatorio' method='POST'><button class='btn b-dkblu'>📨 RESUMO AGORA</button></form>"));
  server.sendContent(F("<a href='" AIO_DASHBOARD_30D "' target='_blank' style='text-decoration:none'><button class='btn b-gray'>📅 HISTÓRICO 30 DIAS</button></a>"));
  if(sistemaPausado) server.sendContent(F("<form action='/pausar' method='POST'><button class='btn b-yel'>▶️ RETOMAR</button></form>"));
  else server.sendContent(F("<form action='/pausar' method='POST'><button class='btn b-yel'>⏸️ PAUSAR</button></form>"));
  if(modoNaoPerturbe) server.sendContent(F("<form action='/dnd' method='POST'><button class='btn b-purp'>🔔 MENSAGENS ON</button></form>"));
  else server.sendContent(F("<form action='/dnd' method='POST'><button class='btn b-purp'>🔕 MENSAGENS OFF</button></form>"));
  server.sendContent(F("<form action='/resetar' method='POST'><button class='btn b-red'>🔄 RESET</button></form>"));
  server.sendContent(F("</div>"));

  server.sendContent(F("<h3>Integrações</h3><table><tr><th>SERVIÇO</th><th>STATUS</th></tr>"));
  server.sendContent("<tr><td>Telegram</td><td style='font-weight:bold;color:"+corStatusTelegram+"'>"+ultimoStatusTelegram+"</td></tr>");
  server.sendContent("<tr><td>Adafruit Nuvem</td><td style='font-weight:bold;color:"+corStatusMQTT+"'>"+ultimoStatusMQTT+"</td></tr></table>");
  
  server.sendContent(F("<p style='font-size:0.75em;color:#555;margin-top:15px;'><a href='/config' style='color:#555;text-decoration:none;'>🔧 Configurações de Equipamentos</a> &nbsp;|&nbsp; <a href='/update' style='color:#555;text-decoration:none;'>⚙️ Atualização de Firmware (OTA)</a></p>"));

  server.sendContent(F("</div></body></html>"));
  server.sendContent("");
}

void handleTestar() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='background:#1a1a1a;color:#fff;text-align:center;padding-top:50px;font-family:sans-serif;'><h2>🚀 Rodando Testes...</h2><script>setTimeout(function(){window.location.href='/';},10000);</script></body></html>");
  rodarTestes();
}

void handleRelatorio() {
  enviarRelatorioCompleto(true);
  server.sendHeader("Connection", "close");
  server.sendHeader("Location", "/"); server.send(303);
}

void handleReset() {
  rtcData.webReset = 1; saveData();
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='background:#1a1a1a;color:#fff;text-align:center;padding-top:50px;font-family:sans-serif;'><h2>🔄 Reiniciando...</h2><script>setTimeout(function(){window.location.href='/';},20000);</script></body></html>");
  delay(1000); ESP.restart();
}

void handleConfigGet() {
  if (!server.authenticate("admin", WIFI_PASS)) {
    return server.requestAuthentication();
  }
  
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", "");
  
  server.sendContent(F("<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<script>(function(){var t=localStorage.getItem('theme')||'auto';function apply(theme){var actual=theme;if(theme==='auto'){actual=window.matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light';}document.documentElement.setAttribute('data-theme',actual);}apply(t);window.matchMedia('(prefers-color-scheme:dark)').addEventListener('change',function(){if(localStorage.getItem('theme')==='auto'||!localStorage.getItem('theme')){apply('auto');}});})();</script>"
    "<style>"
    ":root{--bg:#1a1a1a;--color:#eee;--card-bg:#252525;--border:#333;--border-bottom:#444;--card-inner-bg:#2d2d2d;--input-bg:#222;--input-border:#444;--text-muted:#aaa;}"
    "[data-theme=\"light\"]{--bg:#f5f5f5;--color:#222;--card-bg:#ffffff;--border:#ddd;--border-bottom:#ddd;--card-inner-bg:#fdfdfd;--input-bg:#ffffff;--input-border:#ccc;--text-muted:#666;}"
    "body{font-family:sans-serif;background:var(--bg);color:var(--color);text-align:center;margin:0;padding:10px;}"
    ".c{max-width:500px;margin:0 auto;}"
    "h2,h3{margin-top:20px;color:#00E5FF;border-bottom:1px solid var(--border-bottom);padding-bottom:5px;}"
    ".box{background:var(--card-bg);padding:15px;border-radius:5px;margin-bottom:15px;text-align:left;box-shadow:0 2px 4px rgba(0,0,0,0.15);}"
    ".card{background:var(--card-inner-bg);padding:12px;border-radius:6px;margin-bottom:12px;border-left:4px solid #00E5FF;}"
    ".card h4{margin:0 0 10px 0;color:#00E5FF;font-size:0.95em;display:flex;justify-content:space-between;align-items:center;}"
    ".field{margin-bottom:10px;}"
    ".field label{display:block;font-size:0.78em;color:var(--text-muted);margin-bottom:3px;font-weight:bold;}"
    ".field input[type=text]{width:100%;padding:8px 10px;background:var(--input-bg);border:1px solid var(--input-border);color:var(--color);border-radius:4px;box-sizing:border-box;font-size:0.9em;}"
    ".field input[type=text]:focus{border-color:#00E5FF;outline:none;}"
    ".btn{display:block;width:100%;padding:12px;border:none;border-radius:5px;font-weight:bold;cursor:pointer;font-size:0.95em;text-align:center;text-decoration:none;margin-top:10px;box-sizing:border-box;}"
    ".btn-clear{padding:4px 8px;background:#c62828;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:0.75em;font-weight:bold;}"
    ".btn-clear:hover{background:#b71c1c;}"
    ".b-cyan{background:#00E5FF;color:#000;}"
    ".b-cyan:hover{background:#00b4cc;}"
    ".b-green{background:#4CAF50;color:#fff;}"
    ".b-green:hover{background:#43a047;}"
    ".back{display:inline-block;margin-top:15px;color:#888;text-decoration:none;font-size:0.85em;}"
    ".back:hover{color:#bbb;}"
    ".chk-box{display:flex;align-items:center;gap:8px;margin-top:8px;}"
    ".chk-box input[type=checkbox]{width:16px;height:16px;margin:0;cursor:pointer;}"
    ".chk-box label{display:inline;font-size:0.78em;color:var(--text-muted);font-weight:bold;cursor:pointer;margin:0;}"
    "</style></head><body><div class='c'><h2>🔧 Configurações de IPs</h2><form action='/config' method='POST'><div class='box'><h3>📍 Equipamentos a Monitorar</h3>"));
  
  int visibleCount = 0;
  for (int i = 0; i < 10; i++) {
    if (configData.ip[i][0] != '\0' || configData.label[i][0] != '\0') {
      visibleCount++;
    }
  }
  
  char buf[1024];
  for (int i = 0; i < 10; i++) {
    bool showCard = (configData.ip[i][0] != '\0' || configData.label[i][0] != '\0') || (i == 0 && visibleCount == 0);
    const char* alertChecked = (configData.alertTelegram[i] == 1) ? "checked" : "";
    snprintf(buf, sizeof(buf),
      "<div class='card' id='card%d' style='display: %s;'>"
      "<h4>Equipamento %d <button type='button' class='btn-clear' onclick='removeDevice(%d)'>🗑️ Excluir</button></h4>"
      "<div class='field'><label>Nome do Equipamento</label><input type='text' id='label%d' name='label%d' value='%s' maxlength='15'></div>"
      "<div class='field'><label>IP Local</label><input type='text' id='ip%d' name='ip%d' value='%s' maxlength='15'></div>"
      "<div class='field chk-box'>"
      "<input type='checkbox' id='alert%d' name='alert%d' value='1' %s>"
      "<label for='alert%d'>🔔 Enviar alertas no Telegram</label>"
      "</div>"
      "</div>",
      i, showCard ? "block" : "none", i+1, i, i, i, configData.label[i], i, i, configData.ip[i], i, i, alertChecked, i
    );
    server.sendContent(buf);
  }
  
  server.sendContent(F("<button type='button' class='btn b-green' id='btn-add' onclick='addDevice()'>➕ Adicionar Equipamento</button>"
    "<button class='btn b-cyan' type='submit' style='margin-top:20px;'>💾 Salvar Configurações</button></div></form>"
    "<div class='box'><h3>🌓 Tema do Portal</h3>"
    "<div class='field'><label>Escolha o Estilo Visual</label>"
    "<select id='theme-select' style='width:100%;padding:10px;background:var(--input-bg);border:1px solid var(--input-border);color:var(--color);border-radius:4px;font-size:0.9em;margin-top:5px;cursor:pointer;' onchange='setPortalTheme(this.value)'>"
    "<option value='auto'>🌓 Automático (Sistema)</option>"
    "<option value='dark'>🌑 Escuro</option>"
    "<option value='light'>☀️ Claro</option>"
    "</select></div></div>"
    "<a href='/' class='back'>◀ Voltar ao Portal</a></div>"));
  
  server.sendContent(F("<script>"
    "document.getElementById('theme-select').value = localStorage.getItem('theme') || 'auto';"
    "function setPortalTheme(val) {"
      "localStorage.setItem('theme', val);"
      "var actual = val;"
      "if (val === 'auto') {"
        "actual = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';"
      "}"
      "document.documentElement.setAttribute('data-theme', actual);"
    "}"
    "function addDevice() {"
      "for (let i = 0; i < 10; i++) {"
        "let card = document.getElementById('card' + i);"
        "if (card.style.display === 'none') {"
          "card.style.display = 'block';"
          "return;"
        "}"
      "}"
      "alert('Limite máximo de 10 equipamentos atingido.');"
    "}"
    "function removeDevice(i) {"
      "document.getElementById('label' + i).value = '';"
      "document.getElementById('ip' + i).value = '';"
      "document.getElementById('alert' + i).checked = false;"
      "document.getElementById('card' + i).style.display = 'none';"
    "}"
    "</script></body></html>"
  ));
  server.sendContent("");
}

void handleConfigPost() {
  if (!server.authenticate("admin", WIFI_PASS)) {
    return server.requestAuthentication();
  }
  
  for (int i = 0; i < 10; i++) {
    String labelArg = "label" + String(i);
    String ipArg = "ip" + String(i);
    String alertArg = "alert" + String(i);
    
    String lblStr = server.hasArg(labelArg) ? server.arg(labelArg) : "";
    lblStr.trim();
    strncpy(configData.label[i], lblStr.c_str(), 15);
    configData.label[i][15] = '\0';
    
    String ipStr = server.hasArg(ipArg) ? server.arg(ipArg) : "";
    ipStr.trim();
    strncpy(configData.ip[i], ipStr.c_str(), 15);
    configData.ip[i][15] = '\0';
    
    configData.alertTelegram[i] = (server.hasArg(alertArg) && server.arg(alertArg) == "1") ? 1 : 0;
  }
  
  configData.magic = 0xEEBB0016;
  EEPROM.put(0, configData);
  EEPROM.commit();
  
  ultimoTeste = millis() - 55000;
  
  server.sendHeader("Connection", "close");
  server.send(200, "text/html", F("<html><head><meta charset='utf-8'></head><body style='background:#1a1a1a;color:#fff;text-align:center;padding-top:50px;font-family:sans-serif;'><h2>✅ Configurações Salvas!</h2><p style='color:#aaa;'>Atualizando os pings e retornando...</p><script>setTimeout(function(){window.location.href='/';},2000);</script></body></html>"));
}

void setup() {
  Serial.begin(115200);

  EEPROM.begin(512);
  EEPROM.get(0, configData);
  if (configData.magic != 0xEEBB0016) {
    configData.magic = 0xEEBB0016;
    for (int i = 0; i < 10; i++) {
      configData.label[i][0] = '\0';
      configData.ip[i][0] = '\0';
      configData.alertTelegram[i] = 0;
    }
    EEPROM.put(0, configData);
    EEPROM.commit();
  }

  ESP.rtcUserMemoryRead(0, (uint32_t*)&rtcData, sizeof(rtcData));
  bool rtcValido = (rtcData.magic == RTC_MAGIC);
  if(!rtcValido) {
    memset(&rtcData, 0, sizeof(rtcData));
    rtcData.magic = RTC_MAGIC;
    strcpy(rtcData.hUltimaQuedaV, "-");
    strcpy(rtcData.hUltimaQuedaT, "-");
    for(int i=0; i<HIST_SIZE; i++) { rtcData.histV[i]=-1; rtcData.histT[i]=-1; }
    rtcData.ultimoCheckOTA = 0;
    saveData();
  }
  
  // Lógica de Atualização OTA no Boot (Heap RAM máximo livre)
  if (rtcData.magic == RTC_MAGIC && rtcData.webReset == 3) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(20.5);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    int tentativas = 0;
    while (WiFi.status() != WL_CONNECTED && tentativas < 40) {
      delay(500);
      tentativas++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      // Define webReset = 2 para o Telegram sinalizar sucesso ao voltar online
      rtcData.webReset = 2; 
      saveData();
      
      if (!executarOTA()) {
        rtcData.webReset = 0;
        saveData();
      }
    } else {
      rtcData.webReset = 0;
      saveData();
    }
    
    // Se falhar ou não conectar, reinicia o ESP de forma limpa (rodará normal)
    delay(1000);
    ESP.restart();
  }
  
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  configTime(-3 * 3600, 0, "pool.ntp.org");
  
  // Sincronização do NTP corrigida com Timeout de 15 segundos para evitar travamento
  unsigned long startNTP = millis();
  while (time(nullptr) < 1000 && millis() - startNTP < 15000) {
    delay(500);
  }
  
  time_t n = time(nullptr);
  struct tm* t = localtime(&n); 
  char buf[20]; 
  strftime(buf, 20, "%H:%M:%S", t); 
  horaBoot = String(buf);

  // Se o RTC era inválido, inicializa o último dia enviado como hoje para evitar report/reset imediato no boot
  if (!rtcValido && t->tm_year > 100) {
    rtcData.ultimoDiaEnviadoRTC = t->tm_mday;
    rtcData.ultimoCheckOTA = n; // Define o dia de hoje como último check
    saveData();
  }
  
  // Checagem semanal automática de atualizações via GitHub
  if (t->tm_year > 100) {
    if (n - rtcData.ultimoCheckOTA > 7 * 24 * 3600 || rtcData.ultimoCheckOTA == 0) {
      logSistema("Buscando atualizacoes online...");
      String versaoOnline = obterVersaoOnline();
      if (versaoOnline != "" && novaVersaoDisponivel(versaoOnline, VERSAO)) {
        String msg = "🔔 <b>Nova atualizacao disponivel!</b>\n";
        msg += "Versao Atual: " + String(VERSAO) + "\n";
        msg += "Nova Versao: " + versaoOnline + "\n\n";
        msg += "Acesse o portal local em /update para atualizar.";
        sendTelegram(msg);
      }
      rtcData.ultimoCheckOTA = n;
      saveData();
    }
  }
  
  heapNoBoot = ESP.getFreeHeap();
  ultimaAtualizacao = horaBoot;

  // Se o ESP reiniciou durante queda, restaura estado
  if (rtcData.vivoCaido) {
    linkVivoOff = true;
    iniV = (time_t)rtcData.inicioQuedaVivo;
    hIniV = String(rtcData.hUltimaQuedaV);
    logSistema("VIVO: queda em andamento recuperada apos reset");
  }
  if (rtcData.timCaido) {
    linkTimOff = true;
    iniT = (time_t)rtcData.inicioQuedaTim;
    hIniT = String(rtcData.hUltimaQuedaT);
    logSistema("TIM: queda em andamento recuperada apos reset");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/testar", HTTP_POST, handleTestar);
  server.on("/relatorio", HTTP_POST, handleRelatorio); 
  server.on("/pausar", HTTP_POST, [](){ 
    sistemaPausado = !sistemaPausado; 
    logSistema(sistemaPausado ? "MONITOR PAUSADO" : "MONITOR RETOMADO");
    server.sendHeader("Connection", "close");
    server.sendHeader("Location", "/"); server.send(303); 
  });
  server.on("/dnd", HTTP_POST, [](){ 
    modoNaoPerturbe = !modoNaoPerturbe; 
    logSistema(modoNaoPerturbe ? "MENSAGENS OFF" : "MENSAGENS ON");
    server.sendHeader("Connection", "close");
    server.sendHeader("Location", "/"); server.send(303); 
  });
  server.on("/resetar", HTTP_POST, handleReset);

  // Rotas de Configuração (vB1.29)
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  
  // Rota do Portal OTA Customizado e Bonito (Somente Firmware)
  server.on("/update", HTTP_GET, []() {
    if (!server.authenticate("admin", WIFI_PASS)) {
      return server.requestAuthentication();
    }
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", "");
    
    server.sendContent(F("<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<script>(function(){var t=localStorage.getItem('theme')||'auto';function apply(theme){var actual=theme;if(theme==='auto'){actual=window.matchMedia('(prefers-color-scheme:dark)').matches?'dark':'light';}document.documentElement.setAttribute('data-theme',actual);}apply(t);window.matchMedia('(prefers-color-scheme:dark)').addEventListener('change',function(){if(localStorage.getItem('theme')==='auto'||!localStorage.getItem('theme')){apply('auto');}});})();</script>"
      "<style>"
      ":root{--bg:#1a1a1a;--color:#eee;--card-bg:#252525;--border:#333;--text-muted:#aaa;--card-inner-bg:#333;--input-border:#555;}"
      "[data-theme=\"light\"]{--bg:#f5f5f5;--color:#222;--card-bg:#ffffff;--border:#ddd;--text-muted:#666;--card-inner-bg:#eaeaea;--input-border:#ccc;}"
      "body{font-family:sans-serif;background:var(--bg);color:var(--color);text-align:center;margin:0;padding:10px;display:flex;align-items:center;justify-content:center;min-height:90vh;}"
      ".c{max-width:400px;width:100%;background:var(--card-bg);padding:20px;border-radius:8px;box-shadow:0 4px 10px rgba(0,0,0,0.15);border:1px solid var(--border);box-sizing:border-box;}"
      "h2{margin-top:0;color:#00E5FF;font-size:1.4em;}"
      "h3{margin:0 0 5px 0;font-size:1.05em;color:#00E5FF;text-align:left;}"
      "p{font-size:0.8em;color:var(--text-muted);line-height:1.4;text-align:left;margin:0 0 10px 0;}"
      "input[type=file]{display:none;}"
      ".file-label{display:block;background:var(--card-inner-bg);padding:12px;border-radius:5px;border:1px dashed var(--input-border);cursor:pointer;margin:10px 0;font-size:0.9em;color:var(--color);transition:all 0.2s;text-align:center;}"
      ".file-label:hover{background:var(--card-bg);border-color:#00E5FF;}"
      ".btn{display:block;width:100%;background:#00E5FF;color:#000;border:none;padding:12px;border-radius:5px;font-weight:bold;cursor:pointer;font-size:0.95em;transition:all 0.2s;box-sizing:border-box;margin-top:10px;text-align:center;text-decoration:none;}"
      ".btn:hover{background:#00b4cc;box-shadow:0 0 10px rgba(0,229,255,0.3);}"
      ".btn:disabled{background:#555 !important;color:#888 !important;cursor:not-allowed;box-shadow:none !important;}"
      ".back{display:inline-block;margin-top:15px;color:#888;text-decoration:none;font-size:0.8em;}"
      ".back:hover{color:#bbb;}"
      ".screen{display:none;}"
      "</style>"
      "<script>"
      "var versaoAtual = \""));
    
    server.sendContent(VERSAO);
    
    server.sendContent(F("\";"
      "var novaVersao = '';"
      "function updateFileLabel(i){var l=document.getElementById('file-name');if(i.files.length>0){l.innerText='📁 '+i.files[0].name;l.style.borderColor='#00E5FF';l.style.color='var(--color)';}}"
      "function showScreen(id){document.querySelectorAll('.screen').forEach(s=>s.style.display='none');document.getElementById(id).style.display='block';}"
      "function iniciarContador(tipo, targetVersao){"
        "showScreen('screen-progress');"
        "var pt=document.getElementById('progress-title');var ps=document.getElementById('progress-subtitle');var tv=document.getElementById('timer-value');"
        "pt.innerText = (tipo==='manual') ? 'Gravando Firmware...' : 'Baixando & Gravando...';"
        "ps.innerText = 'Não desligue o monitor. O dispositivo reiniciará em breve.';"
        "var seg=25; tv.innerText=seg;"
        "var interval=setInterval(function(){"
          "seg--; tv.innerText=seg;"
          "if(seg<=0){clearInterval(interval); iniciarPolling(targetVersao);}"
        "},1000);"
      "}"
      "function iniciarPolling(targetVersao){"
        "var ps=document.getElementById('progress-subtitle'); ps.innerText='Reconectando ao monitor...';"
        "var tent=0; var maxTent=25;"
        "var poll=setInterval(function(){"
          "tent++;"
          "var xhr=new XMLHttpRequest(); xhr.open('GET','/ota-check',true); xhr.timeout=2000;"
          "xhr.onload=function(){"
            "if(xhr.status===200){"
              "clearInterval(poll);"
              "try{"
                "var res=JSON.parse(xhr.responseText);"
                "var vInst=res.versao_atual||'';"
                "if(vInst && (vInst!==versaoAtual || targetVersao==='')){"
                  "document.getElementById('success-msg').innerText='O monitor foi atualizado com sucesso! Versão instalada: '+vInst;"
                  "showScreen('screen-success');"
                "}else if(targetVersao!=='' && vInst===targetVersao){"
                  "document.getElementById('success-msg').innerText='O monitor foi atualizado com sucesso! Versão instalada: '+vInst;"
                  "showScreen('screen-success');"
                "}else{"
                  "document.getElementById('error-msg').innerText='A atualização foi concluída, mas a versão continua '+versaoAtual+'.';"
                  "showScreen('screen-error');"
                "}"
              "}catch(e){"
                "document.getElementById('success-msg').innerText='O monitor reiniciou e está respondendo na rede!';"
                "showScreen('screen-success');"
              "}"
            "}"
          "};"
          "xhr.onerror=function(){"
            "if(tent>=maxTent){"
              "clearInterval(poll);"
              "document.getElementById('error-msg').innerText='Tempo limite esgotado para reconectar. Verifique se o monitor iniciou.';"
              "showScreen('screen-error');"
            "}"
          "};"
          "xhr.send();"
        "},2000);"
      "}"
      "function verificarNuvem(){"
        "var s=document.getElementById('ota-status');var bc=document.getElementById('btn-check');var bu=document.getElementById('btn-upgrade');"
        "s.innerHTML='🔍 Checando versão no GitHub...';s.style.color='#00E5FF';bc.disabled=true;"
        "var xhr=new XMLHttpRequest();xhr.open('GET','/ota-check',true);"
        "xhr.onload=function(){"
          "bc.disabled=false;"
          "if(xhr.status===200){"
            "var res=JSON.parse(xhr.responseText);"
            "s.innerHTML=res.msg;"
            "if(res.status==='update'){"
              "s.style.color='#4CAF50';bu.style.display='block';novaVersao=res.versao;"
            "}else if(res.status==='ok'){"
              "s.style.color='var(--text-muted)';bu.style.display='none';"
            "}else{"
              "s.style.color='#FF5252';bu.style.display='none';"
            "}"
          "}else{s.innerHTML='❌ Erro ao conectar ao ESP.';s.style.color='#FF5252';}"
        "};"
        "xhr.send();"
      "}"
      "function iniciarNuvem(){"
        "var s=document.getElementById('ota-status');var bc=document.getElementById('btn-check');var bu=document.getElementById('btn-upgrade');"
        "s.innerHTML='🔄 Disparando download...';"
        "bc.disabled=true;bu.disabled=true;"
        "var xhr=new XMLHttpRequest();xhr.open('POST','/ota-start',true);"
        "xhr.onload=function(){iniciarContador('nuvem',novaVersao);};"
        "xhr.onerror=function(){iniciarContador('nuvem',novaVersao);};"
        "xhr.send();"
      "}"
      "function enviarLocal(){"
        "var fi=document.getElementById('file-input'); if(fi.files.length===0){alert('Escolha o firmware .bin primeiro.');return;}"
        "var formData=new FormData(); formData.append('update',fi.files[0]);"
        "showScreen('screen-progress');"
        "var pt=document.getElementById('progress-title');var ps=document.getElementById('progress-subtitle');var tv=document.getElementById('timer-value');"
        "pt.innerText='Enviando Firmware...';ps.innerText='Enviando arquivo local para o chip: 0%';tv.innerText='⏳';"
        "var xhr=new XMLHttpRequest(); xhr.open('POST','/update',true);"
        "xhr.upload.onprogress=function(e){if(e.lengthComputable){var pct=Math.round((e.loaded/e.total)*100);ps.innerText='Enviando: '+pct+'%';}};"
        "xhr.onload=function(){"
          "if(xhr.status===200){iniciarContador('manual','');}"
          "else{document.getElementById('error-msg').innerText='Falha no upload. HTTP '+xhr.status; showScreen('screen-error');}"
        "};"
        "xhr.onerror=function(){document.getElementById('error-msg').innerText='Erro de rede durante o upload.'; showScreen('screen-error');};"
        "xhr.send(formData);"
      "}"
      "</script></head><body>"
      "<div class='c'>"
      
      "<!-- MAIN SCREEN -->"
      "<div id='screen-main' class='screen' style='display:block;'>"
        "<h2>⚙️ Atualizações</h2>"
        "<div style='border-bottom:1px solid var(--border);padding-bottom:15px;margin-bottom:15px;'>"
          "<h3>📂 Enviar Arquivo Local</h3>"
          "<p>Selecione o firmware .bin para gravar manualmente.</p>"
          "<form id='upload-form' onsubmit='event.preventDefault();enviarLocal();'>"
            "<label class='file-label' id='file-name' for='file-input'>📁 Clique para escolher</label>"
            "<input id='file-input' type='file' name='update' onchange='updateFileLabel(this)' accept='.bin'>"
            "<button id='btn-submit' type='submit' class='btn'>📤 Enviar Firmware</button>"
          "</form>"
          "<p style='font-size:0.72em;color:#e57373;margin-top:8px;line-height:1.2;text-align:left;'>⚠️ <b>Aviso de Placa:</b> Compile na IDE do Arduino para placa <b>Generic ESP8266 Module</b> com <b>1MB Flash (No FS)</b>. Se compilar para 4MB (NodeMCU), o firmware não caberá na memória do ESP-01 e dará erro.</p>"
        "</div>"
        "<div style='margin-bottom:15px;'>"
          "<h3>☁️ Atualização via Nuvem</h3>"
          "<p id='ota-status'>Verifique se há novas atualizações publicadas no GitHub.</p>"
          "<button id='btn-check' type='button' class='btn' onclick='verificarNuvem()' style='background:var(--card-inner-bg);border:1px solid var(--input-border);color:var(--color);'>🔍 Verificar Atualizações</button>"
          "<button id='btn-upgrade' type='button' class='btn' onclick='iniciarNuvem()' style='display:none;background:#4CAF50;color:#fff;'>⚡ Atualizar Agora</button>"
        "</div>"
        "<a href='/' class='back'>◀ Voltar ao Portal</a>"
      "</div>"

      "<!-- PROGRESS SCREEN -->"
      "<div id='screen-progress' class='screen'>"
        "<h2 id='progress-title'>Atualizando...</h2>"
        "<div style='margin:30px 0;'>"
          "<div id='timer-circle' style='width:120px;height:120px;border-radius:50%;border:4px solid #00E5FF;margin:0 auto;display:flex;align-items:center;justify-content:center;box-shadow:0 0 15px rgba(0,229,255,0.2);'>"
            "<span id='timer-value' style='font-size:3em;font-weight:bold;color:#00E5FF;'>25</span>"
          "</div>"
        "</div>"
        "<p id='progress-subtitle' style='text-align:center;font-size:0.95em;color:var(--text-muted);'>Gravando firmware no chip...</p>"
      "</div>"

      "<!-- SUCCESS SCREEN -->"
      "<div id='screen-success' class='screen'>"
        "<h2 style='color:#4CAF50;'>🎉 Sucesso!</h2>"
        "<div style='font-size:4em;margin:20px 0;'>🎉</div>"
        "<p id='success-msg' style='text-align:center;font-size:1em;color:var(--color);margin-bottom:20px;'>O monitor foi atualizado com sucesso.</p>"
        "<a href='/' class='btn' style='background:#4CAF50;color:#fff;text-decoration:none;'>◀ Voltar ao Painel Principal</a>"
      "</div>"

      "<!-- ERROR SCREEN -->"
      "<div id='screen-error' class='screen'>"
        "<h2 style='color:#FF5252;'>❌ Falha na Atualização</h2>"
        "<div style='font-size:4em;margin:20px 0;'>❌</div>"
        "<p id='error-msg' style='text-align:center;font-size:1em;color:var(--color);margin-bottom:20px;'>Ocorreu um erro durante o processo.</p>"
        "<button type='button' class='btn' onclick='location.reload()' style='background:#FF5252;color:#fff;'>🔄 Tentar Novamente</button>"
        "<br><a href='/' class='back'>Voltar ao Painel Principal</a>"
      "</div>"

      "</div></body></html>"));
    server.sendContent("");
  });

  server.on("/ota-check", HTTP_GET, []() {
    if (!server.authenticate("admin", WIFI_PASS)) {
      return server.requestAuthentication();
    }
    server.sendHeader("Connection", "close");
    String onlineVer = obterVersaoOnline();
    String resp;
    if (onlineVer == "") {
      resp = "{\"status\":\"erro\",\"versao_atual\":\"" + String(VERSAO) + "\",\"msg\":\"❌ Falha ao conectar ao GitHub.\"}";
    } else if (novaVersaoDisponivel(onlineVer, VERSAO)) {
      String msg = "🟢 Nova versão " + onlineVer + " disponível!";
      resp = "{\"status\":\"update\",\"versao\":\"" + onlineVer + "\",\"versao_atual\":\"" + String(VERSAO) + "\",\"msg\":\"" + msg + "\"}";
    } else {
      resp = "{\"status\":\"ok\",\"versao_atual\":\"" + String(VERSAO) + "\",\"msg\":\"✅ Seu monitor já está atualizado (Versão " + String(VERSAO) + ").\"}";
    }
    server.send(200, "application/json", resp);
  });

  server.on("/ota-start", HTTP_POST, []() {
    if (!server.authenticate("admin", WIFI_PASS)) {
      return server.requestAuthentication();
    }
    server.sendHeader("Connection", "close");
    server.send(200, "application/json", F("{\"status\":\"ok\",\"msg\":\"Iniciando atualização...\"}"));
    delay(1000);
    
    rtcData.webReset = 3; // Sinaliza Boot-OTA (executará no próximo boot)
    saveData();
    
    ESP.restart(); // Reinicia para rodar o OTA com RAM Heap máxima
  });

  server.on("/update", HTTP_POST, []() {
    if (!server.authenticate("admin", WIFI_PASS)) {
      return server.requestAuthentication();
    }
    server.sendHeader("Connection", "close");
    if (Update.hasError()) {
      char errBuf[128];
      snprintf(errBuf, sizeof(errBuf), "Erro %d: %s. Verifique o tamanho de Flash compilado.", Update.getError(), Update.getErrorString());
      String resp = "{\"status\":\"erro\",\"msg\":\"" + String(errBuf) + "\"}";
      server.send(500, "application/json", resp);
    } else {
      rtcData.webReset = 2; // Sinaliza sucesso no boot para o Telegram
      saveData();
      server.send(200, "application/json", F("{\"status\":\"ok\",\"msg\":\"Upload concluído.\"}"));
      delay(1000);
      ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
  
  motivoReset = ESP.getResetReason();
  
  // Inicialização do bot com mensagem comemorativa pós-atualização
  String msg;
  if (rtcData.magic == RTC_MAGIC && rtcData.webReset == 2) {
    msg = "🎉 <b>MONITOR ATUALIZADO COM SUCESSO!</b>\n";
    msg += "🆕 Nova Versão: <b>" + String(VERSAO) + "</b>\n";
    msg += "🌐 <a href='http://" + WiFi.localIP().toString() + "'>Portal Web Local</a>\n";
    msg += "ℹ️ Motivo: Atualização de Firmware";
  } else {
    msg = "🟢 <b>MONITOR ONLINE " + String(VERSAO) + "</b>\n";
    msg += "🌐 <a href='http://" + WiFi.localIP().toString() + "'>Portal Web Local</a>\n";
    msg += "ℹ️ " + traduzirReset(motivoReset);
  }
  
  rtcData.webReset = 0;
  saveData();
  sendTelegram(msg);
  
  logSistema("SISTEMA INICIADO");
}

void loop() {
  server.handleClient();
  unsigned long now = millis();
  time_t agora = time(nullptr); struct tm* t = localtime(&agora);
  
  // Limiar diário às 09h00 (Report + Reset de Variáveis + Reboot em bloco)
  int minutosDesdeMeiaNoite = t->tm_hour * 60 + t->tm_min;
  if (minutosDesdeMeiaNoite >= 9 * 60 && t->tm_mday != rtcData.ultimoDiaEnviadoRTC && t->tm_year > 100) {
    rtcData.resetFeitoHoje = 1;
    enviarRelatorioCompleto(false); 
    rtcData.ultimoDiaEnviadoRTC = t->tm_mday;
    
    // Zera variáveis no RTC
    rtcData.testesV=0; rtcData.onlineV=0; rtcData.quedasV=0; rtcData.instabV=0;
    rtcData.testesT=0; rtcData.onlineT=0; rtcData.quedasT=0; rtcData.instabT=0;
    rtcData.pkgEnviadosV=0; rtcData.pkgPerdidosV=0;
    rtcData.pkgEnviadosT=0; rtcData.pkgPerdidosT=0;
    strcpy(rtcData.hUltimaQuedaV, "-");
    strcpy(rtcData.hUltimaQuedaT, "-");
    
    // Zera o histórico do sparkline
    for(int i=0; i<HIST_SIZE; i++) { rtcData.histV[i]=-1; rtcData.histT[i]=-1; }
    
    saveData();

    sendTelegram("🔄 RESET DIÁRIO AGENDADO...");
    delay(1000);
    ESP.restart(); 
  }
  
  if (!sistemaPausado && now - ultimoTeste >= 60000) { rodarTestes(); }
}
