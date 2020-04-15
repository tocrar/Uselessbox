#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <string>

//Webserver
#include <WiFi.h>
#include <WiFiClient.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h> //Changed TEMPLATE_PLACEHOLDER in WebResponseImpl.h to "~"
#include <DNSServer.h>

//def pins
#define arm_rot 22
#define arm_push 23
#define deckel 21

#define arm_rot_id 1
#define arm_push_id 2
#define deckel_id 3

//def Servo pulse
#define arm_move_push_min 1100
#define arm_move_push_max 1750
#define arm_move_rot_min 750
#define arm_move_rot_max 2450

#define arm_rot_default 1300
#define arm_pressed 1900 // kontakt 1830
#define arm_waiting 1700

#define deckel_min 1060
#define deckel_auf 1600
#define deckel_max 2100

#define hz 50
#define bit_res 12

typedef std::function<String(const String&)> TemplateProcessor;

Preferences preferences;
SemaphoreHandle_t TouchLock;
TaskHandle_t TouchTask, BaseTask, ConfigTask, MoveTask;
AsyncWebServer server(80);
DNSServer dnsServer;

IPAddress apIP(192, 168, 1, 1);
IPAddress netMsk(255, 255, 255, 0);
const byte DNS_PORT = 53;

String switch_box;
String info_box;

bool server_active = false;
unsigned long force_restart = 0;
bool force_restart_active = true;
bool serial_active = false;

uint16_t config[4][3];
uint8_t config_default[2][3] = {{8,30,15},{12,21,18}};

uint8_t user_extra = 0;
uint8_t user_mode = 0;

uint8_t switch_pins[4] = {17, 16, 4, 18}; //B1 17, B2 16, B3 4, B4 18
uint8_t touch_pins[4] = {T4, T7, T6, T5};
uint16_t switch_pos[4] = {850, 1300, 1850, 2400};
uint16_t touch_status[4] = {0, 0, 0, 0};

bool prerun = false;
bool sleeping = false;
/* uint8_t current_pos = 1; */
int current_pos[3] = {0, 0, 0}; // rot, push, lid

// config functions --------------------------------------------------

void load_config(){
  uint8_t config_mode = 0;
  if((user_extra&2) == 2){
    preferences.begin("batterie");
    config_mode = 1;
  }
  else {preferences.begin("normal");}

  char min[7] = "sx_min";
  char max[7] = "sx_max";
  char th[7] = "sx_th";
  for(int i=0; i<4; i++){
    min[1] = char(i+49);
    max[1] = char(i+49);
    th[1] = char(i+49);
    config[i][0] = preferences.getUChar(min, config_default[config_mode][0]);
    config[i][1] = preferences.getUChar(max, config_default[config_mode][1]);
    config[i][2] = preferences.getUChar(th, config_default[config_mode][2]);
  }
  preferences.end();
}

void save_config(){
  if((user_extra&2) == 2){
    preferences.begin("batterie");
  }
  else{
    preferences.begin("normal");
  }
  char min[7] = "sx_min";
  char max[7] = "sx_max";
  char th[7] = "sx_th";
  for(int i=0; i<4; i++){
    min[1] = char(i+49);
    max[1] = char(i+49);
    th[1] = char(i+49);
    preferences.putUChar(min, config[i][0]);
    preferences.putUChar(max, config[i][1]);
    preferences.putUChar(th, config[i][2]);
  }
  preferences.end();
}

//switch base functions ----------------------------------------------
bool is_pressed(uint8_t pos){
  return !digitalRead(switch_pins[pos]);
}

uint8_t get_switchmap(){
  uint8_t switchmap = 0;
  for(int i=0; i<4; i++){
    switchmap = switchmap | int(!digitalRead(switch_pins[i]))<<(3-i);
  }
  return switchmap;
}

//touch base functions -----------------------------------------------
uint16_t is_touched(uint8_t pos){
  xSemaphoreTake(TouchLock, portMAX_DELAY);
  uint16_t status = touch_status[pos];
  xSemaphoreGive(TouchLock);
  return status;
}

