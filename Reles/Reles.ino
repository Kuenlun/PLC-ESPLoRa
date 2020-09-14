/************
 * INCLUDES *
 ************/
#include <SPI.h>                // To communicate with the relay driver and LoRa module
#include <LoRa.h>               // To send and receive messages with the LoRa module
#include <Wire.h>               // To communicate with the DS3231 (I2C)
#include <RTClib.h>             // To manage the DS3231
#include <EEPROM.h>             // To store the programming of the device
// To read the temperature of the probe
#include <OneWire.h>            
#include <DallasTemperature.h>


/**************
 * ESP32 PINS *
 **************/
// SPI
#define SPI_CLK 18
#define SPI_MISO 19
#define SPI_MOSI 23
// LoRa pins
#define LORA_CS 5
#define LORA_RST 17
#define LORA_DIO0 16
// Relay driver
#define DRIVER_RST 32
#define DRIVER_CS 33
// I2C
#define SCL 22
#define SDA 21
// RTC
#define RTC_INT 25
#define RTC_RST 27
// GPIOs for indications
#define LED_RED 4
// Temperature probe
#define TEMP_PROBE 26


/*************
 * CONSTANTS *
 *************/
#define EEPROM_SIZE 72    // Number of bytes that will be used to store the programming
#define MESSAGE_LEN 85    // Number of bytes that the message will take
#define OFFSET_PROGRAM 13 // Where the programming starts in the message
#define SPI_CLK_FRQ 1E6   // SPI Clock (Máx 5MHz)


/********************
 * GLOBAL VARIABLES *
 ********************/
RTC_DS3231 rtc;
byte current_states = B00000000;  // Current states of the outputs
byte selector = B00000000;        // Selector between states and programming
byte message[MESSAGE_LEN] = {0};  // Container for sending and receiving messages
// To read the temperature of the probe
OneWire oneWireBus(TEMP_PROBE);
DallasTemperature sensor(&oneWireBus);


/*************
 * FUNCTIONS *
 *************/
void print_time(){
  // Prints the time from the RTC
  DateTime fecha = rtc.now();
  char time_buffer[26] = " ";
  char* formato = "%d/%02d/%02d - %02d:%02d:%02d";
  sprintf(time_buffer, formato, fecha.year(), fecha.month(), fecha.day(),
          fecha.hour(), fecha.minute(), fecha.second());
  Serial.print(time_buffer);
}

void print_message(){
  // Prints the message
  byte i;
  for(i = 0; i < MESSAGE_LEN; i++){
    Serial.print(i);
    Serial.print(":\t");
    Serial.println(message[i], BIN);
  }
}

unsigned int temperature_to_message(float value){
  // Converts the float temperature to unsigned int 16
  // 1  bit for the sign
  // 11 bits for the integer part
  // 4  bits for the decimal part
  unsigned int out = 0x0000;
  int aux_int = (int)value;
  unsigned int aux_uint;
  unsigned int aux_decimal = (value - aux_int) * 10;
  for(byte i = 0; i < 4; i++){
    bitWrite(out, i, bitRead(aux_decimal, i));
  }
  if (value < 0){
    bitWrite(out, 15, 1);
    aux_uint = aux_int * (-1);
  }else{
    aux_uint = aux_int;
  }
  for(byte i = 0; i < 11; i++){
    bitWrite(out, i + 4, bitRead(aux_uint, i));
  }
  return out;
}

unsigned int states_to_TLE8108(byte states){
  // Converts the given states to TLE8108 SPI protocol
  // 0 --> 11   OFF state
  // 1 --> 10    ON state
  unsigned int out = 0x0000;
  byte aux = B00000001;
  byte i;
  for(i = 0; i < 8; i++){
    if (((states & aux) >> i) == 1){
      out = out | (B10 << (i * 2));
    }else{
      out = out | (B11 << (i * 2));
    }
    aux = aux << 1;
  }
  return(out);
}

byte weekday(int year, int month, int day){
  // Calculate day of week in proleptic Gregorian calendar. Sunday == 0
  int adjustment, mm, yy;
  if (year<2000) year+=2000;
  adjustment = (14 - month) / 12;
  mm = month + 12 * adjustment - 2;
  yy = year - adjustment;
  return (day + (13 * mm - 1) / 5 + yy + yy / 4 - yy / 100 + yy / 400) % 7;
}

