#include "Arduino.h"
#include "time.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>  // https://arduinojson.org/v7/assistant/#/step1
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_MCP23X17.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "config.h"

#define BUILT_LED 2

Adafruit_MCP23X17 mcp;

int Boot_Timestamp = 0; //czas uruchomienia (uniksowy)

String accessToken;
String refreshToken;

String WiFi_IP = ""; //WiFi IP uzupełniane po nawiązaniu połączenia

String Mqtt_Server; //MQTT broker address
int Mqtt_Port; //MQTT broker port
String Mqtt_User; //MQTT broker username
String Mqtt_Password; //MQTT broker user password
const int Mqtt_Buffer_Size = 512; //powiększony bufor ze względu na systemStatus (do usunięcie, gdy systemStatus będzie okrojony do informacji zmieniających się)

const int Blinds_Count = 4; //ilość obsługiwanych rolet
const int Blinds_Id[4] = {3,4,5,6}; //id rolet obsługiwanych przez to urządzenie
const int Mcp_Up_Pin[4] = {12,15,10,9}; //numery pinów MCP sterujących przejazdem w górę
const int Mcp_Down_Pin[4] = {13,14,11,8}; //numery pinów MCP sterujących przejazdem w dół
const int Mcp_Sensor_Up_Pin[4] = {0,2,5,6}; //numery pinów MCP krańcówek górnych
const int Mcp_Sensor_Down_Pin[4] = {1,3,4,7}; //numery pinów MCP krańcówek dolnych
const int Blinds_Speed_Pin[4] = {26,25,33,32}; //numery pinów ESP sterujących prędkością
bool Blinds_Move_Up[4] = {}; //stan dla pinów MCP dla przejazdów w górę
bool Blinds_Move_Down[4] = {}; //stan dla pinów MCP dla przejazdów w dół
bool Blinds_Sensor_Up[4] = {}; //stan MCP na pinach wejściowych krańcówej górnych
bool Blinds_Sensor_Down[4] = {}; //stan MCP na pinach wejściowyck krańcówek dolnych
bool Blinds_To_Calibrate[4] = {}; //wskaźniki, czy skalibrować roletę
float Blinds_Position[4] = {}; //aktualne pozycje rolet w %
int Blinds_Speed_Set[4] {100,100,100,100}; //nastawienie prędkości rolety w %
int Blinds_Runtime_Up[4] = {}; //czas przebiegu rolet w górę - ze 100% do 0%
int Blinds_Runtime_Down[4] = {}; //czas przebiegu rolet w dół - z 0% do 100%
int Blinds_Pass_Up[4] = {}; //czas przejazdu poza górną krańcówkę
int Blinds_Pass_Down[4] = {}; //czas przejazdu poza dolną krańcówkę
int Blinds_Set[4] = {}; //wymagane położenie rolet otrzymane przez MQTT

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
OneWire oneWire(15);
DallasTemperature sensors(&oneWire);

const String myHostname = "ssh_device_" + DEVICE_ID;
typedef struct {int param;} TaskParams;

void connectWiFi();
void callback(char*, byte*, unsigned int);
void connectMqtt();
void mcpLoop(void*);
void calibrateBlind(int);
void setBlinds(void*);
void publishBlinds(void*);
void systemStatus(void*);
void publishErrors(void*);

bool apiGetTokens();
bool apiRefreshToken();
bool apiGetConfig();
bool apiGetBlinds();
void xGetTokens(void*);
void xRefreshToken(void*);
void apiUpdatePosition(void*);