//servo functions ----------------------------------------------------
int calc_duty(int pulse, int freq, int res){
  //pulse [microsec], freq [Hz], res [bit]
  return (pow(2, bit_res)-1) * (pulse/1000.0)* (freq/1000.0);
}

bool set_lid(uint16_t pulse){
  if(pulse >= deckel_min && pulse <= deckel_max){
    ledcWrite(deckel_id, calc_duty(pulse, hz, bit_res));
    delay(100);
    return true;
  }
  return false;
}

bool set_rot(uint16_t pulse){
  if(pulse >= arm_move_rot_min && pulse <= arm_move_rot_max){
    ledcWrite(arm_rot_id, calc_duty(pulse, hz, bit_res));
    delay(100);
    return true;
  }
  return false;
}

bool set_push(uint16_t pulse, bool press = false){
  if(pulse >= arm_move_push_min && (pulse <= arm_move_push_max || (press && pulse <= arm_pressed))){
    ledcWrite(arm_push_id, calc_duty(pulse, hz, bit_res));
    delay(100);
    return true;
  }
  return false;
}

// ------------------

void rotate_to_switch(uint8_t pos){
  if(pos != current_pos[0]){
    set_rot(switch_pos[pos]);
    delay(abs(current_pos[0]-pos)*150);
  }
  current_pos[0] = pos;
}

void open_lid(){
  if(!current_pos[2]){
    set_lid(deckel_auf);
    delay(100);
  }
  current_pos[2] = true;
}

void close_lid(bool force = false){
  if(current_pos[2] || force){
    set_lid(deckel_min);
    delay(100);
  }
  current_pos[2] = false;
}

void push_switch(){
  if(is_touched(current_pos[0])){return;}
  set_push(arm_pressed, true);
  current_pos[1] = arm_pressed;
  do{
    if(is_touched(current_pos[0])){break;}
    delay(100);
  }while(is_pressed(current_pos[0]));
  set_push(arm_waiting);
  current_pos[1] = arm_waiting;
  delay(100);
}

void retreat(){
  set_push(arm_move_push_min);
  delay(200);
}

void home_pos(){
  //open lid
  open_lid();
  //reset switches
  for(int i=0;i<4;i++){
    if(is_pressed(i)){
      rotate_to_switch(i);
      push_switch();
    }
  }
  set_push(arm_move_push_min);
  rotate_to_switch(0);
  close_lid();
}

//Server template processor-----------------------------------------------------------

String html_config_processor(const String& var){
  if(var.startsWith("cst")){
    return String(config[int(var.charAt(var.length()-1))-48][0]);
  }
  else if(var.startsWith("csf")){
    return String(config[int(var.charAt(var.length()-1))-48][1]);
  }
  else if(var.startsWith("csh")){
    return String(config[int(var.charAt(var.length()-1))-48][2]);
  }
  return String("N/A");
}

String html_switch_processor(const String& var){
  if(var.startsWith("switch")){
    return !digitalRead(switch_pins[int(var.charAt(var.length()-1))-48]) ? "true" : "false";
  }
  else if(var.startsWith("touch")){
    return String(is_touched(int(var.charAt(var.length()-1))-48))+"s";
  }
  else if(var.startsWith("tval")){
    return String(touchRead(touch_pins[int(var.charAt(var.length()-1))-48]));
  }
  return String("N/A");
}

String html_info_processor(const String& var){
  if(var == "uptime"){
    int sec = millis() / 1000;
    int min = sec / 60;
    int hr = min / 60;
    char buffer[10];
    snprintf(buffer, 10, "%02d:%02d:%02d", hr, min % 60, sec % 60);
    return String(buffer);
  }
  else if (var == "mode"){

    switch (user_mode)
    {
    case 0:
      return "Touch";
      break;
    case 2:
      return "Move";
      break;
    case 4:
      return "Config";
      break;
    case 6:
      return "Kiosk";
      break;
    case 8:
      return "No Touch";
      break;
    }
    return "None";
  }
  else if (var == "conf"){
    if ((user_extra&2) == 2){return "Battery";}
    else {return "Wired";}
  }
  else if (var == "serial"){
    return serial_active ? "true" : "false";
  }
  else if (var == "reset"){
    return force_restart_active ? "true" : "false";
  }
  return String("N/A");
}

