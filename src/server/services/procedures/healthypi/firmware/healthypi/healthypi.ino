//////////////////////////////////////////////////////////////////////////////////////////
//   Arduino Library for Healthypi-v4
//   
//   Copyright (c) 2019 ProtoCentral
//   Heartrate and respiration computation based on original code from Texas Instruments
//
//   This software is licensed under the MIT License(http://opensource.org/licenses/MIT).
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,  INCLUDING BUT
//   NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR   OTHER LIABILITY,
//   WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
//   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//   Requires g4p_control graphing library for processing. 
//   Downloaded from Processing IDE Sketch->Import Library->Add Library->G4P Install
//////////////////////////////////////////////////////////////////////////////////////

// this is an extension of the healthypi v4 open source code.
// it does everything it used to do (including webserver, ble, and serial out for pi display), but additionally supports the udp interface for talking to the netsblox HealthyPi service.
// some other changes were made to the original source where appropriate (including simple cleanup).
// modified by: Devin Jean

// netsblox-capable networking based on: https://github.com/gsteinLTU/NetsbloxESP

#include <SPI.h>
#include <Wire.h>
#include <ESPmDNS.h>
#include <Update.h>

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <WebServer.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#include <FS.h>
#include "SPIFFS.h"

#include "Protocentral_ADS1292r.h"
#include "Protocentral_ecg_resp_signal_processing.h"
#include "Protocentral_AFE4490_Oximeter.h"
#include "Protocentral_MAX30205.h"
#include "Protocentral_spo2_algorithm.h"

// -----------------------------------------------------------------------------------

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

// -----------------------------------------------------------------------------------

#include "network.h"

constexpr size_t UDP_TX_PACKET_MAX_SIZE = 128;

WiFiUDP UDP;

byte MAC[6]; // MAC address for identification

// -----------------------------------------------------------------------------------

struct FileCloser 
{
  File &f;
  [[nodiscard]] FileCloser(File &file) : f(file) {}
  ~FileCloser() { f.close(); }

  FileCloser(const FileCloser&) = delete;
  FileCloser(FileCloser&&) = delete;
  
  FileCloser &operator=(const FileCloser&) = delete;
  FileCloser &operator=(FileCloser&&) = delete;
};

// -----------------------------------------------------------------------------------

constexpr u16 Heartrate_SERVICE_UUID         = 0x180D;
constexpr u16 Heartrate_CHARACTERISTIC_UUID  = 0x2A37;
constexpr u16 sp02_SERVICE_UUID              = 0x1822;
constexpr u16 sp02_CHARACTERISTIC_UUID       = 0x2A5E;
constexpr u16 DATASTREAM_SERVICE_UUID        = 0x1122;
constexpr u16 DATASTREAM_CHARACTERISTIC_UUID = 0x1424;
constexpr u16 TEMP_SERVICE_UUID              = 0x1809;
constexpr u16 TEMP_CHARACTERISTIC_UUID       = 0x2a6e;
constexpr u16 BATTERY_SERVICE_UUID           = 0x180F;
constexpr u16 BATTERY_CHARACTERISTIC_UUID    = 0x2a19;
const char *HRV_SERVICE_UUID                 = "cd5c7491-4448-7db8-ae4c-d1da8cba36d0";
const char *HRV_CHARACTERISTIC_UUID          = "01bfa86f-970f-8d96-d44d-9023c47faddc";
const char *HIST_CHARACTERISTIC_UUID         = "01bf1525-970f-8d96-d44d-9023c47faddc";

#define BLE_MODE                0X01
#define WEBSERVER_MODE          0X02
#define V3_MODE                 0X03
#define CES_CMDIF_PKT_START_1   0x0A
#define CES_CMDIF_PKT_START_2   0xFA
#define CES_CMDIF_DATA_LEN_LSB   20
#define CES_CMDIF_DATA_LEN_MSB    0
#define CES_CMDIF_TYPE_DATA     0x02
#define CES_CMDIF_PKT_STOP_1    0x00
#define CES_CMDIF_PKT_STOP_2    0x0B
#define PUSH_BUTTON              17
#define SLIDE_SWITCH             16
#define MAX30205_READ_INTERVAL 10000
#define LINELEN                 34 
#define HISTGRM_DATA_SIZE      12*4
#define HISTGRM_CALC_TH         10
constexpr int MAX = 20;

unsigned int array[MAX];

int rear = -1;
int sqsum;
int hist[] = {0};
int k=0;
int count = 0;
int min_f=0;
int max_f=0;
int max_t=0;
int min_t=0;
int index_cnt = 0;
int pass_size; 
int data_count;
int ssid_size;
int status_size;
int temperature;
int number_of_samples = 0;
int battery=0;
int bat_count=0;
int bt_rem = 0;
int wifi_count;
int flag=0;

float sdnn;
float sdnn_f;
float rmssd;
float mean_f;
float rmssd_f;
float per_pnn;
float pnn_f=0;
float tri =0;
float temp;

void HealthyPiV4_Webserver_Init();
void send_data_serial_port(void);

volatile u8 global_HeartRate = 0;
volatile u8 global_HeartRate_prev = 0;
volatile u8 global_RespirationRate=0;
volatile u8 global_RespirationRate_prev = 0;
volatile u8 npeakflag = 0;
volatile long time_count=0;
volatile long hist_time_count=0;
volatile bool histgrm_ready_flag = false;
volatile unsigned int RR;

u8 ecg_data_buff[20];
u8 resp_data_buff[2];
u8 ppg_data_buff[20];
u8 Healthypi_Mode = WEBSERVER_MODE;
u8 lead_flag = 0x04;
u8 data_len = 20;
u8 heartbeat,sp02,respirationrate;
u8 histgrm_percent_bin[HISTGRM_DATA_SIZE/4];
u8 hr_percent_count = 0;
u8 hrv_array[20];

