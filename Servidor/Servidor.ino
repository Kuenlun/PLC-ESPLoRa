/************
 * INCLUDES *
 ************/
#include <WiFi.h>   // To create the server and connect to the WiFi
#include <SPI.h>    // To communicate with the LoRa module
#include <LoRa.h>   // To send and receive messages with the LoRa module
#include <EEPROM.h> // To store the programming of the device


/**************
 * ESP32 PINS *
 **************/
//SPI
#define SPI_CLK 18
#define SPI_MISO 19
#define SPI_MOSI 23
//Define the pins used by the transceiver module
#define LORA_CS 5
#define LORA_RST 17
#define LORA_DIO0 16
//GPIOs for indications
#define LED_RED 4
#define LED_GREEN1 32
#define LED_GREEN2 33
#define LED_GREEN3 25
#define LED_GREEN4 26
#define LED_GREEN5 27
#define LED_GREEN6 12
#define LED_GREEN7 13
#define LED_GREEN8 2


/*************
 * CONSTANTS *
 *************/
#define EEPROM_SIZE 72    // Number of bytes that will be used to store the programming
#define MESSAGE_LEN 85    // Number of bytes that the message will take
#define OFFSET_PROGRAM 13 // Where the programming starts in the message
#define SPI_CLK_FRQ 1E6   // SPI Clock (Máx 5MHz)
#define RESEND_ATTEMPTS 5 // Number of times a message will be sent while waiting for confirmation
// Replace with your network credentials
const char* ssid = "TP-Link_DE5B";
const char* password = "UnaBuenaContrasena";


/********************
 * GLOBAL VARIABLES *
 ********************/
WiFiServer server(80);  // Set web server port number to 80
String header;          // Variable to store the HTTP request

// Variables to store the date
unsigned int time_seg, time_min, time_hour, time_day, time_month, time_year;
byte current_states = B00000000;  // Current states of the outputs
byte selector = B00000000;        // Selector between states and programming
byte programs[8][9] = {0};        // Variables to store the programming of the device
float temperature1, temperature2; // Variables to store the temperature of the sensors
byte message[MESSAGE_LEN] = {0};  // Container for sending and receiving messages

byte programming = 0;             // Auxiliar for the html
int rssi = 0;                     // Auxiliar to store the RSSI of the message received

// For server timing
unsigned long currentTime = millis();
unsigned long previousTime = 0; 
const long timeoutTime = 2000;

// For send_data function timing
unsigned long startMillis;
unsigned long currentMillis;
const unsigned long period = 500;


/*************
 * FUNCTIONS *
 *************/