void html_template_processor(AsyncResponseStream *response, String html, TemplateProcessor callback){
  bool keymode = false;
  String keybuffer;
  char placeholder = '~';
  for(int i=0; i<html.length(); i++){
    if(!keymode && html[i] != placeholder){
      response->print(html[i]);
    }
    else if(!keymode && html[i] == placeholder){
      keymode = true;
      keybuffer = "";
    }
    else if(keymode && html[i] != placeholder){
      keybuffer += html[i];
    }
    else if(keymode && html[i] == placeholder){
      if(keybuffer.length() == 0){
        response->print(placeholder);
        continue;
      }
      response->print(callback(keybuffer));
      keymode = false;
    }
  }
}

// spiffs functions--------------------------------------------------

String readFile(fs::SPIFFSFS filesystem , String path){
  String s_tmp = "";
  File file = filesystem.open(path);
  while(file.available()){
    char tmp = file.read();
    s_tmp += tmp;
  }
  file.close();
  return s_tmp;
}

// sleep functions---------------------------------------------------

void start_sleep(){
  ledcDetachPin(arm_rot);
  ledcDetachPin(arm_push);
  ledcDetachPin(deckel);
  sleeping = true;
}

void stop_sleep(){
  if(sleeping){
    ledcAttachPin(arm_rot, arm_rot_id);
    ledcAttachPin(arm_push, arm_push_id);
    ledcAttachPin(deckel, deckel_id);
    sleeping = false;
  }
}

/* void callback(){
  Serial.print("Touch me");
} */

// Code for Tasks ---------------------------------------------------

void codeForTouchTask( void * parameter ){
  //touchAttachInterrupt(touch_pins[0], callback, Threshold);
  //touchAttachInterrupt(touch_pins[1], callback, Threshold);
  //touchAttachInterrupt(touch_pins[2], callback, Threshold);
  //touchAttachInterrupt(touch_pins[3], callback, Threshold);
  uint8_t runval[4] = {100, 100, 100, 100};
  unsigned long starttime[4] = {0,0,0,0};
  for (;;) {
    for (int i = 0; i < 4; i++) {
      int val = touchRead(touch_pins[i]);
      if(val <= 5){
        continue;
      }

      runval[i] = runval[i] * 0.8 + val * 0.2;
      if(val < config[i][2]){
        if(starttime[i] == 0){
          starttime[i] = millis();
        }
          xSemaphoreTake(TouchLock, portMAX_DELAY);
          touch_status[i] = 1+(uint16_t)((millis()- starttime[i])/1000);
          xSemaphoreGive(TouchLock);
      }
      else{
        xSemaphoreTake(TouchLock, portMAX_DELAY);
        touch_status[i] = 0;
        xSemaphoreGive(TouchLock);
        starttime[i] = 0;
      }
    }
    delay(5);
  }
}

void codeForBaseTask(void * parameter){
  for(;;){
    uint8_t switches = get_switchmap();
    if(switches == 0){
      int pos = -1;
      for(int i=0; i<4; i++){
        if(is_touched(i)){
          if(pos == -1){
            pos = i;
          }
          else{
            pos = -1;
            break;
          }
        }
      }
      if(pos != -1){
        uint16_t t_time = is_touched(pos);
        if(t_time > 1){
          rotate_to_switch(pos);
          if(t_time > 2){
            /* rotate_to_switch(pos); */
            open_lid();
            set_push(arm_move_push_max);
          }
        }
      }
      else{
        retreat();
        close_lid();
      }
    }
    else{
      uint8_t next_target = 5;
      for(int i=0; i<4; i++){
        if(!is_touched(i) && is_pressed(i) && abs(current_pos[0]-i) < next_target){
          next_target = i;
        }
      }
      if(next_target < 5){
        rotate_to_switch(next_target);
        open_lid();
        push_switch();
      }
      else{
        retreat();
        close_lid();
      }
    }
  }
}