u16 ecg_stream_cnt = 0;
u16 resp_stream_cnt = 0;
u16 ppg_stream_cnt = 0;
u16 ppg_wave_ir;

i16 ecg_filterout;
i16 resp_filterout;

u32 hr_histgrm[HISTGRM_DATA_SIZE];

bool deviceConnected = false;
bool oldDeviceConnected = false;
bool temp_data_ready = false;
bool spo2_calc_done = false;
bool ecg_buf_ready = false;
bool resp_buf_ready = false;
bool ppg_buf_ready = false;
bool hrv_ready_flag = false;
bool mode_write_flag = false;
bool slide_switch_flag = false;
bool processing_intrpt = false;
bool credential_success_flag = false;
bool STA_mode_indication = false;
bool bat_data_ready = false;
bool leadoff_detected = true;
bool startup_flag = true;

char DataPacket[30];
char ssid[32];
char password[64];
char modestatus[32];
char tmp_ecgbuf[1200];

String ssid_to_connect;
String password_to_connect;
String tmp_ecgbu;
String strValue = "";

static int bat_prev=100;
static u8 bat_percent = 100;

constexpr int ADS1292_DRDY_PIN = 26;
constexpr int ADS1292_CS_PIN = 13;
constexpr int ADS1292_START_PIN = 14;
constexpr int ADS1292_PWDN_PIN = 27;
constexpr int AFE4490_CS_PIN = 21; 
constexpr int AFE4490_DRDY_PIN = 39; 
constexpr int AFE4490_PWDN_PIN = 4; 
constexpr int freq = 5000;
constexpr int ledChannel = 0;
constexpr int resolution = 8;

const char *const host = "Healthypi_v4";
const char *const host_password = "Open@1234";
const char DataPacketHeader[] = {CES_CMDIF_PKT_START_1, CES_CMDIF_PKT_START_2, CES_CMDIF_DATA_LEN_LSB, CES_CMDIF_DATA_LEN_MSB, CES_CMDIF_TYPE_DATA};
const char DataPacketFooter[] = {CES_CMDIF_PKT_STOP_1, CES_CMDIF_PKT_STOP_2};

BLEServer* pServer = NULL;
BLECharacteristic* Heartrate_Characteristic = NULL;
BLECharacteristic* sp02_Characteristic = NULL;
BLECharacteristic* datastream_Characteristic = NULL;
BLECharacteristic* battery_Characteristic = NULL;
BLECharacteristic* temperature_Characteristic = NULL;
BLECharacteristic* hist_Characteristic = NULL;
BLECharacteristic* hrv_Characteristic = NULL;

ads1292r ADS1292R;   // define class ads1292r
ads1292r_processing ECG_RESPIRATION_ALGORITHM; // define class ecg_algorithm
AFE4490 afe4490;
MAX30205 tempSensor;
spo2_algorithm spo2;
ads1292r_data ads1292r_raw_data;
afe44xx_data afe44xx_raw_data;

class MyServerCallbacks: public BLEServerCallbacks 
{
  void onConnect(BLEServer* pServer) override
  {
    deviceConnected = true;
    Serial.println("connected");
  }

  void onDisconnect(BLEServer* pServer) override
  {
    deviceConnected = false;
  }
};

class MyCallbackHandler: public BLECharacteristicCallbacks 
{
  void onWrite(BLECharacteristic *datastream_Characteristic) override
  {
    std::string value = datastream_Characteristic->getValue();
    int len = value.length();
    strValue = "0";

    if (value.length() > 0) 
    {
      Serial.print("New value: ");

      for (int i = 0; i < value.length(); i++)
      {
        Serial.print(String(value[i]));
        strValue += value[i];
      }

      Serial.println();
    }
  }
};

WebServer server(80);

// Sends a Netsblox formatted message
void netsblox_send(const char *msg, int len)
{
  u32 time = millis();
  UDP.beginPacket(SERVER_IP, SERVER_PORT);
  UDP.write(MAC, sizeof(MAC));
  UDP.write(reinterpret_cast<const u8*>(&time), 4);
  UDP.write(reinterpret_cast<const u8*>(msg), len);
  UDP.endPacket();
}

const char *wifi_status_str(int stat)
{
  switch (stat)
  {
    case WL_CONNECTED: return "connected";
    case WL_NO_SHIELD: return "no shield";
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "no ssid available";
    case WL_SCAN_COMPLETED: return "scan completed";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "unknown";
  }
}

void wifi_connect()
{
  if (*NET_PASSWD) WiFi.begin(NET_SSID, NET_PASSWD);
  else WiFi.begin(NET_SSID);
}

void delLine(fs::FS &fs, const char *path, u32 line, const int char_to_delete)
{ 
  File file = fs.open(path, FILE_WRITE);
  FileCloser _{file};
  
  if (!file)
  {
    Serial.println("- failed to open file for writing");
    return;
  }
  
  file.seek((line - 1) * LINELEN);  
  char ch[35]; 

  // build the 'delete line'
  for (u8 i = 0; i < char_to_delete; i++) ch[i] = ' ';

  file.print(ch); // all marked as deleted! yea!
}

void deleteFile(fs::FS &fs, const char *path)
{
  Serial.printf("Deleting file: %s\r\n", path);
  
  if (fs.remove(path)) Serial.println("- file deleted");
  else Serial.println("- delete failed");
}

bool readFile(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\r\n", path);
  u8 rd_config = 0;
  
  File file = fs.open(path, FILE_READ);
  FileCloser _{file};
  
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");
    return false;
  }

  Serial.println("- read from file:");
  rd_config = file.read();
  Serial.println(rd_config);
 
  if (rd_config == 0x0f)
  {
    Healthypi_Mode = WEBSERVER_MODE;
    delLine(SPIFFS, "/v4_mode.txt", 1, 5);
  }
  else return false;

  return true;
}