void setup()
{
  Serial.begin(115200);
  Serial.println();
  Serial.println(myHostname + " is online");

  pinMode(BUILT_LED, OUTPUT);
  digitalWrite(BUILT_LED, LOW);

  Wire.begin(21,22);

  if (!mcp.begin_I2C()) {
    Serial.println("MCP23017 Error");
    while (true);
  }

  for (int i=0; i < Blinds_Count; i++)
  {
    mcp.pinMode(Mcp_Up_Pin[i], OUTPUT);
    mcp.digitalWrite(Mcp_Up_Pin[i], HIGH);
    mcp.pinMode(Mcp_Down_Pin[i], OUTPUT);
    mcp.digitalWrite(Mcp_Down_Pin[i], HIGH);
    mcp.pinMode(Mcp_Sensor_Up_Pin[i], INPUT);
    mcp.pinMode(Mcp_Sensor_Down_Pin[i], INPUT);
    pinMode(Blinds_Speed_Pin[i], OUTPUT);
    digitalWrite(Blinds_Speed_Pin[i], LOW);
  }

  xTaskCreate(
    mcpLoop,           // Function that should be called
    "MCP Read Write",  // Name of the task (for debugging)
    3000,              // Stack size (bytes)
    NULL,              // Parameter to pass
    10,                // Task priority
    NULL               // Task handle
  );

  connectWiFi();
  while (!apiGetTokens()) { delay(1000); }
  delay(100);
  while (!apiGetConfig()) { delay(1000); }
  while (!apiGetBlinds()) { delay(1000); }
  connectMqtt();

  for(int i = 0; i < Blinds_Count; i++)
  {
    TaskParams* params = new TaskParams;
    params->param = i;
    String s = "Task_" + String(i);
    char* taskName = strdup(s.c_str());

    xTaskCreate(
      setBlinds,
      taskName,
      3000,
      params,
      1,
      NULL
    );
  }

  xTaskCreate(
    publishBlinds,
    "Blinds publish",
    3000,
    NULL,
    tskIDLE_PRIORITY,
    NULL
  );

  xTaskCreate(
    systemStatus,
    "System status",
    3000,
    NULL,
    tskIDLE_PRIORITY,
    NULL
  );

  xTaskCreate(
    xRefreshToken,
    "Refresh API Token",
    3000,
    NULL,
    tskIDLE_PRIORITY,
    NULL
  );

  xTaskCreate(
    xGetTokens,
    "Get new API Tokens",
    3000,
    NULL,
    tskIDLE_PRIORITY,
    NULL
  );

  xTaskCreate(
    apiUpdatePosition,
    "Update blinds position in API",
    3000,
    NULL,
    tskIDLE_PRIORITY,
    NULL
  );
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); }
  if (!mqttClient.connected()) { connectMqtt(); }
  mqttClient.loop();
  vTaskDelay(pdMS_TO_TICKS(1000));
}


void connectWiFi()
{ // ustanawianie połączenia WiFi
  digitalWrite(BUILT_LED, LOW);
  WiFi.hostname(myHostname);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  Serial.print("Connecting to WiFi ");

  while (WiFi.status() != WL_CONNECTED)
  {
    if (attempt == 30)
        {
      Serial.println("Reboot po 30 probach polaczenia z WiFi");
      ESP.restart();
    }
    ++attempt;
    vTaskDelay(pdMS_TO_TICKS(200));

    wl_status_t status = WiFi.status();
    if(status != WL_CONNECTED) {
      digitalWrite(BUILT_LED, HIGH);
      Serial.print("   Connecting to WiFi error: ");
      Serial.println(status);
        // WL_NO_SHIELD	      255   assigned when no WiFi shield is present
        // WL_IDLE_STATUS	    0     it is a temporary status assigned when WiFi.begin() is called and remains active until the number of attempts expires (resulting in WL_CONNECT_FAILED) or a connection is established (resulting in WL_CONNECTED)
        // WL_NO_SSID_AVAIL	  1     assigned when no SSID are available
        // WL_SCAN_COMPLETED	2     assigned when the scan networks is completed
        // WL_CONNECTED	      3     assigned when connected to a WiFi network
        // WL_CONNECT_FAILED	4     assigned when the connection fails for all the attempts
        // WL_CONNECTION_LOST	5     assigned when the connection is lost
        // WL_DISCONNECTED	  6     assigned when disconnected from a network

      vTaskDelay(pdMS_TO_TICKS(100));
      digitalWrite(BUILT_LED, LOW);
    }
  }

  WiFi_IP = WiFi.localIP().toString();
  Serial.println("Connected to WiFi:");
  Serial.println("         IP: " + String(WiFi_IP));
  Serial.println("   Hostname: " + myHostname);
  long rssi = WiFi.RSSI();
  Serial.print("     Signal: ");
  Serial.print(rssi);
  Serial.println(" dBm");

  //TODO API POST
}