void codeForConfigTask(void * parameter){
  //Configure Touch sensitivity
  home_pos();
  //ledcWrite(deckel_id, calc_duty(deckel_max, hz, bit_res));
  set_lid(deckel_max);

  uint8_t config_mode = 0;
  if((user_extra&2) == 2){
    config_mode = 1;
  }
  for(int i=0;i<4;i++){
    config[i][0] = config_default[config_mode][0];
    config[i][1] = config_default[config_mode][1];
    config[i][2] = config_default[config_mode][2];
  }

  delay(2000);
  uint8_t samplesize;
  uint16_t summe[8] = {0,0,0,0,0,0,0,0};
  for(samplesize=1; samplesize < 250; samplesize++){
    for(int i=0;i<4;i++){
      //get average value for no touch
      int temp = touchRead(touch_pins[i]);
      if(temp > 10){
        //config[i][1] += temp;
        summe[i+4] += temp;
      }
      else{
        //config[i][1] += config[i][1]/samplesize;
        summe[i+4] += summe[i+4]/samplesize;
      }
    }
    delay(50);
  }
  for(int i=0;i<4;i++){
    //config[i][1] /= samplesize;
    rotate_to_switch(i);
    //ledcWrite(arm_push_id, calc_duty(arm_waiting, hz, bit_res));
    set_push(arm_waiting);
    delay(2000);
    //get average value for touched
    for(uint8_t t=1;t<250;t++){
      //config[i][0] += touchRead(touch_pins[i]);
      summe[i] += touchRead(touch_pins[i]);
      delay(5);
    }
    config[i][0] = summe[i]/250;
    config[i][1] = summe[i+4]/samplesize;
    config[i][2] = int(config[i][0]+(config[i][1]-config[i][0])*0.4);
    //ledcWrite(arm_push_id, calc_duty(arm_move_push_min, hz, bit_res));
    set_push(arm_move_push_min);
  }

  //save to nvs
  save_config();

  if(serial_active){
    Serial.println("Results:");
    Serial.println("Pos | true | false | TH");
    for(int i= 0; i<4; i++){
      Serial.print(i);
      Serial.print("   | ");
      Serial.print(config[i][0]);
      Serial.print("   | ");
      Serial.print(config[i][1]);
      Serial.print("   | ");
      Serial.println(config[i][2]);
    }
  }
  ledcWrite(deckel_id, calc_duty(deckel_min, hz, bit_res));
  delay(1000);
  if(!server_active){ESP.restart();}
  for(;;){delay(1000);}
}

void codeForMoveTask(void * parameter){
  uint16_t rot = arm_move_rot_min;
  uint16_t push = arm_move_push_min;
  uint16_t alt_rot = arm_move_rot_min;
  uint16_t alt_push = arm_move_push_min;
  
  home_pos();
  set_lid(deckel_max);

  uint16_t faktor;
  for(;;){
    faktor = is_touched(0);
    if(faktor > 1){faktor = 10;}
    if(faktor && rot-faktor > arm_move_rot_min){
      //left
      rot -= faktor;
    }
    else if(faktor && rot != arm_move_rot_min){
      rot = arm_move_rot_min;
    }

    faktor = is_touched(1);
    if(faktor > 1){faktor = 10;}
    if(faktor && rot+faktor < arm_move_rot_max){
      //right
      rot += faktor;
    }
    else if(faktor && rot != arm_move_rot_max){
      rot = arm_move_rot_max;
    }

    faktor = is_touched(2);
    if(faktor > 1){faktor = 10;}
    if(faktor && push+faktor < arm_move_push_max){
      //push
      push += faktor;
    }
    else if(faktor && push != arm_move_push_max){
      push = arm_move_push_max;
    }

    faktor = is_touched(3);
    if(faktor > 1){faktor = 10;}
    if(faktor && push-faktor > arm_move_push_min){
      //retreat
      push -= faktor;
    }
    else if(faktor && push != arm_move_push_min){
      push = arm_move_push_min;
    }

    if(alt_push != push){
      //ledcWrite(arm_push_id, calc_duty(push, hz, bit_res));
      set_push(push);
      alt_push = push;
      if(serial_active){      
        Serial.print("push: ");
        Serial.print(push);
        Serial.print(" | rot: ");
        Serial.println(rot);
      }
    }
    if(alt_rot != rot){
      //ledcWrite(arm_rot_id, calc_duty(rot, hz, bit_res));
      set_rot(rot);
      alt_rot = rot;
      if(serial_active){ 
        Serial.print("push: ");
        Serial.print(push);
        Serial.print(" | rot: ");
        Serial.println(rot);
      }
    }
    delay(10);
  }
}