bool fileread(fs::FS &fs, const char *path)
{
  Serial.printf("Reading file: %s\r\n", path);
  u8 md_config = 0;
  
  File file = fs.open(path, FILE_READ);
  FileCloser _{file};
  
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");
    return false;
  }

  Serial.println("- read from file:");
  md_config = file.read();
  Serial.println(md_config);
 
  if (md_config == 0x0a)
  {
    Healthypi_Mode = WEBSERVER_MODE;
    delLine(SPIFFS,"/web_mode.txt",1,5);
  }
  else if (md_config == 0x0b)
  {
    Healthypi_Mode = WEBSERVER_MODE;
    delLine(SPIFFS,"/web_mode.txt",1,5);
  }
  else if (md_config == 0x0c)
  {
    Healthypi_Mode = WEBSERVER_MODE;
    delLine(SPIFFS,"/web_mode.txt",1,5);
  }
  else return false;

  return true;
}

void writeFile(fs::FS &fs, const char *path, const char *message)
{
  Serial.printf("Writing file: %s\r\n", path);
  
  File file = fs.open(path, FILE_WRITE);
  FileCloser _{file};

  if (!file)
  {
    Serial.println("- failed to open file for writing");
    return;
  }
  
  if (file.print(message)) Serial.println("- file written"); 
  else Serial.println("- write failed");
}

void readFile(fs::FS &fs, const char *path, int *data_count, char *file_data)
{
  Serial.printf("Reading file: %s\r\n", path);
  
  File file = fs.open(path, FILE_READ);
  FileCloser _{file};
  
  if (!file || file.isDirectory())
  {
    Serial.println("- failed to open file for reading");    
  }

  Serial.println("- read from file:"); 
  for (int i = 0; file.available(); )
  {
    file_data[i++] = file.read();
    Serial.write(file_data[i-1]);
  }
  *data_count = file.size();
}