bool apiGetTokens()
{ // pobierane tokenów uwierzytelniających API
  if(WiFi.status() == WL_CONNECTED)
  {
    String url = API_URL + "/token/";
    String payload = "{\"username\":\"" + API_USERNAME + "\",\"password\":\"" + API_PASSWORD + "\"}";

    HTTPClient httpClient;
    httpClient.begin(url);
    httpClient.addHeader("Content-Type", "application/json");
    int httpResponseCode = httpClient.POST(payload);

    if (httpResponseCode > 0) 
    {
      if (httpResponseCode == 200)
      {
        String response = httpClient.getString();
        // Serial.println(response);
        JsonDocument doc;
        deserializeJson(doc, response);
        accessToken = doc["access"].as<String>();
        refreshToken = doc["refresh"].as<String>();
        Serial.println("API tokens received");
        return true;
      }
      else
      {
        Serial.print("API - Error code: ");
        Serial.println(httpResponseCode);
      }
    }
    else
    {
      Serial.print("API - Error on sending POST: ");
      Serial.println(httpResponseCode);
    }  
    httpClient.end();
  }
  return false;
}

bool apiRefreshToken()
{ // odświerzenie tokenu uwierzytelniającego API tokenem refresh
  if(WiFi.status() == WL_CONNECTED)
  {
    String url = API_URL + "/token/refresh/";
    String payload = "{\"refresh\":\"" + refreshToken + "\"}";

    HTTPClient httpClient;
    httpClient.begin(url);
    httpClient.addHeader("Content-Type", "application/json");
    int httpResponseCode = httpClient.POST(payload);

    if (httpResponseCode > 0) 
    {
      if (httpResponseCode == 200)
      {
        String response = httpClient.getString();
        // Serial.println(response);
        JsonDocument doc;
        deserializeJson(doc, response);
        accessToken = doc["access"].as<String>();
        return true;
      }
      else
      {
        Serial.print("API Error code: ");
        Serial.println(httpResponseCode);
      }
    }
    else
    {
      Serial.print("API Error on sending POST: ");
      Serial.println(httpResponseCode);
    }
  
    httpClient.end();
  }
  return false;
}

bool apiGetConfig()
{ // pobranie dany  konfiguracyjnych przez API
  if(WiFi.status() == WL_CONNECTED)
  {
    String url = API_URL + "/configurations/1/";
    HTTPClient httpClient;
    httpClient.begin(url);
    httpClient.addHeader("Content-Type", "application/json");
    httpClient.addHeader("Authorization", "Bearer " + accessToken);
    int httpResponseCode = httpClient.GET();

    if (httpResponseCode > 0) 
    {
      if (httpResponseCode == 200)
      {
        String response = httpClient.getString();
        // Serial.println(response);
        JsonDocument doc;
        deserializeJson(doc, response);
        String Ntp_Server = doc["ntp_server"].as<String>();
        Mqtt_Server = doc["mqtt_server"].as<String>();
        Mqtt_Port = doc["mqtt_port"].as<int>();
        Mqtt_User = doc["mqtt_user"].as<String>();
        Mqtt_Password = doc["mqtt_password"].as<String>();
        Serial.println("API configs received");

        // ustawienie aktualnego czau
        configTime(7200, 0, Ntp_Server.c_str(), "ntp.certum.pl");
        Serial.println("Real-time synchronization:");

        time_t now;
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
          Serial.println("Failed to obtain time");
        }

        time(&now);
        Boot_Timestamp = now;
        Serial.print("   Unixtime: ");
        Serial.println(now); // czas uniksowy

        char buffer[26];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.print("   Datetime: ");
        Serial.println(buffer);
        
        httpClient.end();
        return true;
      }
      else
      {
        Serial.print("API Error code: ");
        Serial.println(httpResponseCode);
      }
    }
    else
    {
      Serial.print("API Error on sending POST: ");
      Serial.println(httpResponseCode);
    }  
    httpClient.end();
  }
  else
  {
    connectWiFi();
  }
  return false;
}