void server_setup(){
  if(serial_active){ 
    Serial.println("Server active");
  }
  // Webserver
  WiFi.persistent(false);
  WiFi.setHostname("uselessbox");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("UselessBox", "UselessBox");
  delay(1000);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  if(serial_active){
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  }
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  SPIFFS.begin(false, "/spiffs", 20); // maxOpenFiles=20

  switch_box = readFile(SPIFFS, "/switch_box.html");
  info_box = readFile(SPIFFS,"/info_box.html");

  // favicon
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/favicon.ico", "image/vnd.microsoft.icon");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });

  // js
  server.on("/jQuery.js", HTTP_GET, [](AsyncWebServerRequest *request){
      AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/jQuery.js.gz", "text/javascript");
      response->addHeader("Content-Encoding", "gzip");
      response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
      request->send(response);
    });

  server.on("/main.js", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/main.js", "text/javascript");
        response->addHeader("Cache-Control", "max-age=300, must-revalidate");
        request->send(response);
    });

  // CSS
  server.on("/main.css", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/main.css", "text/css");
        response->addHeader("Cache-Control", "max-age=300, must-revalidate");
        request->send(response);
    });

  server.on("/color_scheme.css", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/color_scheme.css", "text/css");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });

  server.on("/fa-minimal.css", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-minimal.css", "text/css");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });

  // Index
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/index.html", "text/html", false, html_info_processor);
    });

  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/index.html", "text/html", false, html_info_processor);
    });

    server.on("/sidenav.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/sidenav.html", "text/html");
    });

  server.on("/config_page.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/config_page.html", "text/html");
    });

  server.on("/move.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/move.html", "text/html");
    });

  // POSTS
  server.on("/move_data", HTTP_GET, [](AsyncWebServerRequest *request){
        int coords[2] = {0,0};
        if(request->hasParam("x")){
          coords[0] = (request->getParam("x")->value()).toInt();
        }
        if(request->hasParam("y")){
          coords[1] = (request->getParam("y")->value()).toInt();
        }
        Serial.print("x: "); Serial.print(coords[0]); Serial.print("| y: "); Serial.println(coords[1]);
        request->send(200, "text/plain","done");
    });

  // Status Boxes
  server.on("/info_box.html", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *response = request->beginResponseStream("text/html");
        html_template_processor(response, info_box, html_info_processor);
        request->send(response);
    });
  server.on("/switch_box.html", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream *response = request->beginResponseStream("text/html");
        html_template_processor(response, switch_box, html_switch_processor);
        request->send(response);
    });
  server.on("/config_box.html", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(SPIFFS, "/config_box.html", "text/html", false, html_config_processor);
    });

  // Fonts
  // fa-regular
  server.on("/webfonts/fa-regular-400.ttf", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-regular-400.ttf", "font/ttf");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });
  server.on("/webfonts/fa-regular-400.woff", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-regular-400.woff", "font/woff");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });
  server.on("/webfonts/fa-regular-400.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-regular-400.woff2", "font/woff2");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });
  // fa-solid
  server.on("/webfonts/fa-solid-900.ttf", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-solid-900.ttf", "font/ttf");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });
  server.on("/webfonts/fa-solid-900.woff", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-solid-900.woff", "font/woff");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });
  server.on("/webfonts/fa-solid-900.woff2", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(SPIFFS, "/fa-solid-900.woff2", "font/woff2");
        response->addHeader("Cache-Control", "max-age=3600, must-revalidate");
        request->send(response);
    });

  server.begin();

  server_active = true;
  if(serial_active){ 
    Serial.println("Server started");
  }
}