void print_time(){
  // Prints the time
  char time_buffer[26] = " ";
  char* formato = "%d/%02d/%02d - %02d:%02d:%02d";
  sprintf(time_buffer, formato, time_year, time_month, time_day, time_hour, time_min, time_seg);
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

float message_to_temperature(byte upper, byte lower){
  // Converts unsigned int 16 to float
  // 1  bit for the sign
  // 11 bits for the integer part
  // 4  bits for the decimal part
  unsigned int joined = 0;
  unsigned int decimal = 0;
  unsigned int integer = 0;
  float out;
  joined = upper;
  joined = joined << 8;
  joined = joined | lower;
  for(byte i = 0; i < 4; i++){
    bitWrite(decimal, i, bitRead(joined, i));
  }
  for(byte i = 0; i < 11; i++){
    bitWrite(integer, i, bitRead(joined, i + 4));
  }
  out = decimal;
  out /= 10;
  if (bitRead(joined, 15)){
    out = out * (-1) - integer;
  }else{
    out += integer;
  }
  return out;
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
  for(i = 0; i < 8; i++){
    // Check the selector. If 1 then check time, if 0 then check state
    if (bitRead(selector, i)){
      // Check the month
      if (time_month < 9){
        if (!bitRead(EEPROM.read(i * 9 + 7), time_month - 1)){
          continue;
        }
      }else{
        if (!bitRead(EEPROM.read(i * 9 + 8), time_month - 9)){
          continue;
        }
      }
      // Check the week
      aux_dayofweek = weekday(time_year, time_month, time_day);
      if (!bitRead(EEPROM.read(i * 9 + 6), aux_dayofweek)){
        continue;
      }
      // Check the hour
      if (time_min >= 30){
          aux_minute = 1;
      }
      if (time_hour < 4){
        if (!bitRead(EEPROM.read(i * 9 + 0), (time_hour % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (time_hour < 8){
        if (!bitRead(EEPROM.read(i * 9 + 1), (time_hour % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (time_hour < 12){
        if (!bitRead(EEPROM.read(i * 9 + 2), (time_hour % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (time_hour < 16){
        if (!bitRead(EEPROM.read(i * 9 + 3), (time_hour % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (time_hour < 20){
        if (!bitRead(EEPROM.read(i * 9 + 4), (time_hour % 4) * 2 + aux_minute)){
          continue;
        }
      }else if (time_hour < 24){
        if (!bitRead(EEPROM.read(i * 9 + 5), (time_hour % 4) * 2 + aux_minute)){
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

void message_to_variables(){
  // Update variables from the message
  byte i, j;
  time_seg = message[0];
  time_min = message[1];
  time_hour = message[2];
  time_day = message[3];
  time_month = message[4];
  time_year = message[5];
  time_year = time_year << 8;
  time_year = time_year | message[6];

  if (message[7] != current_states){
    Serial.println("We Should Update the relay states");
  }
  if (message[8] != selector){
    Serial.println("We Should Update the relay selector");
  }
  temperature1 = message_to_temperature(message[9], message[10]);
  temperature2 = message_to_temperature(message[11], message[12]);
  
  for(i = 0; i < 8; i++){
    for(j = 0; j < 9; j++){
      if (message[OFFSET_PROGRAM + i * 9 + j] != EEPROM.read(i * 9 + j)){
        Serial.println("We Should Update the relay programming");
      }
    }
  }
}

byte* variables_to_message(byte states){
  // Creates the message from the variables
  byte i, j;
  message[7] = states;
  message[8] = selector;
  for(i = 0; i < 8; i++){
    for(j = 0; j < 9; j++){
      message[OFFSET_PROGRAM + i * 9 + j] = EEPROM.read(i * 9 + j);
    }
  }
  return message;
}

void EEPROM_to_variables(){
  // Set variables with EEPROM values
  byte i, j;
  for (i = 0; i < 8; i++){
    for (j = 0; j < 9; j++){
      programs[i][j] = EEPROM.read(i * 9 + j);
    }
  }
}


void variables_to_EEPROM(){
  // Set EEPROM with variables values
  Serial.println("Escribiendo a memoria");
  byte i, j;
  for (i = 0; i < 8; i++){
    for (j = 0; j < 9; j++){
      EEPROM.write(i * 9 + j, programs[i][j]);
      EEPROM.commit();
    }
  }
}

int send_data(byte* message, unsigned int len){
  // Sends the message through LoRa and listens for the response
  byte flag = false;
  byte i, j;
  EEPROM_to_variables(); // Update the variables
  for (i = 0; i < RESEND_ATTEMPTS; i++){
    // Send LoRa packet to receiver
    digitalWrite(LED_RED, HIGH);
    LoRa.beginPacket();
    LoRa.write(message, len);
    LoRa.endPacket();
    Serial.println("Message sent");
    digitalWrite(LED_RED, LOW);
  
    // Wait for confirmation
    startMillis = millis();
    while(true){
      currentMillis = millis();
      // Check if too much time has elapsed
      if (currentMillis - startMillis >= period){
        break;
      }
      int packetSize = LoRa.parsePacket();
      if (packetSize) { // A packet has arrived
        j = 0;
        while (LoRa.available()) {  // There are still bytes to read from the packet
          if (j == MESSAGE_LEN - 1){
            flag = true;
          }else if(j >= MESSAGE_LEN){
            break;
          }
          message[j] = LoRa.read();      
          j++;
        }
        // Print RSSI of packet
        rssi = LoRa.packetRssi();
        Serial.print("' with RSSI ");
        Serial.println(rssi);
        // If we have received all the message correctly
        if (flag){
          //print_message();
          // Update the variables
          message_to_variables();
          return 0;
        }
      }
      flag = false;
    }
  }
  return -1;
}

void update_outputs(){
  byte i;
  byte states = calculate_outputs();
  if (bitRead(states, 0)){
    digitalWrite(LED_GREEN1, HIGH);
  }else{
    digitalWrite(LED_GREEN1, LOW);
  }
  if (bitRead(states, 1)){
    digitalWrite(LED_GREEN2, HIGH);
  }else{
    digitalWrite(LED_GREEN2, LOW);
  }
  if (bitRead(states, 2)){
    digitalWrite(LED_GREEN3, HIGH);
  }else{
    digitalWrite(LED_GREEN3, LOW);
  }
  if (bitRead(states, 3)){
    digitalWrite(LED_GREEN4, HIGH);
  }else{
    digitalWrite(LED_GREEN4, LOW);
  }
  if (bitRead(states, 4)){
    digitalWrite(LED_GREEN5, HIGH);
  }else{
    digitalWrite(LED_GREEN5, LOW);
  }
  if (bitRead(states, 5)){
    digitalWrite(LED_GREEN6, HIGH);
  }else{
    digitalWrite(LED_GREEN6, LOW);
  }
  if (bitRead(states, 6)){
    digitalWrite(LED_GREEN7, HIGH);
  }else{
    digitalWrite(LED_GREEN7, LOW);
  }
  if (bitRead(states, 7)){
    digitalWrite(LED_GREEN8, HIGH);
  }else{
    digitalWrite(LED_GREEN8, LOW);
  }
}

void print_buttons(WiFiClient client, byte x){
  // Prints the buttons of the programming page
  byte h1, h2, m1, m2;
  char html_line[150]=" ";
  for (byte i = 0; i < 8; i++){
    h1 = 4 * x + i / 2;
    h2 = 4 * x + (i + 1) / 2;
    if (i % 2 == 0){
      m1 = 0;
      m2 = 30;
    }else{
      m1 = 30;
      m2 = 0;
    }
    if (bitRead(programs[programming][x], i)){
      char* formato = "<p><a href='/h/%i%i/off'><button style='width:12.5%%' class='button_on'>%02d:%02d<br/>%02d:%02d</button></a></p>";
      sprintf(html_line, formato, x, i, h1, m1, h2, m2);
      client.println(html_line);
    }else{
      char* formato = "<p><a href='/h/%i%i/on'><button style='width:12.5%%' class='button_off'>%02d:%02d<br/>%02d:%02d</button></a></p>";
      sprintf(html_line, formato, x, i, h1, m1, h2, m2);
      client.println(html_line);
    }
  }
}

void print_outputs(WiFiClient client, byte group){
  client.println("        <div class='btn-group' style='border-style:ridge;padding:0px 25px'>");
  
  char html_line1[150]=" ";
  char* formato1 = "<p style='font-size: 20px; width:100%%; height:15px;'>Salida %i</p>";
  sprintf(html_line1, formato1, group + 1);
  client.println(html_line1);
  
  if (bitRead(calculate_outputs(), group)){
    if (bitRead(selector, group)){
      client.println("          <p style='font-size: 12px; background-color:#4CAF50;color: white;'>Programa: ON</p>");
    }else{
      client.println("          <p style='font-size: 12px; background-color:#4CAF50;color: white;'>ON</p>");
    }
  }else{
    if (bitRead(selector, group)){
      client.println("          <p style='font-size: 12px; background-color:#F44336;color: white;'>Programa: OFF</p>");
    }else{
      client.println("          <p style='font-size: 12px; background-color:#F44336;color: white;'>OFF</p>");
    }
  }
  if (bitRead(selector, group)){
    if (bitRead(current_states, group)){
      client.println("          <p><a href='/'><button style='width:100%; border: 1px solid green' class='button_blocked'>ON</button></a></p>");
    }else{
      client.println("          <p><a href='/'><button style='width:100%; border: 1px solid red' class='button_blocked'>OFF</button></a></p>");
    }
  }else{
    if (bitRead(current_states, group)){
      char html_line2[150]=" ";
      char* formato2 = "<p><a href='/but/%i/off'><button style='width:100%%; border: 1px solid green' class='button_on'>ON</button></a></p>";
      sprintf(html_line2, formato2, group);
      client.println(html_line2);
    }else{
      char html_line3[150]=" ";
      char* formato3 = "<p><a href='/but/%i/on'><button style='width:100%%; border: 1px solid red' class='button_off'>OFF</button></a></p>";
      sprintf(html_line3, formato3, group);
      client.println(html_line3);
    }
  }
  if (bitRead(selector, group)){
    char html_line4[150]=" ";
    char* formato4 = "<p><a href='/sel/%i/off'><button style='float: left; width:100%%;' class='btn btn-default col-md-4'>Bot&oacute;n / Programa</button></a></p>";
    sprintf(html_line4, formato4, group);
    client.println(html_line4);
  }else{
    char html_line5[150]=" ";
    char* formato5 = "<p><a href='/sel/%i/on'><button style='float: left; width:100%%;' class='btn btn-default col-md-4'>Bot&oacute;n / Programa</button></a></p>";
    sprintf(html_line5, formato5, group);
    client.println(html_line5);
  }
  char html_line6[150]=" ";
  char* formato6 = "<p><a href='/prg/%i'><button class='btn btn-default col-md-4'>Programar</button></a></p>";
  sprintf(html_line6, formato6, group);
  client.println(html_line6);
  client.println("        </div>");
}


/*********
 * SETUP *
 *********/
void setup() {
  Serial.begin(115200);
  
  // Initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  delay(500);
  EEPROM_to_variables();

  // Initialize the outputs
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN1, OUTPUT);
  pinMode(LED_GREEN2, OUTPUT);
  pinMode(LED_GREEN3, OUTPUT);
  pinMode(LED_GREEN4, OUTPUT);
  pinMode(LED_GREEN5, OUTPUT);
  pinMode(LED_GREEN6, OUTPUT);
  pinMode(LED_GREEN7, OUTPUT);
  pinMode(LED_GREEN8, OUTPUT);
  // Set outputs to LOW
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN1, LOW);
  digitalWrite(LED_GREEN2, LOW);
  digitalWrite(LED_GREEN3, LOW);
  digitalWrite(LED_GREEN4, LOW);
  digitalWrite(LED_GREEN5, LOW);
  digitalWrite(LED_GREEN6, LOW);
  digitalWrite(LED_GREEN7, LOW);
  digitalWrite(LED_GREEN8, LOW);
  
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

  // Connect to Wi-Fi network with SSID and password
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  // Print local IP address and start web server
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  server.begin();
  Serial.println("HTTP server started");
}


/********
 * LOOP *
 ********/
void loop(){
  byte aux_button;
  byte* aux_message;
  WiFiClient client = server.available();   // Listen for incoming clients
  if (client) {                             // If a new client connects,
    currentTime = millis();
    previousTime = currentTime;
    Serial.println("New Client.");          // print a message out in the serial port
    String currentLine = "";                // make a String to hold incoming data from the client
    while (client.connected() && currentTime - previousTime <= timeoutTime) {  // loop while the client's connected
      currentTime = millis();
      if (client.available()) {             // if there's bytes to read from the client,
        char c = client.read();             // read a byte, then
        Serial.write(c);                    // print it out the serial monitor
        header += c;
        if (c == '\n') {                    // if the byte is a newline character
          // if the current line is blank, you got two newline characters in a row.
          // that's the end of the client HTTP request, so send a response:
          if (currentLine.length() == 0) {
            // HTTP headers always start with a response code (e.g. HTTP/1.1 200 OK)
            // and a content-type so the client knows what's coming, then a blank line:
            client.println("HTTP/1.1 200 OK");
            client.println("Content-type:text/html");
            client.println("Connection: close");
            client.println();
            
            /**************************
             * Interpret the requests *
             **************************/
            int index = header.indexOf("\n");
            if (header.substring(5, index).indexOf("but/") >= 0){
              // If a button has been pressed
              if (bitRead(selector, header[9] - 48) == 0){
                // If the selector is in button mode
                if (header.substring(5, index).indexOf("on") >= 0){
                  // Send information
                  aux_button = 0x00;
                  aux_message = variables_to_message(current_states | bitWrite(aux_button, header[9] - 48, 1));
                  send_data(aux_message, MESSAGE_LEN);
                  // Change the state
                  bitWrite(current_states, header[9] - 48, 1);
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  // Send information
                  aux_button = 0xFF;
                  aux_message = variables_to_message(current_states & bitWrite(aux_button, header[9] - 48, 0));
                  send_data(aux_message, MESSAGE_LEN);
                  // Change the state
                  bitWrite(current_states, header[9] - 48, 0);
                }
              }
            }else if (header.substring(5, index).indexOf("prg/") >= 0){
              programming = header[9] - 48;
            }else if (header.substring(0, index).indexOf("GET /h/") >= 0){
              if (header.substring(5, index).indexOf("all") >= 0){
                if (header.substring(5, index).indexOf("on") >= 0){
                  for(byte i = 0; i < 6; i++){
                    programs[programming][i] = 0xFF;
                  }
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  for(byte i = 0; i < 6; i++){
                    programs[programming][i] = 0x00;
                  }
                }
              }else{
                if (header.substring(5, index).indexOf("on") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 1);
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 0);
                }
              }
            }else if (header.substring(0, index).indexOf("GET /d/") >= 0){
              if (header.substring(5, index).indexOf("all") >= 0){
                if (header.substring(5, index).indexOf("on") >= 0){
                  programs[programming][6] = 0xFF;
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  programs[programming][6] = 0x00;
                }
              }else{
                if (header.substring(5, index).indexOf("on") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 1);
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 0);
                }
              }
            }else if (header.substring(0, index).indexOf("GET /m/") >= 0){
              if (header.substring(5, index).indexOf("all") >= 0){
                if (header.substring(5, index).indexOf("on") >= 0){
                  programs[programming][7] = 0xFF;
                  programs[programming][8] = 0xFF;
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  programs[programming][7] = 0x00;
                  programs[programming][8] = 0x00;
                }
              }else{
                if (header.substring(5, index).indexOf("on") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 1);
                }else if (header.substring(5, index).indexOf("off") >= 0){
                  bitWrite(programs[programming][header[7] - 48], header[8] - 48, 0);
                }
              }
            }else if (header.substring(0, index).indexOf("GET /sincronize") >= 0){
              send_data(variables_to_message(current_states), MESSAGE_LEN);
            }else if (header.substring(0, index).indexOf("GET /program") >= 0){
              variables_to_EEPROM();
              send_data(variables_to_message(current_states), MESSAGE_LEN);
            }else if (header.substring(0, index).indexOf("GET /discard") >= 0){
              EEPROM_to_variables();
            }else if (header.substring(0, index).indexOf("GET /sel/") >= 0){
              if (header.substring(5, index).indexOf("on") >= 0){
                bitWrite(selector, header[9] - 48, 1);
              }else if (header.substring(5, index).indexOf("off") >= 0){
                bitWrite(selector, header[9] - 48, 0);
              }
              send_data(variables_to_message(current_states), MESSAGE_LEN);
            }

            /***********************************
             * SHOW THE CORRESPONDING WEB PAGE *
             ***********************************/
            // Show the programming page
            if (header.substring(0, index).indexOf("GET /prg/") >= 0 |
                header.substring(0, index).indexOf("GET /h/") >= 0 |
                header.substring(0, index).indexOf("GET /d/") >= 0 |
                header.substring(0, index).indexOf("GET /m/") >= 0) {
              client.println("<!Doctype html>");
              client.println("<html>");
              client.println("  <head>");
              client.println("    <meta name='viewport' content='width=device-width, initial-scale=1'>");
              client.println("    <link rel='icon' href='data:,'>");
              client.println("    <style>");
              client.println("      html {");
              client.println("        font-family: Helvetica;");
              client.println("        display: inline-block;");
              client.println("        margin: 0px auto;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("      .btn-group button:not(:last-child) {");
              client.println("        border-right: none; /* Prevent double borders */");
              client.println("      }");
              client.println("      /* Clear floats (clearfix hack) */");
              client.println("      .btn-group:after {");
              client.println("        content: "";");
              client.println("        clear: both;");
              client.println("        display: table;");
              client.println("      }");
              client.println("      /* Add a background color on hover */");
              client.println("      .btn-group button:hover {");
              client.println("        background-color: #3e8e41;");
              client.println("      }");
              client.println("      .btn-group button {");
              client.println("        padding: 5px 8px;");
              client.println("        font-size: 12px;");
              client.println("      }");
              client.println("      .button_on {");
              client.println("        background-color: #4CAF50;");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("      .button_off {");
              client.println("        background-color: #555555;");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("      .fila button {");
              client.println("        padding: 5px 8px;");
              client.println("        font-size: 12px;");
              client.println("      }");
              client.println("      .fila > * {");
              client.println("        padding:0px;");
              client.println("        display: inline-block;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("      .matriz {");
              client.println("        padding:5px;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("    </style>");
              client.println("  </head>");
              client.println("  <body>");
              client.println("    <h1 style='text-align: center;'>TFG Web Server</h1>");
              
              char html_line[150]=" ";
              char* formato = "<h2 style='text-align: center;'>Panel de programaci&oacute;n de la salida %i</h2>";
              sprintf(html_line, formato, programming + 1);
              client.println(html_line);
              
              client.println("    <hr>");
              client.println("    <h3 style='text-align: center;'>Horas</h3>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 0);
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 1);
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 2);
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 3);
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 4);
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              print_buttons(client, 5);
              client.println("      </div>");
              client.println("      <div class='fila'>");
              client.println("        <p><a href='/h/all/on'><button type='button' class='btn btn-default col-md-4'>Todos</button></a></p>");
              client.println("        <p><a href='/h/all/off'><button type='button' class='btn btn-default col-md-4'>Ninguno</button></a></p>");
              client.println("      </div>");
              client.println("    </div>");
              client.println("    <hr>");
              client.println("    <h3 style='text-align: center;'>D&iacute;as</h3>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='btn-group' style='width:100%'>");
              if (bitRead(programs[programming][6], 1)){
                client.println("        <p><a href='/d/61/off'><button style='width:25%' class='button_on'>Lunes</button></a></p>");
              }else{
                client.println("        <p><a href='/d/61/on'><button style='width:25%' class='button_off'>Lunes</button></a></p>");
              }
              if (bitRead(programs[programming][6], 2)){
                client.println("        <p><a href='/d/62/off'><button style='width:25%' class='button_on'>Martes</button></a></p>");
              }else{
                client.println("        <p><a href='/d/62/on'><button style='width:25%' class='button_off'>Martes</button></a></p>");
              }
              if (bitRead(programs[programming][6], 3)){
                client.println("        <p><a href='/d/63/off'><button style='width:25%' class='button_on'>Mi&eacute;rcoles</button></a></p>");
              }else{
                client.println("        <p><a href='/d/63/on'><button style='width:25%' class='button_off'>Mi&eacute;rcoles</button></a></p>");
              }
              if (bitRead(programs[programming][6], 4)){
                client.println("        <p><a href='/d/64/off'><button style='width:25%' class='button_on'>Jueves</button></a></p>");
              }else{
                client.println("        <p><a href='/d/64/on'><button style='width:25%' class='button_off'>Jueves</button></a></p>");
              }
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%>");
              if (bitRead(programs[programming][6], 5)){
                client.println("        <p><a href='/d/65/off'><button style='width:33.3%' class='button_on'>Viernes</button></a></p>");
              }else{
                client.println("        <p><a href='/d/65/on'><button style='width:33.3%' class='button_off'>Viernes</button></a></p>");
              }
              if (bitRead(programs[programming][6], 6)){
                client.println("        <p><a href='/d/66/off'><button style='width:33.3%' class='button_on'>S&aacute;bado</button></a></p>");
              }else{
                client.println("        <p><a href='/d/66/on'><button style='width:33.3%' class='button_off'>S&aacute;bado</button></a></p>");
              }
              if (bitRead(programs[programming][6], 0)){
                client.println("        <p><a href='/d/60/off'><button style='width:33.3%' class='button_on'>Domingo</button></a></p>");
              }else{
                client.println("        <p><a href='/d/60/on'><button style='width:33.3%' class='button_off'>Domingo</button></a></p>");
              }
              client.println("      </div>");
              client.println("      <div class='fila'>");
              client.println("        <p><a href='/d/all/on'><button type='button' class='btn btn-default col-md-4'>Todos</button></a></p>");
              client.println("        <p><a href='/d/all/off'><button type='button' class='btn btn-default col-md-4'>Ninguno</button></a></p>");
              client.println("      </div>");
              client.println("    </div>");
              client.println("    <hr>");
              client.println("    <h3 style='text-align: center;'>Meses</h3>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='btn-group' style='width:100%'>");
              if (bitRead(programs[programming][7], 0)){
                client.println("        <p><a href='/d/70/off'><button style='width:25%' class='button_on'>Enero</button></a></p>");
              }else{
                client.println("        <p><a href='/d/70/on'><button style='width:25%' class='button_off'>Enero</button></a></p>");
              }
              if (bitRead(programs[programming][7], 1)){
                client.println("        <p><a href='/d/71/off'><button style='width:25%' class='button_on'>Febrero</button></a></p>");
              }else{
                client.println("        <p><a href='/d/71/on'><button style='width:25%' class='button_off'>Febrero</button></a></p>");
              }
              if (bitRead(programs[programming][7], 2)){
                client.println("        <p><a href='/d/72/off'><button style='width:25%' class='button_on'>Marzo</button></a></p>");
              }else{
                client.println("        <p><a href='/d/72/on'><button style='width:25%' class='button_off'>Marzo</button></a></p>");
              }
              if (bitRead(programs[programming][7], 3)){
                client.println("        <p><a href='/d/73/off'><button style='width:25%' class='button_on'>Abril</button></a></p>");
              }else{
                client.println("        <p><a href='/d/73/on'><button style='width:25%' class='button_off'>Abril</button></a></p>");
              }
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              if (bitRead(programs[programming][7], 4)){
                client.println("        <p><a href='/d/74/off'><button style='width:25%' class='button_on'>Mayo</button></a></p>");
              }else{
                client.println("        <p><a href='/d/74/on'><button style='width:25%' class='button_off'>Mayo</button></a></p>");
              }
              if (bitRead(programs[programming][7], 5)){
                client.println("        <p><a href='/d/75/off'><button style='width:25%' class='button_on'>Junio</button></a></p>");
              }else{
                client.println("        <p><a href='/d/75/on'><button style='width:25%' class='button_off'>Junio</button></a></p>");
              }
              if (bitRead(programs[programming][7], 6)){
                client.println("        <p><a href='/d/76/off'><button style='width:25%' class='button_on'>Julio</button></a></p>");
              }else{
                client.println("        <p><a href='/d/76/on'><button style='width:25%' class='button_off'>Julio</button></a></p>");
              }
              if (bitRead(programs[programming][7], 7)){
                client.println("        <p><a href='/d/77/off'><button style='width:25%' class='button_on'>Agosto</button></a></p>");
              }else{
                client.println("        <p><a href='/d/77/on'><button style='width:25%' class='button_off'>Agosto</button></a></p>");
              }
              client.println("      </div>");
              client.println("      <div class='btn-group' style='width:100%'>");
              if (bitRead(programs[programming][8], 0)){
                client.println("        <p><a href='/d/80/off'><button style='width:25%' class='button_on'>Septiembre</button></a></p>");
              }else{
                client.println("        <p><a href='/d/80/on'><button style='width:25%' class='button_off'>Septiembre</button></a></p>");
              }
              if (bitRead(programs[programming][8], 1)){
                client.println("        <p><a href='/d/81/off'><button style='width:25%' class='button_on'>Octubre</button></a></p>");
              }else{
                client.println("        <p><a href='/d/81/on'><button style='width:25%' class='button_off'>Octubre</button></a></p>");
              }
              if (bitRead(programs[programming][8], 2)){
                client.println("        <p><a href='/d/82/off'><button style='width:25%' class='button_on'>Noviembre</button></a></p>");
              }else{
                client.println("        <p><a href='/d/82/on'><button style='width:25%' class='button_off'>Noviembre</button></a></p>");
              }
              if (bitRead(programs[programming][8], 3)){
                client.println("        <p><a href='/d/83/off'><button style='width:25%' class='button_on'>Diciembre</button></a></p>");
              }else{
                client.println("        <p><a href='/d/83/on'><button style='width:25%' class='button_off'>Diciembre</button></a></p>");
              }
              client.println("      </div>");
              client.println("      <div class='fila'>");
              client.println("        <p><a href='/m/all/on'><button type='button' class='btn btn-default col-md-4'>Todos</button></a></p>");
              client.println("        <p><a href='/m/all/off'><button type='button' class='btn btn-default col-md-4'>Ninguno</button></a></p>");
              client.println("      </div>");
              client.println("    </div>");
              client.println("    <hr>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='fila'>");
              client.println("        <p><a href='/program'><button type='button' class='btn btn-default col-md-4'>Programar</button></a></p>");
              client.println("        <p><a href='/discard'><button type='button' class='btn btn-default col-md-4'>Descartar</button></a></p>");
              client.println("      </div>");
              client.println("    </div>");
              client.println("  </body>");
              client.println("</html>");
            }
            
            // Show the main page
            else{
              client.println("<!Doctype html>");
              client.println("<html>");
              client.println("  <head>");
              client.println("    <meta name='viewport' content='width=device-width, initial-scale=1'>");
              client.println("    <link rel='icon' href='data:,'>");
              client.println("    <style>");
              client.println("      html {");
              client.println("        font-family: Helvetica;");
              client.println("        display: inline-block;");
              client.println("        margin: 0px auto;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("      .btn-group button:not(:last-child) {");
              client.println("        border-right: none; /* Prevent double borders */");
              client.println("      }");
              client.println("      /* Clear floats (clearfix hack) */");
              client.println("      .btn-group:after {");
              client.println("        content: "";");
              client.println("        clear: both;");
              client.println("        display: table;");
              client.println("      }");
              client.println("      .btn-group button {");
              client.println("        padding: 5px 8px;");
              client.println("        font-size: 12px;");
              client.println("      }");
              client.println("      .button_on {");
              client.println("        background-color: #4CAF50;");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("      .button_off {");
              client.println("        background-color: #F44336;");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("      .button_blocked {");
              client.println("        background-color: #616161;");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("      .fila button {");
              client.println("        padding: 5px 8px;");
              client.println("        font-size: 12px;");
              client.println("      }");
              client.println("      .fila > * {");
              client.println("        display: inline-block;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("      .matriz {");
              client.println("        padding:5px;");
              client.println("        text-align: center;");
              client.println("      }");
              client.println("      .btn btn-default col-md-4 {");
              client.println("        border: none;");
              client.println("        color: white;");
              client.println("        padding: 2px 5px;");
              client.println("        text-decoration: none;");
              client.println("        font-size: 12px;");
              client.println("        margin: 0px;");
              client.println("        cursor: pointer;");
              client.println("        float: left;");
              client.println("      }");
              client.println("    </style>");
              client.println("  </head>");
              client.println("  <body>");
              client.println("    <h1 style='text-align: center;'>TFG Web Server</h1>");
              client.println("    <h2 style='text-align: center;'>Panel de control de las salidas</h2>");
              client.println("    <p><a href='/sincronize'><button class='btn btn-default col-md-4'>Sincronizar</button></a></p>");

              char time_buffer[150] = " ";
              char* formato = "<p style='font-size: 14px; width:100%%; height:5px;'>%04d/%02d/%02d - %02d:%02d:%02d</p>";
              sprintf(time_buffer, formato, time_year, time_month, time_day, time_hour, time_min, time_seg);
              client.println(time_buffer);
              
              client.println("    <div class='fila'>");
              client.println("      <div class='btn-group' style='border-style:ridge;'>");
              client.println("        <p style='font-size: 14px; width:100%; height:5px;'>Temperatura Rel&eacute;s</p>");
              
              char html_line1[150]=" ";
              char* formato1 = "<p style='font-size: 14px; width:100%%; height:5px;'>%.1f</p>";
              sprintf(html_line1, formato1, temperature1);
              client.println(html_line1);
              
              client.println("      </div>");
              client.println("      <div class='btn-group' style='border-style:ridge;'>");
              client.println("        <p style='font-size: 14px; width:100%; height:5px;'>Temperatura Sonda</p>");
              
              char html_line2[150]=" ";
              char* formato2 = "<p style='font-size: 14px; width:100%%; height:5px;'>%.1f</p>";
              sprintf(html_line2, formato2, temperature2);
              client.println(html_line2);
      
              client.println("      </div>");
              client.println("    </div>");

              char html_line3[150]=" ";
              if (rssi == 0){
                client.println("<p style='font-size: 14px'>RSSI: 0</p>");
              }else if (rssi > -70){
                char* formato3 = "<p style='font-size: 14px; background-color:green; color: white'>RSSI: %i</p>";
                sprintf(html_line3, formato3, rssi);
                client.println(html_line3);
              }else if (rssi > -85){
                char* formato3 = "<p style='font-size: 14px; background-color:yellow'>RSSI: %i</p>";
                sprintf(html_line3, formato3, rssi);
                client.println(html_line3);
              }else if (rssi > -100){
                char* formato3 = "<p style='font-size: 14px; background-color:red'>RSSI: %i</p>";
                sprintf(html_line3, formato3, rssi);
                client.println(html_line3);
              }else if (rssi > -150){
                char* formato3 = "<p style='font-size: 14px; background-color:black; color: white;'>RSSI: %i</p>";
                sprintf(html_line3, formato3, rssi);
                client.println(html_line3);
              }
              
              client.println("    <hr>");
              client.println("    <h3 style='text-align: center;'>Rel&eacute;s de baja potencia</h3>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='fila'>");
              print_outputs(client, 0);
              print_outputs(client, 1);
              client.println("      </div>");
              client.println("      <div class='fila'>");
              print_outputs(client, 2);
              print_outputs(client, 3);
              client.println("      </div>");
              client.println("      <div class='fila'>");
              print_outputs(client, 4);
              print_outputs(client, 5);
              client.println("      </div>");
              client.println("    </div>");
              client.println("    <hr>");
              client.println("    <h3 style='text-align: center;'>Rel&eacute;s para motores</h3>");
              client.println("    <div class='matriz'>");
              client.println("      <div class='fila'>");
              print_outputs(client, 6);
              print_outputs(client, 7);
              client.println("      </div>");
              client.println("    </div>");
              client.println("  </body>");
              client.println("</html>");
            }



            
            // The HTTP response ends with another blank line
            client.println();
            // Break out of the while loop
            break;
          } else { // if you got a newline, then clear currentLine
            currentLine = "";
          }
        } else if (c != '\r') {  // if you got anything else but a carriage return character,
          currentLine += c;      // add it to the end of the currentLine
        }
      }
    }
    // Clear the header variable
    header = "";
    // Close the connection
    client.stop();
    Serial.println("Client disconnected.");
    Serial.println("");
  }
  update_outputs();
}