bool apiGetBlinds()
{ // pobranie danych rolet przez API
  if(WiFi.status() == WL_CONNECTED)
  {
    String url = API_URL + "/blinds/";
    HTTPClient httpClient;
    httpClient.begin(url);
    httpClient.addHeader("Content-Type", "application/json");
    httpClient.addHeader("Authorization", "Bearer " + accessToken);
    int httpResponseCode = httpClient.GET();

    if (httpResponseCode > 0)
    {
      if (httpResponseCode == 200)
      {
        String response = httpClient.getString();
        // Serial.println(response);
        JsonDocument doc;
        deserializeJson(doc, response);
        for (JsonObject item : doc.as<JsonArray>()) {
          int id = item["id"];
          for (int i=0; i < Blinds_Count; i++)
          {
            if (id == Blinds_Id[i]) {
              Blinds_Position[i] = item["position"].as<int>();
              Blinds_Runtime_Up[i] = item["runtime_up"].as<int>();
              Blinds_Runtime_Down[i] = item["runtime_down"].as<int>();
              Blinds_Pass_Up[i] = item["pass_up"].as<int>();
              Blinds_Pass_Down[i] = item["pass_down"].as<int>();
              Blinds_Set[i] = item["position"].as<int>();
              break;
            }
          }
        }
        httpClient.end();
        return true;
      }
      else
      {
        Serial.print("API Error code: ");
        Serial.println(httpResponseCode);
      }
    }
    else
    {
      Serial.print("API Error on sending POST: ");
      Serial.println(httpResponseCode);
    }  
    httpClient.end();
  }
  else
  {
    connectWiFi();
  }
  return false;
}

void xGetTokens(void* parameters)
{ // zadanie odświerzania obu tokenów API
  int delay = 85000000;
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(delay));
    delay = 10000;
    if (apiGetTokens())
    {
      delay = 85000000; // pobieranie nowych tokenów co 23.6 godziny
    }
  }
}

void xRefreshToken(void* parameters)
{ // zadanie odświerzania tokena uwierzytelniającego
  int delay = 270000;
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(delay));
    delay = 10000;
    if (apiRefreshToken())
    {
      delay = 270000; // odświerzanie tokena co 4.5 minuty
    }
  }
}