void HealthyPiV4_BLE_Init()
{
  BLEDevice::init("Healthypi v4"); // Create the BLE Device
  pServer = BLEDevice::createServer(); // Create the BLE Server
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *HeartrateService = pServer->createService(Heartrate_SERVICE_UUID); // Create the BLE Service
  BLEService *sp02Service = pServer->createService(sp02_SERVICE_UUID); // Create the BLE Service
  BLEService *TemperatureService = pServer->createService(TEMP_SERVICE_UUID);
  BLEService *batteryService = pServer->createService(BATTERY_SERVICE_UUID);
  BLEService *hrvService = pServer->createService(HRV_SERVICE_UUID);
  BLEService *datastreamService = pServer->createService(DATASTREAM_SERVICE_UUID);

  Heartrate_Characteristic = HeartrateService->createCharacteristic(
                              Heartrate_CHARACTERISTIC_UUID,
                              BLECharacteristic::PROPERTY_READ   |
                              BLECharacteristic::PROPERTY_WRITE  |
                              BLECharacteristic::PROPERTY_NOTIFY
                              );

  sp02_Characteristic = sp02Service->createCharacteristic(
                        sp02_CHARACTERISTIC_UUID,
                        BLECharacteristic::PROPERTY_READ   |
                        BLECharacteristic::PROPERTY_WRITE  |
                        BLECharacteristic::PROPERTY_NOTIFY
                        );

  temperature_Characteristic = TemperatureService->createCharacteristic(
                                TEMP_CHARACTERISTIC_UUID,
                                BLECharacteristic::PROPERTY_READ   |
                                BLECharacteristic::PROPERTY_WRITE  |
                                BLECharacteristic::PROPERTY_NOTIFY
                                );
                                                                      
  battery_Characteristic = batteryService->createCharacteristic(
                            BATTERY_CHARACTERISTIC_UUID,
                            BLECharacteristic::PROPERTY_READ   |
                            BLECharacteristic::PROPERTY_WRITE  |
                            BLECharacteristic::PROPERTY_NOTIFY
                            );

  hrv_Characteristic = hrvService->createCharacteristic(
                        HRV_CHARACTERISTIC_UUID,
                        BLECharacteristic::PROPERTY_READ   |
                        BLECharacteristic::PROPERTY_WRITE  |
                        BLECharacteristic::PROPERTY_NOTIFY
                        );

  hist_Characteristic = hrvService->createCharacteristic(
                          HIST_CHARACTERISTIC_UUID,
                          BLECharacteristic::PROPERTY_READ   |
                          BLECharacteristic::PROPERTY_WRITE  |
                          BLECharacteristic::PROPERTY_NOTIFY
                          );
 
  datastream_Characteristic = datastreamService->createCharacteristic(
                              DATASTREAM_CHARACTERISTIC_UUID,
                              BLECharacteristic::PROPERTY_READ   |
                              BLECharacteristic::PROPERTY_WRITE  |
                              BLECharacteristic::PROPERTY_NOTIFY 
                              );
                
  Heartrate_Characteristic->addDescriptor(new BLE2902());
  sp02_Characteristic->addDescriptor(new BLE2902());
  temperature_Characteristic->addDescriptor(new BLE2902());
  battery_Characteristic->addDescriptor(new BLE2902());
  hist_Characteristic->addDescriptor(new BLE2902());
  hrv_Characteristic->addDescriptor(new BLE2902());
  datastream_Characteristic->addDescriptor(new BLE2902());
  datastream_Characteristic->setCallbacks(new MyCallbackHandler()); 

  // Start the service
  HeartrateService->start();
  sp02Service->start();
  TemperatureService->start();
  batteryService->start();
  hrvService->start();
  datastreamService->start();

  // Start advertising
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(Heartrate_SERVICE_UUID);
  pAdvertising->addServiceUUID(sp02_SERVICE_UUID);
  pAdvertising->addServiceUUID(TEMP_SERVICE_UUID);
  pAdvertising->addServiceUUID(BATTERY_SERVICE_UUID);
  pAdvertising->addServiceUUID(HRV_SERVICE_UUID);
  pAdvertising->addServiceUUID(DATASTREAM_SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinPreferred(0x00);  // set value to 0x00 to not advertise this parameter
  BLEDevice::startAdvertising();
  ble_advertising(); 
  Serial.println("Waiting a client connection to notify...");
}

void read_battery_value()
{
  static int adc_val = analogRead(A0);
  battery += adc_val;
  
  if (bat_count == 9)
  {         
    battery = (battery/10);
    battery=((battery*2)-400);
    
    if (battery > 4100)
    {
      battery = 4100;
    }
    else if (battery < 3600 )
    {
      battery = 3600;
    }

    if (startup_flag == true)
    {
      bat_prev = battery;
      startup_flag = false;
    }

    bt_rem = (battery % 100); 

    if (bt_rem>80 && bt_rem < 99 && (bat_prev != 0))
    {
      battery = bat_prev;
    }

    if ((battery/100)>=41)
    {
      battery = 100;
    }
    else if ((battery/100)==40)
    {
      battery = 80;
    }
    else if ((battery/100)==39)
    {
      battery = 60;
    }
    else if ((battery/100)==38)
    {
      battery=45;
    }
    else if ((battery/100)==37)
    {
      battery=30;
    }
    else if ((battery/100)<=36)
    {
      battery = 20;
    }

    bat_percent = (u8) battery;
    bat_count=0;
    battery=0;
    bat_data_ready = true;
  }
  else
  {
    bat_count++;
  }
}
 
void add_hr_histgrm(u8 hr)
{
  u8 index = hr/10;
  hr_histgrm[index-4]++;
  u32 sum = 0;

  if (hr_percent_count++ > HISTGRM_CALC_TH)
  {
    hr_percent_count = 0;
    
    for (int i = 0; i < HISTGRM_DATA_SIZE; i++)
    {
      sum += hr_histgrm[i];
    }

    if (sum != 0)
    {
      for (int i = 0; i < HISTGRM_DATA_SIZE/4; i++)
      {
        u32 percent = ((hr_histgrm[i] * 100) / sum);
        histgrm_percent_bin[i] = percent;
      }
    }
    
    histgrm_ready_flag = true;
  }
}

u8* read_send_data(u8 peakvalue, u8 respirationrate)
{  
  int meanval;
  u16 sdnn;
  u16 pnn;
  u16 rmsd;
  RR = peakvalue;
  k++;

  if (rear == MAX-1)
  {
    for (int i = 0; i < MAX - 1; i++) array[i] = array[i+1];
    array[MAX-1] = RR;   
  }
  else array[++rear] = RR;

  if (k >= MAX)
  { 
    max_f = HRVMAX(array);
    min_f = HRVMIN(array);
    mean_f = mean(array);
    sdnn_f = sdnn_ff(array);
    pnn_f = pnn_ff(array);
    rmssd_f=rmssd_ff(array);
  
    meanval = mean_f * 100;
    sdnn= sdnn_f * 100;
    pnn= pnn_f * 100;
    rmsd=rmssd_f * 100;

    hrv_array[0]= meanval;
    hrv_array[1]= meanval >> 8;
    hrv_array[2]= meanval >> 16;
    hrv_array[3]= meanval >> 24;
    hrv_array[4]= sdnn;
    hrv_array[5]= sdnn >> 8;
    hrv_array[6]= pnn;
    hrv_array[7]= pnn >> 8;
    hrv_array[10] = rmsd;
    hrv_array[11] = rmsd >> 8;
    hrv_array[12] = respirationrate;
    hrv_ready_flag = true;
  }
}

int HRVMAX(unsigned int array[])
{  
  for (int i = 0; i < MAX; i++)
    if (array[i] > max_t) max_t = array[i];

  return max_t;
}

int HRVMIN(unsigned int array[])
{   
  min_t = max_f;

  for (int i = 0; i < MAX; i++)
    if (array[i] < min_t) min_t = array[i]; 

  return min_t;
}

float mean(unsigned int array[])
{ 
  int sum = 0;
  for (int i = 0; i < MAX; i++) sum += array[i];
   
  return (float)sum / MAX;
} 

float sdnn_ff(unsigned int array[])
{
  int sumsdnn = 0;
  int diff;
 
  for (int i = 0; i < MAX; i++)
  {
    diff = (array[i] - mean_f);
    diff = diff * diff;
    sumsdnn = sumsdnn + diff;   
  }

  sdnn = sqrt(sumsdnn / MAX);
  return   sdnn;
}

float pnn_ff(unsigned int array[])
{ 
  unsigned int pnn50[MAX];
  count = 0;
  sqsum = 0;

  for (int i = 0; i < MAX - 2; i++)
  {
    pnn50[i] = abs(array[i+1] - array[i]);
    sqsum = sqsum + (pnn50[i] * pnn50[i]);

    if (pnn50[i] > 50) ++count;    
  }
  per_pnn = ((float)count / MAX) * 100;
  return per_pnn;
}

float rmssd_ff(unsigned int array[])
{
  unsigned int pnn50[MAX];
  sqsum = 0;

  for (int i = 0; i < MAX - 2; i++)
  {
    pnn50[i] = abs(array[i + 1] - array[i]);
    sqsum = sqsum + (pnn50[i] * pnn50[i]);
  }

  rmssd = sqrt(sqsum / (MAX - 1));
  return rmssd;
}

void ble_advertising()
{
  while (!deviceConnected && !slide_switch_flag && !mode_write_flag)
  {
    digitalWrite(A13, LOW);
    delay(100);
    digitalWrite(A13, HIGH);
    delay(3000);
  }
}

void V3_mode_indication()
{
  digitalWrite(A13, HIGH);

  for (int dutyCycle = 0; dutyCycle <= 254; dutyCycle += 3)
  {   
    ledcWrite(ledChannel, dutyCycle);
    delay(25);
  }
  for (int dutyCycle = 254; dutyCycle >= 0; dutyCycle -= 3)
  {
    ledcWrite(ledChannel, dutyCycle);   
    delay(25);
  }
}

void restart_indication()
{
  digitalWrite(A13, LOW);
  delay(2500);
  digitalWrite(A13, HIGH);
  delay(2500);
}

void webserver_mode_indication()
{
  for (int dutyCycle = 254; dutyCycle >= 0; dutyCycle -= 3)
  {
    ledcWrite(ledChannel, dutyCycle);   
    delay(25);
  }
  for (int dutyCycle = 0; dutyCycle <= 254; dutyCycle += 3)
  {   
    ledcWrite(ledChannel, dutyCycle);
    delay(25);
  }
}

void soft_AP_mode_indication()
{
  for (int i = 0; i <= 5; i++)
  {
    digitalWrite(A15, HIGH);
    delay(50);
    digitalWrite(A15, LOW);
    delay(2000); 
  }
}

void OTA_update_indication()
{
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(A13, HIGH);
    digitalWrite(A15, HIGH);
    delay(25);
    digitalWrite(A15, LOW);
    digitalWrite(A13, LOW);
    delay(25);
    digitalWrite(A13, HIGH);
  }
}