byte calculate_outputs(){
  // Calculate wich states to switch on or off depending on the states and the programming
  byte states = B00000000;
  byte i;
  byte aux_dayofweek;
  byte aux_minute = 0;
  DateTime fecha = rtc.now();
  for(i = 0; i < 8; i++){
    // Check the selector. If 1 then check time, if 0 then check state
    if (bitRead(selector, i)){
      // Check the month
      if (fecha.month() < 9){
        if (!bitRead(EEPROM.read(i * 9 + 7), fecha.month() - 1)){
          continue;
        }
      }else{
        if (!bitRead(EEPROM.read(i * 9 + 8), fecha.month() - 9)){
          continue;
        }
      }
      // Check the week
      aux_dayofweek = weekday(fecha.year(), fecha.month(), fecha.day());
      if (!bitRead(EEPROM.read(i * 9 + 6), aux_dayofweek)){
        continue;
      }
      // Check the hour
      if (fecha.minute() >= 30){
          aux_minute = 1;
      }
      if (fecha.hour() < 4){
        if (!bitRead(EEPROM.read(i * 9 + 0), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (fecha.hour() < 8){
        if (!bitRead(EEPROM.read(i * 9 + 1), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (fecha.hour() < 12){
        if (!bitRead(EEPROM.read(i * 9 + 2), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (fecha.hour() < 16){
        if (!bitRead(EEPROM.read(i * 9 + 3), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (fecha.hour() < 20){
        if (!bitRead(EEPROM.read(i * 9 + 4), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (fecha.hour() < 24){
        if (!bitRead(EEPROM.read(i * 9 + 5), (fecha.hour() % 4) * 2 + aux_minute)){
          continue;
        }
      }
      bitWrite(states, i, 1);
    }else{
      bitWrite(states, i, bitRead(current_states, i));
    }
  }
  return states;
}

unsigned int relay_switcher(byte states){
  // Send the states to the relay driver
  unsigned int driver_out;
  SPI.beginTransaction(SPISettings(SPI_CLK_FRQ, MSBFIRST, SPI_MODE1));
  digitalWrite(DRIVER_CS, LOW);
  driver_out = SPI.transfer16(states_to_TLE8108(states));
  digitalWrite(DRIVER_CS, HIGH);
  SPI.endTransaction();
  // Return information given by the relay driver
  return driver_out;
}

void message_to_variables(){
  // Update variables from the message
  byte i, j;
  current_states = message[7];
  selector = message[8];
  for(i = 0; i < 8; i++){
    for(j = 0; j < 9; j++){
      EEPROM.write(i * 9 + j, message[OFFSET_PROGRAM + i * 9 + j]);
      EEPROM.commit();
    }
  }
}

byte* variables_to_message(byte states){
  // Creates the message from the variables
  DateTime fecha = rtc.now();
  unsigned int temp;
  message[0] = lowByte(fecha.second());
  message[1] = lowByte(fecha.minute());
  message[2] = lowByte(fecha.hour());
  message[3] = lowByte(fecha.day());
  message[4] = lowByte(fecha.month());
  message[5] = lowByte(fecha.year() >> 8);
  message[6] = lowByte(fecha.year());
  message[7] = states;
  message[8] = selector;
  temp = temperature_to_message(rtc.getTemperature());
  message[9] = lowByte(temp >> 8);
  message[10] = lowByte(temp);
  sensor.requestTemperatures();
  temp = temperature_to_message(sensor.getTempCByIndex(0));
  message[11] = lowByte(temp >> 8);
  message[12] = lowByte(temp);
  for(byte i = 0; i < 8; i++){
    for(byte j = 0; j < 9; j++){
      message[OFFSET_PROGRAM + i * 9 + j] = EEPROM.read(i * 9 + j);
    }
  }
  return message;
}

void send_data(byte* message, unsigned int len){
  // Send the message
  digitalWrite(LED_RED, HIGH);
  LoRa.beginPacket();
  LoRa.write(message, len);
  LoRa.endPacket();
  Serial.println("Message sent");
  digitalWrite(LED_RED, LOW);
}


/*********
 * SETUP *
 *********/
void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  delay(500);
  
  // Initialize the outputs
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, LOW);
  pinMode(DRIVER_RST, OUTPUT);
  digitalWrite(DRIVER_RST, HIGH);
  pinMode(DRIVER_CS, OUTPUT);
  digitalWrite(DRIVER_CS, HIGH);
  
  // Initialize SPI
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI);
  Serial.println("SPI started");

  // Initialize LoRa transceiver module
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  LoRa.setSPIFrequency(SPI_CLK_FRQ);
  while (!LoRa.begin(433E6)) {
    Serial.println("Starting LoRa failed!");
    delay(500);
  }
  Serial.println("LoRa begined");
  // Change sync word to match the receiver
  // The sync word assures you don't get LoRa messages from other LoRa transceivers
  // ranges from 0-0xFF
  LoRa.setSyncWord(0xAE);
  
  // Initialize the RTC
  if (!rtc.begin()){
    Serial.println("RTC module not found");
    while(1);
  }
  // Update the RTC time
  rtc.adjust(DateTime(__DATE__, __TIME__));

  // Initialize the temperature probe
  sensor.begin(); 
}


/********
 * LOOP *
 ********/
void loop() {
  byte flag = false;
  byte i = 0;
  unsigned int relay_driver_out;
  // Try to parse packet
  int packetSize = LoRa.parsePacket();
  if (packetSize) { // A packet has arrived
    digitalWrite(LED_RED, HIGH);
    Serial.print("Message received");
    while (LoRa.available()) {  // There are still bytes to read from the packet
      if (i == MESSAGE_LEN - 1){
        flag = true;
      }else if(i >= MESSAGE_LEN){
        break;
      }
      message[i] = LoRa.read();
      i++;
    }
    // Print RSSI of packet
    Serial.print(" with RSSI ");
    Serial.println(LoRa.packetRssi());
    digitalWrite(LED_RED, LOW);
    // If we have received all the message correctly
    if (flag){
      //print_message();
      // Update the variables
      message_to_variables();
      // Check if we need to turn on or off the outputs
      relay_driver_out = relay_switcher(calculate_outputs());
      if (relay_driver_out != 0xFFFF){
        Serial.print("WARNING - Relay Driver Response: ");
        Serial.println(relay_driver_out, BIN);
      }
      // Send confirmation to the server
      send_data(variables_to_message(current_states), MESSAGE_LEN);
    }
  }
  // Check if we need to turn on or off the outputs
  relay_driver_out = relay_switcher(calculate_outputs());
  if (relay_driver_out != 0xFFFF){
    Serial.print("WARNING - Relay Driver Response: ");
    Serial.println(relay_driver_out, BIN);
  }
}