void connectMqtt()
{ // ustanawianie połączenia MQTT
  digitalWrite(BUILT_LED, LOW);
  int hostnameLenght = myHostname.length() + 1;
  char myMqttName[hostnameLenght];
  myHostname.toCharArray(myMqttName, hostnameLenght);

  mqttClient.setServer(Mqtt_Server.c_str(), Mqtt_Port);
  mqttClient.setCallback(callback);
  mqttClient.setBufferSize(Mqtt_Buffer_Size);

  while (!mqttClient.connected()) {
    vTaskDelay(pdMS_TO_TICKS(500));
    digitalWrite(BUILT_LED, HIGH);
    Serial.println("Connecting to MQTT...");

    //willMessage:
    String json = "{\"device\":{\"id\":" + DEVICE_ID 
    + ",\"name\":\"" + myHostname 
    + "\",\"type\":\"" + ESP.getChipModel() 
    + "\",\"online\":false}}";

    if (mqttClient.connect(myMqttName, Mqtt_User.c_str(), Mqtt_Password.c_str(), String("ssh/devices/status/" + DEVICE_ID).c_str(), 1, true, json.c_str()))
    {
      Serial.println("Connected to MQTT");
      mqttClient.subscribe("ssh/blinds/set/#"); //kanał wiadomości nastawiania rolet
      // mqttClient.publish("ssh/test", "hello");
    } 
    else 
    {
      Serial.print("MQTT Client Failed with state ");
      Serial.println(mqttClient.state());
        // -4 : MQTT_CONNECTION_TIMEOUT - the server didn't respond within the keepalive time
        // -3 : MQTT_CONNECTION_LOST - the network connection was broken
        // -2 : MQTT_CONNECT_FAILED - the network connection failed
        // -1 : MQTT_DISCONNECTED - the client is disconnected cleanly
        //  0 : MQTT_CONNECTED - the client is connected
        //  1 : MQTT_CONNECT_BAD_PROTOCOL - the server doesn't support the requested version of MQTT
        //  2 : MQTT_CONNECT_BAD_CLIENT_ID - the server rejected the client identifier
        //  3 : MQTT_CONNECT_UNAVAILABLE - the server was unable to accept the connection
        //  4 : MQTT_CONNECT_BAD_CREDENTIALS - the username/password were rejected
        //  5 : MQTT_CONNECT_UNAUTHORIZED - the client was not authorized to connect
      
      if (WiFi.status() != WL_CONNECTED) 
      {
        Serial.println("WiFi disconnected!");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      digitalWrite(BUILT_LED, LOW);
    }
  }
  digitalWrite(BUILT_LED, HIGH);
}

void callback(char* topic, byte* payload, unsigned int length)
{ //odebranie wiadomości MQTT
  char *token;
  const char *delimiter ="/";
  String splitTopic[4] = {};
  token = strtok(topic, delimiter);

  int i = 0;
  while (token != NULL) 
  {
    splitTopic[i] = token;
    i++;
    token=strtok(NULL, delimiter);
  }

  if (splitTopic[0] == "ssh" and splitTopic[1] == "blinds" and splitTopic[2] == "set")
  {
    for (int i = 0; i < Blinds_Count; i++)
    {
      if (String(Blinds_Id[i]) == splitTopic[3])
      {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
          Serial.print("deserializeJson() failed: ");
          Serial.println(error.c_str());
        }

        // int id = doc["id"];
        bool calibrate = doc["calibrate"];
        int set = doc["set"];
        int speed = doc["speed"];

        if (set < 0 or set > 100)
        {
          Serial.println("otrzymana wartość nastawienia rolety poza dopuszczalnymi granicami");
        }
        else if (speed < 70 or speed > 100)
        {
          Serial.println("otrzymana wartość prędkości rolety poza dopuszczalnymi granicami");
        }
        else
        {
          Blinds_To_Calibrate[i] = calibrate;
          Blinds_Set[i] = set;

          // ponieważ aktualnie kontrola położenia obliczana jest przez czas przeazdu
          // zmiana tej prędkości spowoduje błędy w osiąganiu wymaganej pozycji rolety
          // być może sens miałoby zaimplementowanie tylko do przejazdów na krańcówki
          // lub obsługa tylko dwóch czy trzech prędkości
          // Blinds_Speed_Set[i] = speed;
        }
      }
    }
  }
}

void systemStatus(void* parameters)
{ // przesyłanie przez MQTT informacji o urządzeniu
  while (true)
  {
    if (mqttClient.connected())
    {
      time_t now;
      struct tm timeinfo;
      if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
      }
      time(&now);

      sensors.requestTemperatures(); 
      float temperatureC = sensors.getTempCByIndex(0);

      //TODO stałe dane przesłać do API tylko raz - po połączeniu z WiFi
    
      String json = "{\"device\":{\"id\":" + DEVICE_ID 
      + ",\"name\":\"" + myHostname 
      + "\",\"type\":\"" + ESP.getChipModel()
      + "\",\"online\":true"
      + ",\"temperature:\":" + sensors.getTempCByIndex(0)
      + "},"
      + "\"wifi\":{\"ssid\":\"" + WiFi.SSID()
      + "\",\"hostname\":\"" + myHostname
      + "\",\"ip\":\"" + WiFi_IP
      + "\",\"mac\":\"" + WiFi.macAddress()
      + "\",\"signal\":" + WiFi.RSSI()
      + "},"
      + "\"cpu\":{"
      + "\"cores\":" + ESP.getChipCores() 
      + ",\"mhz\":" + ESP.getCpuFreqMHz()
      + ",\"temperature\":" + temperatureRead() 
      + "},"
      + "\"meta\":{\"boottime\":" + Boot_Timestamp
      + ",\"timestamp\":" + now
      + "}}";
      // Serial.println(json);
      
      String topic = "ssh/devices/status/" + String(DEVICE_ID);
      int pub = mqttClient.publish(topic.c_str(), json.c_str(), true);
      if (!pub)
      {
        Serial.println("MQTT publish fail!");
        Serial.println("json size: " + String(json.length()) + " bajts");
        Serial.println("message buffor: " + String(Mqtt_Buffer_Size));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(60000));
  }
}