void send_data_serial_port()
{
  for (int i = 0; i < 5; i++) Serial.write(DataPacketHeader[i]);
  for (int i = 0; i < 20; i++) Serial.write(DataPacket[i]);
  for (int i = 0; i < 2; i++) Serial.write(DataPacketFooter[i]);
}

void handle_ble_stack()
{
  if (strValue == "\0")
  {
    if (ecg_buf_ready)
    {
      ecg_buf_ready = false;
      datastream_Characteristic->setValue(ecg_data_buff, 18);    
      datastream_Characteristic->notify();
    }
  }
  else if (strValue =="0spo2")
  {
    if (ppg_buf_ready)
    {
      ppg_buf_ready = false;
      datastream_Characteristic->setValue(ppg_data_buff, 18);    
      datastream_Characteristic->notify();
    }
  }
 
  //send notifications if connected to a client
  if (global_HeartRate_prev != global_HeartRate)
  {
    global_HeartRate_prev = global_HeartRate;
    u8 hr_att_ble[2];
    hr_att_ble[0] = lead_flag;
    hr_att_ble[1] = (u8)global_HeartRate;    
    Heartrate_Characteristic->setValue(hr_att_ble, 2);
    Heartrate_Characteristic->notify(); 
  }  
    
  if (spo2_calc_done)
  {
    // afe44xx_raw_data.buffer_count_overflow = false;
    u8 spo2_att_ble[5];
    spo2_att_ble[0] = 0x00;
    spo2_att_ble[1] = (u8)sp02;
    spo2_att_ble[2] = (u8)(sp02>>8);
    spo2_att_ble[3] = 0;
    spo2_att_ble[4] = 0;
    sp02_Characteristic->setValue(spo2_att_ble, 5);     
    sp02_Characteristic->notify();        
    spo2_calc_done = false;
  }

  if (hrv_ready_flag)
  {
    hrv_Characteristic->setValue(hrv_array, 13);
    hrv_Characteristic->notify(); 
    hrv_ready_flag = false;
  }
    
  if (temp_data_ready)
  {
    temperature_Characteristic->setValue((u8 *)&temperature, 2);
    temperature_Characteristic->notify();
    temp_data_ready = false;
  }
  
  if (histgrm_ready_flag )
  {
    histgrm_ready_flag = false;
    hist_Characteristic->setValue(histgrm_percent_bin, 13);
    hist_Characteristic->notify();
  }
 
  if (bat_data_ready)
  {
    battery_Characteristic->setValue((u8 *)&bat_percent, 1);
    battery_Characteristic->notify();
    bat_data_ready = false;
  }

  if (!deviceConnected && oldDeviceConnected)
  {
    delay(500); // give the bluetooth stack the chance to get things ready
    pServer->startAdvertising(); // restart advertising
    Serial.println("start advertising");
    ble_advertising();
    oldDeviceConnected = deviceConnected;
  }
  
  // connecting
  if (deviceConnected && !oldDeviceConnected)
  {
    // do stuff here on connecting
    oldDeviceConnected = deviceConnected;
  } 
}