void reset_setup(){
  if(serial_active){ 
    Serial.println("force restart deactivated");
  }
  force_restart_active = false; 
}

void serial_setup(){
  Serial.begin(115200);
  serial_active = true;
  delay(100);
  Serial.println("Serial active");
}

void setup() {
  //Serial setup
  TouchLock = xSemaphoreCreateMutex();

  //switch setup
  // uint8_t switchmap = 0;
  for(int i=0; i<4; i++){
    pinMode(switch_pins[i], INPUT_PULLUP);
    delay(100);
    user_extra = user_extra | int(!digitalRead(switch_pins[i]))<<(3-i);
  }

  //Servo setup
  ledcSetup(arm_rot_id, hz, bit_res);
  ledcAttachPin(arm_rot, arm_rot_id);
  /* ledcWrite(arm_rot_id, calc_duty(arm_rot_default, hz, bit_res)); */
  rotate_to_switch(1);

  ledcSetup(arm_push_id, hz, bit_res);
  ledcAttachPin(arm_push, arm_push_id);
  /* ledcWrite(arm_push_id, calc_duty(arm_move_push_min, hz, bit_res)); */
  retreat();
  delay(500);

  ledcSetup(deckel_id, hz, bit_res);
  ledcAttachPin(deckel, deckel_id);
  /* ledcWrite(deckel_id, calc_duty(deckel_min, hz, bit_res)); */
  close_lid(true);
  
  if((user_extra&8) == 8){serial_setup();} //Switch 2 serial
  if((user_extra&4) == 4){server_setup();} //Switch 1 server
  load_config();  //Switch 3 config 1/2
  if(serial_active && (user_extra&2) == 2){ 
    Serial.println("Battery config");
  }

  if((user_extra&1) == 1){
    /*mode select
    0: Touch
    8: No Touch
    4: Config
    2: Move
    6: Kiosk
    */
    while(!digitalRead(switch_pins[3])){
      if(server_active){dnsServer.processNextRequest();}
      delay(100);
    }
    user_mode = get_switchmap();
  }

  switch(user_mode)
  {
    case 6:
      reset_setup();
    
    case 0:
      if(serial_active){ 
        Serial.println("Touch mode");
      }
      //Touch
      xTaskCreatePinnedToCore(
        codeForTouchTask, "TouchTask", 1000, NULL, 1, &TouchTask, 0);

    case 8:
      if(serial_active){ 
        Serial.println("Base active");
      }
      //Base
      xTaskCreatePinnedToCore(
        codeForBaseTask, "BaseTask", 1000, NULL, 1, &BaseTask, 1);
      break;

    case 4:
      if(serial_active){ 
        Serial.println("Config mode");
      }
      //Config
      xTaskCreatePinnedToCore(
        codeForConfigTask, "ConfigTask", 3000, NULL, 1, &ConfigTask, 1);
      break;

    case 2:
      //Touch
      xTaskCreatePinnedToCore(
        codeForTouchTask, "TouchTask", 1000, NULL, 1, &TouchTask, 0);
      //Arm controll
      if(serial_active){ 
        Serial.println("Move mode");
      }
      xTaskCreatePinnedToCore(
        codeForMoveTask, "MoveTask", 1000, NULL, 1, &ConfigTask, 1);
      break;
  }

  delay(500);
}


void loop() {
  //selfdestruct loop
  //vTaskDelete(NULL);
  if(server_active){dnsServer.processNextRequest();}
  if(force_restart_active){
    if(touchRead(touch_pins[0]) < config[0][2] && touchRead(touch_pins[1]) < config[1][2] && touchRead(touch_pins[2]) < config[2][2] && touchRead(touch_pins[3]) < config[3][2]){
      if(force_restart == 0){
        force_restart = millis();
      }
      else if((millis()-force_restart) > 5000){
        if(serial_active){
          Serial.println("force reboot");
          delay(10);
        }
        ESP.restart();
      }
    }
    else{
      force_restart = 0;
    }
  }
  delay(10);
}