void setBlinds(void* parameters)
{ //nastawianie rolety o -id z parametru w xTaskCreate
  TaskParams* params = (TaskParams*)parameters;
  int id = params->param;
  int delay = 100;

  while (true)
  {
    if(!Blinds_To_Calibrate[id])
    {
      if (abs(Blinds_Set[id] - Blinds_Position[id]) > 0.005) // roleta jest na swoim miejscu jeżeli różnica jest <= 0.005% ponieważ float może przeskoczyć równą wartość int
      {
        delay = 1;
        analogWrite(Blinds_Speed_Pin[id], int((Blinds_Speed_Set[id] * 255) / 100));

        //TODO jeżeli Blind_Set = 0 lub 100 to nie obliczać pozycji a jechać do krańcówki

        //TODO przejazdy poza krańcówki zgodnie z ustawieniami

        if (Blinds_Set[id] < Blinds_Position[id])
        { //podnoszenie rolety
          if (Blinds_Sensor_Up[id] == 1)
          {
            Blinds_Position[id] = 0;
          }
          else
          {
            Blinds_Move_Up[id] = true;
            Blinds_Move_Down[id] = false;
            Blinds_Position[id] = (Blinds_Position[id] * Blinds_Runtime_Up[id] / 100 - 1) / Blinds_Runtime_Up[id] * 100;
          }
        }
        else
        {
          if (Blinds_Sensor_Down[id] == 1)
          {
            Blinds_Position[id] = 100;
          }
          else
          { //opuszczanie rolety
            Blinds_Move_Up[id] = false;
            Blinds_Move_Down[id] = true;
            Blinds_Position[id] = (Blinds_Position[id] * Blinds_Runtime_Down[id] / 100 + 1) / Blinds_Runtime_Down[id] * 100;
          }
        }
      }
      else
      {
        digitalWrite(Blinds_Speed_Pin[id], LOW);
        Blinds_Move_Up[id] = false;
        Blinds_Move_Down[id] = false;
        Blinds_Position[id] = Blinds_Set[id];
        delay = 100;
      }
    }
    else
    {
      calibrateBlind(id);
    }
    vTaskDelay(pdMS_TO_TICKS(delay));
  }
  delete params;
}