bool loadFromSpiffs(String path)
{
  String dataType = "text/plain";
  if (path.endsWith("/")) path += "index.htm";
  if (path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
  else if (path.endsWith(".html")) dataType = "text/html";
  else if (path.endsWith(".htm")) dataType = "text/html";
  else if (path.endsWith(".css")) dataType = "text/css";
  else if (path.endsWith(".js")) dataType = "application/javascript";
  else if (path.endsWith(".png")) dataType = "image/png";
  else if (path.endsWith(".gif")) dataType = "image/gif";
  else if (path.endsWith(".jpg")) dataType = "image/jpeg";
  else if (path.endsWith(".ico")) dataType = "image/x-icon";
  else if (path.endsWith(".xml")) dataType = "text/xml";
  else if (path.endsWith(".pdf")) dataType = "application/pdf";
  else if (path.endsWith(".zip")) dataType = "application/zip";
  
  File dataFile = SPIFFS.open(path.c_str(), "r");
  FileCloser _{dataFile};
 
  if (server.hasArg("download")) dataType = "application/octet-stream";
  
  server.streamFile(dataFile, dataType);
 
  return true;
}

void HealthyPiV4_Webserver_Init()
{ 
  if ((!SPIFFS.exists("/mode_status.txt")))
  {
    if (!SPIFFS.exists("/mode_status.txt"))
    {
      Serial.print("Creating "); 
      Serial.println("/mode_status.txt");
      
      SPIFFS.open("/mode_status.txt", "w").close();
    }
    
    Serial.println("Enter Soft AP Mode");
    soft_AP_mode_indication();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(host);
    IPAddress myIP = WiFi.softAPIP();
    Serial.println(myIP);
    delay(1000); 
  }
  else
  {
    Serial.println("Required files does exist");
    readFile(SPIFFS,"/mode_status.txt",&status_size,modestatus); 
    Serial.println();
    readFile(SPIFFS,"/ssid_list.txt",&ssid_size,ssid); 
    Serial.println();
    readFile(SPIFFS,"/pass_list.txt",&pass_size,password);   
    Serial.println();
  
    if ((status_size == 0))
    {
      Serial.println("Enter Soft AP Mode");
      WiFi.mode(WIFI_AP);
      WiFi.softAP(host);
      IPAddress myIP = WiFi.softAPIP();
      Serial.println(myIP);
      soft_AP_mode_indication();
      delay(1000);
    }
    else
    {
      Serial.printf("Connecting to the network ");
      if (WiFi.status() != WL_CONNECTED)
      {
        for (byte k = 0; k < 32; k++) Serial.print(ssid[k]); 
  
        Serial.printf("\nWi-Fi mode set to WIFI_STA %s\n", WiFi.mode(WIFI_STA) ? "Success" : "Failed!"); 
        WiFi.begin(ssid, password);
        
        while (WiFi.status() != WL_CONNECTED)
        {
          delay(1000);
          Serial.print(".");
          wifi_count++;
  
          if (wifi_count == 61)
          {
            wifi_count = 0;
            Serial.println(" ");
            Serial.println("connection failed");
            deleteFile(SPIFFS, "/mode_status.txt");
            delay(1000);
            const char w = 0x0c;
            writeFile(SPIFFS, "/web_mode.txt", &w);
            restart_indication();
            ESP.restart();  
          }
        }
      }
      
      Serial.println("");
      Serial.println("WiFi connected.");
      Serial.println("IP address: ");
      Serial.println(WiFi.localIP());
      STA_mode_indication = true;
    }
  }
 
  if (MDNS.begin("HealthyPi")) Serial.println("MDNS responder started");
  
  server.on("/", []() 
  {
    server.sendHeader("Location", "/main.html",true); // redirect to our html web page
    server.send(303, "text/plane","");
  });
  server.on("/readheartrate", []() 
  {
    int ln = tmp_ecgbu.length();
    server.setContentLength(ln);
    index_cnt = 0;
    number_of_samples = 0;
    server.send(200, "text/html", tmp_ecgbu);
  });
  server.on("/readtemperature", []() 
  {
    char sensor_data[20]; 
    sprintf(sensor_data, "%d,%d,%d,%d", global_HeartRate, global_RespirationRate, afe44xx_raw_data.spo2, temperature);
    server.send(200, "text/html", sensor_data); //Send ADC value only to client ajax request
  });
  server.on("/root", HTTP_GET, []() 
  {    
    server.sendHeader ("Location","/set_network_credentials.html",true);
    server.send(303,"text/plane","");
  });
  server.on("/data", HTTP_POST, []()
  {
    if (server.hasArg("SSID") && server.hasArg("Password") && (server.arg("Password").length()>7))
    { 
      ssid_to_connect = server.arg("SSID");
      password_to_connect = server.arg("Password");
      server.send(200, "text/html", "<html><body><h3>Successfully Changed the Network Credentials. It is restarting.<br></h3><h3>After restart connect to the changed network. In case if it is not connected in 60sec it will go to the softAp mode.<br><br>Enter healthypi.local/ in the address bar. If it is not connecting, use the IP address of the connected network which is printed in the serial moniter<br><br></h3></body></html>");
      Serial.print("SSID: ");
      Serial.println(ssid_to_connect);
      Serial.print("Password: ");
      Serial.println(password_to_connect);
      credential_success_flag = true;
    } 
    else 
    {
      server.send(400, "text/html", "<html><body><h1>HTTP Error 400</h1><p>Bad request. Please enter a value.</p></body></html>");
      credential_success_flag = false;
    }
  });
  server.onNotFound([]()
  {
    if (loadFromSpiffs(server.uri())) return;
  
    String message = "File Not Detected\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += server.method() == HTTP_GET ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
  
    for (u8 i = 0; i < server.args(); i++)
    {
      message += " NAME:"+server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
    }
  
    server.send(404, "text/plain", message);
    Serial.println(message);
  });
 
  server.on("/network_change", HTTP_GET, []()
  {
    server.sendHeader("Connection", "close");
    server.sendHeader("Location", "/set_network_credentials.html", true); // redirect to our html web page
    server.send(303, "text/plane", "");
  });
  
  server.on("/OTA_login", HTTP_GET, []()
  {
    detachInterrupt(ADS1292_DRDY_PIN);
    server.sendHeader("Connection", "close");
    server.sendHeader("Location", "/ota_login.html", true); // redirect to our html web page
    server.send(303, "text/plane", "");
  });

  server.on("/serverIndex", HTTP_GET, []()
  {
    server.sendHeader("Connection", "close");
    server.sendHeader("Location", "/ota_upload.html", true); // redirect to our html web page
    server.send(303, "text/plane", "");
  });
  
  server.on("/update", HTTP_POST, []()
  {
    OTA_update_indication();
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
    restart_indication();
    ESP.restart();
  }, []()
  {
    //OTA_update_indication();
    HTTPUpload& upload = server.upload();
   
    if (upload.status == UPLOAD_FILE_START)
    {
      Serial.printf("Update: %s\n", upload.filename.c_str());
    
      if (!Update.begin(UPDATE_SIZE_UNKNOWN))
      { //start with max available size
        Update.printError(Serial);
        Serial.println("unknown");
      }
    } 
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
      //flashing firmware to ESP
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      {
        Update.printError(Serial);
        Serial.println("size mismatch");
      }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
      //true to set the size to the current progress
      if (Update.end(true)) Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      else
      {
        Update.printError(Serial);
        Serial.println("fail");
      }
    }
    else Serial.println("Aborted");

  });
  
  server.begin();
  Serial.println("HTTP server started");
}

void handleNetsbloxUDP()
{
  int packet_size = UDP.parsePacket();
  if (packet_size)
  {
    char buf[UDP_TX_PACKET_MAX_SIZE];
    UDP.read(buf, sizeof(buf));

    // get current vitals
    if (buf[0] == 'V') {
      buf[1] = (char)(u8)global_HeartRate;
      buf[2] = (char)(u8)global_RespirationRate;
      buf[3] = (char)(u8)afe44xx_raw_data.spo2;
      const auto temp = (u16)tempSensor.getTemperature();
      memcpy(buf + 4, &temp, 2);
      packet_size = 6;
    }

    // send response
    netsblox_send(buf, packet_size);
  }
}

void netsbloxAnnounce() {
  constexpr decltype(millis()) ANNOUNCE_RATE = 1000; // rate of announcements (ms)
  static decltype(millis()) next_announce = 0; // the time of the next announcement to make

  // if it's time for the next announcement, do it and update next timepoint
  const auto current = millis();
  if (current >= next_announce) {
    netsblox_send("I", 1);
    next_announce = current + ANNOUNCE_RATE;
  }
}

void setup()
{
  pinMode(ADS1292_DRDY_PIN, INPUT);  
  pinMode(ADS1292_CS_PIN, OUTPUT);    
  pinMode(ADS1292_START_PIN, OUTPUT);
  pinMode(ADS1292_PWDN_PIN, OUTPUT);  
  pinMode(A15, OUTPUT);
  pinMode(A13, OUTPUT);
  pinMode(AFE4490_PWDN_PIN, OUTPUT);
  pinMode(AFE4490_CS_PIN, OUTPUT);  // Slave Select
  pinMode(AFE4490_DRDY_PIN, INPUT); // data ready 
  pinMode(SLIDE_SWITCH, OUTPUT);    // set up mode selection pins
  pinMode(PUSH_BUTTON, INPUT);      // set up mode selection pins

  Serial.begin(115200);
  Serial.println("Setting up Healthy pI V4...");

  for (;;)
  {
    wifi_connect();
    if (WiFi.status() == WL_CONNECTED) break;
    Serial.printf("wifi status: %s -- strength: %d\n", wifi_status_str(WiFi.status()), WiFi.RSSI());
    delay(1000);
  }

  Serial.printf("wifi connected -- local ip: %d\n", WiFi.localIP());
  Serial.printf("starting UDP server on port %d\n", LISTEN_PORT);
  UDP.begin(LISTEN_PORT);
  WiFi.macAddress(MAC); // read the mac address (needed for netsblox communication)
  
  int buttonState = digitalRead(SLIDE_SWITCH);
  Serial.printf("slide switch state: %d\n", buttonState);

  if (!SPIFFS.begin())
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  else
  {
    Serial.println("SPIFFS initialization completed");
  }

  if (!readFile(SPIFFS, "/v4_mode.txt") && !fileread(SPIFFS, "/web_mode.txt"))
  {
    delLine(SPIFFS,"/web_mode.txt",1,5);

    if (buttonState)
    {
      restart_indication();
      Healthypi_Mode = V3_MODE;
    }
    else
    {
      restart_indication();
      Healthypi_Mode = BLE_MODE;
    }
  }
  
  pinMode(PUSH_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PUSH_BUTTON), []()
  {
    if (Healthypi_Mode != WEBSERVER_MODE)
    {
      detachInterrupt(ADS1292_DRDY_PIN);
      mode_write_flag = true;
    }
  }, FALLING);
  pinMode(SLIDE_SWITCH, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SLIDE_SWITCH), []()
  { 
    if (!processing_intrpt)
    {
      processing_intrpt = true;
      detachInterrupt(ADS1292_DRDY_PIN);
      slide_switch_flag = true;
    }
  }, CHANGE);
  
  if (Healthypi_Mode == WEBSERVER_MODE)
  {
    ledcSetup(ledChannel, freq, resolution);
    ledcAttachPin(A13, ledChannel);
    Serial.println("starts in webserver mode");
    webserver_mode_indication();
    ledcDetachPin(A13);
    HealthyPiV4_Webserver_Init();
  }
  else if (Healthypi_Mode == V3_MODE)
  { 
    ledcSetup(ledChannel, freq, resolution);
    ledcAttachPin(A15, ledChannel);  
    V3_mode_indication();
    Serial.println("starts in v3 mode");
    ledcDetachPin(A15);
  }
  else
  {
    Serial.println("starts in ble mode");
    HealthyPiV4_BLE_Init();
  }

  SPI.begin();
  Wire.begin(25, 22);
  SPI.setClockDivider(SPI_CLOCK_DIV16);
  SPI.setBitOrder(MSBFIRST);
  SPI.setDataMode(SPI_MODE0);
  delay(10);
  afe4490.afe44xxInit (AFE4490_CS_PIN, AFE4490_PWDN_PIN);
  delay(10); 
  SPI.setDataMode(SPI_MODE1); // set SPI mode as 1
  delay(10);
  ADS1292R.ads1292_Init(ADS1292_CS_PIN, ADS1292_PWDN_PIN, ADS1292_START_PIN); // initalize ADS1292 slave
  delay(10); 
  attachInterrupt(digitalPinToInterrupt(ADS1292_DRDY_PIN), ads1292r_interrupt_handler, FALLING); // Digital2 is attached to Data ready pin of AFE is interrupt0 in ARduino
  tempSensor.begin();
  
  Serial.println("initialization complete");
}