void calibrateBlind(int id)
{ // kalibracja rolety
  Serial.print("Rozpoczynanie kalibracji rolety nr ");
  Serial.println(Blinds_Id[id]);

  analogWrite(Blinds_Speed_Pin[id], int((Blinds_Speed_Set[id] * 255) / 100));

  while (Blinds_Sensor_Up[id] == 0)
  {
    Blinds_Move_Up[id] = true;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  Blinds_Move_Up[id] = false;

  int stepsDown = 0;

  while (Blinds_Sensor_Down[id] == 0)
  {
    Blinds_Move_Down[id] = true;
    vTaskDelay(pdMS_TO_TICKS(1));
    ++stepsDown;
  }
  Blinds_Move_Down[id] = false;
  
  int stepsUp = 0;

  while (Blinds_Sensor_Up[id] == 0)
  {
    Blinds_Move_Up[id] = true;
    vTaskDelay(pdMS_TO_TICKS(1));
    ++stepsUp;
  }
  digitalWrite(Blinds_Speed_Pin[id], LOW);

  Blinds_Position[id] = 0;
  Blinds_Runtime_Up[id] = stepsUp;
  Blinds_Runtime_Down[id] = stepsDown;
  
  while (true)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
    if(WiFi.status() == WL_CONNECTED)
    {
      String url = API_URL + "/blinds/" + String(Blinds_Id[id]) + "/";
      String payload = "{\"position\":0, \"runtime_up\": " + String(Blinds_Runtime_Up[id]) + ", \"runtime_down\": " + String(Blinds_Runtime_Down[id]) + "}";

      HTTPClient httpClient;
      httpClient.begin(url);
      httpClient.addHeader("Content-Type", "application/json");
      httpClient.addHeader("Authorization", "Bearer " + accessToken);
      int httpResponseCode = httpClient.PATCH(payload);

      if (httpResponseCode > 0) 
      {
        if (httpResponseCode == 200)
        {
          String response = httpClient.getString();
          // Serial.println(response);
          httpClient.end();
          Blinds_To_Calibrate[id] = false;
          // Serial.println("PATCH success");
          vTaskDelay(pdMS_TO_TICKS(200));
          break;
        }
        else
        {
          Serial.print("API Error code: ");
          Serial.println(httpResponseCode);
        }
      }
      else
      {
        Serial.print("API Error on sending PATCH: ");
        Serial.println(httpResponseCode);
      }
      httpClient.end();
    }
    else
    {
      connectWiFi();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  Serial.print("Wyniki kalibracji rolety nr ");
  Serial.print(Blinds_Id[id]);
  Serial.println(":");
  Serial.print("Przejazd w dół: ");
  Serial.println(stepsDown);
  Serial.print("Przejazd w górę: ");
  Serial.println(stepsUp);
}

void publishBlinds(void* parameters)
{ //publikowanie o zmianie położenia rolet
  int old_Blinds_Position[4] = {};

  for (int i=0; i < Blinds_Count; i++)
  {
    old_Blinds_Position[i] = Blinds_Position[i];
  }

  while (true)
  {
    if (mqttClient.connected())
    {
      for (int i=0; i < Blinds_Count; i++)
      {
        if (old_Blinds_Position[i] != int(Blinds_Position[i]))
        {
          old_Blinds_Position[i] = int(Blinds_Position[i]);
          String topic = "ssh/blinds/run/" + String(Blinds_Id[i]);
          String messageString = "{\"id\": " + String(Blinds_Id[i]) + ", \"set\": " + Blinds_Set[i] + ", \"step\": " + int(Blinds_Position[i]) + "}";
          // Serial.println(messageString);

          int pub = mqttClient.publish(topic.c_str(), messageString.c_str(), true); //wysłanie informacji o zmienie pozycji rolety
          if (!pub)
          {
            Serial.println("MQTT publish fail!");
          }
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void mcpLoop(void* parameters)
{ // funkcja ustawiająca MCP23017 zgodnie ze zmiennymi
  // utworzone w ten sposób aby tylko pojedyncze zadanie komunikowało się z MCP
  while (true)
  {
    for (int i=0; i < Blinds_Count; i++)
    {
      Blinds_Sensor_Up[i] = mcp.digitalRead(Mcp_Sensor_Up_Pin[i]);
      Blinds_Sensor_Down[i] = mcp.digitalRead(Mcp_Sensor_Down_Pin[i]);
      mcp.digitalWrite(Mcp_Up_Pin[i], Blinds_Move_Up[i]);
      mcp.digitalWrite(Mcp_Down_Pin[i], Blinds_Move_Down[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void apiUpdatePosition(void* parameters)
{ // przesyłanie aktualizacji pozycji rolet do API
  float Blinds_Api_Position[Blinds_Count] = {};

  for (int i=0; i < Blinds_Count; i++)
  {
    Blinds_Api_Position[i] = Blinds_Position[i];
  }

  while (true)
  {
    for (int i=0; i < Blinds_Count; i++)
    {
      if (Blinds_Api_Position[i] != Blinds_Position[i] and Blinds_Set[i] == Blinds_Position[i])
      {
        if(WiFi.status() == WL_CONNECTED)
        {
          String url = API_URL + "/blinds/" + String(Blinds_Id[i]) + "/";
          String payload = "{\"position\":" + String(int(Blinds_Position[i])) + "}";

          HTTPClient httpClient;
          httpClient.begin(url);
          httpClient.addHeader("Content-Type", "application/json");
          httpClient.addHeader("Authorization", "Bearer " + accessToken);
          int httpResponseCode = httpClient.PATCH(payload);

          if (httpResponseCode > 0) 
          {
            if (httpResponseCode == 200)
            {
              String response = httpClient.getString();
              // Serial.println(response);
              httpClient.end();
              Blinds_Api_Position[i] = Blinds_Position[i];
              // Serial.println("PATCH position success");
              vTaskDelay(pdMS_TO_TICKS(200));
              break;
            }
            else
            {
              Serial.print("API Error code: ");
              Serial.println(httpResponseCode);
            }
          }
          else
          {
            Serial.print("API Error on sending PATCH: ");
            Serial.println(httpResponseCode);
          }
          httpClient.end();
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}