void loop()
{
  bool ret = ADS1292R.getAds1292r_Data_if_Available(ADS1292_DRDY_PIN, ADS1292_CS_PIN, &ads1292r_raw_data);

  if (ret == true)
  {  
    auto ecg_wave_sample = (i16)(ads1292r_raw_data.raw_ecg >> 8); // ignore the lower 8 bits out of 24bits 
    auto res_wave_sample = (i16)(ads1292r_raw_data.raw_resp >> 8);
  
    if (ads1292r_raw_data.status_reg & 0x1f)
    {
      leadoff_detected = true; 
      lead_flag = 0x04;
      ecg_filterout = 0;
      resp_filterout = 0;      
      DataPacket[14] = 0;
      DataPacket[16] = 0;
    }  
    else
    {
      leadoff_detected = false;
      lead_flag = 0x06;
      ECG_RESPIRATION_ALGORITHM.Filter_CurrentECG_sample(&ecg_wave_sample, &ecg_filterout); // filter out the line noise @40Hz cutoff 161 order
      ECG_RESPIRATION_ALGORITHM.Calculate_HeartRate(ecg_filterout, &global_HeartRate, &npeakflag); // calculate
      ECG_RESPIRATION_ALGORITHM.Filter_CurrentRESP_sample(res_wave_sample, &resp_filterout);
      ECG_RESPIRATION_ALGORITHM.Calculate_RespRate(resp_filterout, &global_RespirationRate);   
   
      if (npeakflag == 1)
      {
        read_send_data(global_HeartRate,global_RespirationRate);
        add_hr_histgrm(global_HeartRate);
        npeakflag = 0;
      }
   
      if (Healthypi_Mode == BLE_MODE)
      {
        ecg_data_buff[ecg_stream_cnt++] = (u8)ecg_wave_sample; //ecg_filterout;
        ecg_data_buff[ecg_stream_cnt++] = (ecg_wave_sample >> 8); //(ecg_filterout >> 8);
      
        if (ecg_stream_cnt >= 18)
        {
          ecg_buf_ready = true;
          ecg_stream_cnt = 0;
        }
      }
      else if (Healthypi_Mode == WEBSERVER_MODE)
      {
        if (index_cnt< 1200)
        {  
          static u8 ec = 0;
          tmp_ecgbu += String(ecg_wave_sample);
          tmp_ecgbu += ',';
          number_of_samples++;
        }
      }
   
      DataPacket[14] = global_RespirationRate;
      DataPacket[16] = global_HeartRate;
    }
  
    memcpy(&DataPacket[0], &ecg_filterout, 2);
    memcpy(&DataPacket[2], &resp_filterout, 2);
    SPI.setDataMode (SPI_MODE0);
    afe4490.get_AFE4490_Data(&afe44xx_raw_data, AFE4490_CS_PIN,AFE4490_DRDY_PIN);
    ppg_wave_ir = (u16)(afe44xx_raw_data.IR_data >> 8);
    ppg_wave_ir = ppg_wave_ir;
    
    ppg_data_buff[ppg_stream_cnt++] = (u8)ppg_wave_ir;
    ppg_data_buff[ppg_stream_cnt++] = (ppg_wave_ir >> 8);
  
    if (ppg_stream_cnt >= 18)
    {
      ppg_buf_ready = true;
      ppg_stream_cnt = 0;
    }
  
    memcpy(&DataPacket[4], &afe44xx_raw_data.IR_data, sizeof(signed long));
    memcpy(&DataPacket[8], &afe44xx_raw_data.RED_data, sizeof(signed long));
  
    if (afe44xx_raw_data.buffer_count_overflow)
    {
      if (afe44xx_raw_data.spo2 == -999)
      {
        DataPacket[15] = 0;
        sp02 = 0;
      }
      else
      { 
        DataPacket[15] =  afe44xx_raw_data.spo2;
        sp02 = (u8)afe44xx_raw_data.spo2;       
      }

      spo2_calc_done = true;
      afe44xx_raw_data.buffer_count_overflow = false;
    }
   
    DataPacket[17] = 80;  // bpsys
    DataPacket[18] = 120; // bp dia
    DataPacket[19]=  ads1292r_raw_data.status_reg;  

    SPI.setDataMode (SPI_MODE1);
   
    if (time_count++ * (1000 / SAMPLING_RATE) > MAX30205_READ_INTERVAL)
    {      
      temp = tempSensor.getTemperature() * 100; // read temperature for every 100ms
      temperature =  (u16)temp;
      time_count = 0;
      DataPacket[12] = (u8)temperature; 
      DataPacket[13] = (u8)(temperature >> 8);
      temp_data_ready = true;
      //reading the battery with same interval as temp sensor
      read_battery_value();
    }
  
    if (Healthypi_Mode == BLE_MODE)
    {
      handle_ble_stack();
    }
    else if (Healthypi_Mode == V3_MODE)
    {
      send_data_serial_port();
    }
  }

  if (mode_write_flag)
  {
    mode_write_flag = false;
    const char t = 0x0f;
    writeFile(SPIFFS, "/v4_mode.txt", &t);
    Serial.println("setting webserver mode..\n restarts in 3 sec"); 
    delay(3000);
    restart_indication();
    ESP.restart();
  }
 
  if (slide_switch_flag)
  {
    slide_switch_flag = false;
    Serial.println("changing the mode..\n restarts in 3 sec");
    delay(3000);
    ESP.restart();
  }
 
  if (credential_success_flag)
  {
    detachInterrupt(ADS1292_DRDY_PIN);
    writeFile(SPIFFS, "/ssid_list.txt", ssid_to_connect.c_str());
    writeFile(SPIFFS, "/pass_list.txt", password_to_connect.c_str());
    writeFile(SPIFFS, "/mode_status.txt", "datawritten");
    credential_success_flag = false;
    const char u = 0x0a;
    writeFile(SPIFFS, "/web_mode.txt", &u);
    restart_indication();
    ESP.restart();   
  }
 
  if (STA_mode_indication)
  {
    for (int dutyCycle = 255; dutyCycle >= 0; dutyCycle -= 3)
    {
      ledcWrite(ledChannel, dutyCycle);   
      delay(25);
    }

    STA_mode_indication = false;
  }
 
  server.handleClient();

  netsbloxAnnounce();
  handleNetsbloxUDP();
}
