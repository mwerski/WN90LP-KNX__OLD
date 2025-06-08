#include <Arduino.h>
#include "utilities.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <Adafruit_NeoPixel.h>
#include <ModbusMaster.h>
#include <PubSubClient.h>
#include <Json.h>
#include "knxprod.h"
#include <knx.h>
#include <RemoteDebug.h>
#include <TimeLib.h>

#define DEBUG_DISABLE_DEBUGGER true	// Debug Optionen in SerialDebug deaktivieren
#define DEBUG_INITIAL_LEVEL DEBUG_LEVEL_VERBOSE	// Default Debug Level

#pragma region Scheduler definitions and prototypes
#include <TaskScheduler.h>
//Task task_updatePressureRingbuffer(RINGBUFFER_UPDATE, TASK_FOREVER, &updatePressureRingbuffer);	// update ringbuffer for trends of pressure readings
Scheduler runner;
#pragma endregion

struct tm myTime;
bool timeKnown = false;
bool dateKnown = false;

void timeCallback(GroupObject& go) {
	if (go.value()) {
		timeKnown = true;
		myTime = KoAPP_Time.value();
		unsigned short tmp_sec = myTime.tm_sec;
		unsigned short tmp_min = myTime.tm_min;
		unsigned short tmp_hour = myTime.tm_hour;
		char buf[52];
		sprintf(buf, "Time received from bus: %02d:%02d:%02d", tmp_hour, tmp_min, tmp_sec );
		Serial.println(buf);
		time_t t = now();
		setTime(tmp_hour, tmp_min, tmp_sec, day(t), month(t), year(t));
		if (dateKnown == true) {
			sprintf(buf, "Setting/Adjusting system time: %d.%d.%d, %02d:%02d:%02d", day(t), month(t), year(t), tmp_hour, tmp_min, tmp_sec );
			Serial.println(buf);
		}
	}
}

void dateCallback(GroupObject& go) {
	if (go.value()) {
		dateKnown = true;
		myTime = KoAPP_Date.value();
		unsigned short tmp_mday = myTime.tm_mday;
		unsigned short tmp_mon = myTime.tm_mon;
		unsigned short tmp_year = myTime.tm_year;
		char buf[52];
		sprintf(buf, "Date received from bus: %d.%d.%d", tmp_mday, tmp_mon, tmp_year );
		Serial.println(buf);
		time_t t = now();
		setTime(hour(t), minute(t), second(t), tmp_mday, tmp_mon, tmp_year);
		if (timeKnown == true) {
			sprintf(buf, "Setting/Adjusting system time: %d.%d.%d, %02d:%02d:%02d", tmp_mday, tmp_mon, tmp_year, hour(t), minute(t), second(t) );
			Serial.println(buf);
		}
	}
}

void dateTimeCallback(GroupObject& go) {
	// Untested
	if (go.value()) {
		dateKnown = true;
		timeKnown = true;
		myTime = KoAPP_Date.value();
		unsigned short tmp_sec = myTime.tm_sec;
		unsigned short tmp_min = myTime.tm_min;
		unsigned short tmp_hour = myTime.tm_hour;
		unsigned short tmp_mday = myTime.tm_mday;
		unsigned short tmp_mon = myTime.tm_mon;
		unsigned short tmp_year = myTime.tm_year;
		char buf[52];
		sprintf(buf, "DateTime received from bus: %d.%d.%d, %02d:%02d:%02d", tmp_mday, tmp_mon, tmp_year, tmp_hour, tmp_min, tmp_sec );
		Serial.println(buf);
		Serial.println("Setting/Adjusting system time");
		setTime(tmp_hour, tmp_min, tmp_sec, tmp_mday, tmp_mon, tmp_year);
	}
}



const char* hostname = "wn90";
const char* mqtt_server = "broker.localnet";
const char *mqtt_username = "";
const char *mqtt_password = "";
u_int8_t	lastHour = NAN;	// last hour (if this changes, a full hour has passed)

WebServer server(80);

// MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Json mqttMsg;
void mqtt_reconnect() {
	Serial.print("Attempting MQTT connection...");
	// Attempt to connect
	if (mqttClient.connect(hostname, mqtt_username, mqtt_password)) {
		Serial.println("connected");
	} else {
		Serial.print("failed, rc=");
		Serial.print(mqttClient.state());
		Serial.println(" try again next time");
	}
}

Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Modbus
ModbusMaster node;
bool RS485mode = true;
void RS485_Mode(int Mode);
void RS485_TX();
void RS485_RX();

#define RINGBUFFERSIZE 12
u_int8_t lasthour = NAN;	// last ringbuffer update hour
u_int8_t lastminute = NAN; // last ringbuffer update minute
int16_t pressureRing[RINGBUFFERSIZE];	// Ringbuffer for calculating pressure tendencies
#pragma region Weather data structures and variables
struct wsdata_float {
	double value;
	bool read = false;
};
struct wsdata_integer {
	int32_t value;
	bool read = false;
};
wsdata_float uvIndex; // UV index
wsdata_integer light; // brightness in Lux
wsdata_float temperature; // temperature in degree celsius
wsdata_integer humidity; // relative humidity in percent
wsdata_float windSpeed; // windspeed in m/s
wsdata_float gustSpeed; // gust speed in m/s
wsdata_integer windDirection; // wind direction in degrees
wsdata_float pressure; // absolute pressure in hPa / mBar
wsdata_integer pressureTrend1; // gets measured and updated every 15 minutes, shows pressure differences from now to -1 hour, available after ~1 hour uptime
wsdata_integer pressureTrend3; // gets measured every full hour, shows pressure differences from now to -3 hours, available after ~3 hours uptime
wsdata_float rainFall; // rainfall in mm / liter - 0.1mm resolution
wsdata_float rainCounter; // rainfall in mm / liter - 0.01mm resolution
wsdata_float dewPoint; // dewpoint in degree celsius
#pragma endregion

#pragma region KNX stuff
char* mqtt_host;
char* mqtt_user;
char* mqtt_pass;
#pragma endregion


float dewpoint(float t, float f) {
	// calculates dewpoint from given temperature and relative hjmidity
	float a, b;
  if (t >= 0) {
    a = 7.5;
    b = 237.3;
  } else {
    a = 7.6;
    b = 240.7;
  }
  float sdd = 6.1078 * pow(10, (a*t)/(b+t));  // Sättigungsdampfdruck in hPa
  float dd = sdd * (f/100);  // Dampfdruck in mBar
  float v = log10(dd/6.1078);  // v-Parameter
  float dp = (b*v) / (a-v);  // Taupunkttemperatur (°C)
//	return dp;
	float m = powf( 10.0f, 2 ); // truncate to x.yz
	dp = roundf( dp * m ) / m;
	return dp;
}

void progLedOff() {
	pixels.clear();
	pixels.show();
}

void progLedOn() {
	pixels.setPixelColor(0, pixels.Color(20, 0, 0));
	pixels.show();
}

void setup() {
	Serial.begin(115200);

	pinMode(RS485_CON_PIN, OUTPUT);
	pinMode(KEY_PIN, INPUT_PULLUP);
	Serial485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, true);
	// communicate with Modbus slave ID 144 over Serial (port 0)
  node.preTransmission(RS485_TX);
  node.postTransmission(RS485_RX);
  node.begin(0x90, Serial485);
	RS485_Mode(RS485_TX_ENABLE);
	delay(20);


  //WiFiManager
  WiFiManager wifiManager;
  //wifiManager.resetSettings();
  wifiManager.setConfigPortalTimeout(180);
	if (!wifiManager.autoConnect("AutoConnectAP")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
    delay(5000);
  } 

	Serial.print("IP Address: ");
	Serial.println(WiFi.localIP());

	// Arduino OTA on üport 3232
	// ArduinoOTA.setPort(3232);
	ArduinoOTA.setHostname(hostname);
	// ArduinoOTA.setPassword("admin");
	// Password can be set with it's md5 value as well
	// MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
	// ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");
	ArduinoOTA
		.onStart([]() {
			String type;
			if (ArduinoOTA.getCommand() == U_FLASH) {
				type = "sketch";
			} else {  // U_SPIFFS
				type = "filesystem";
			}

			// NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
			Serial.println("Start updating " + type);
		})
		.onEnd([]() {
			Serial.println("\nEnd");
		})
		.onProgress([](unsigned int progress, unsigned int total) {
			Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
		})
		.onError([](ota_error_t error) {
			Serial.printf("Error[%u]: ", error);
			if (error == OTA_AUTH_ERROR) {
				Serial.println("Auth Failed");
			} else if (error == OTA_BEGIN_ERROR) {
				Serial.println("Begin Failed");
			} else if (error == OTA_CONNECT_ERROR) {
				Serial.println("Connect Failed");
			} else if (error == OTA_RECEIVE_ERROR) {
				Serial.println("Receive Failed");
			} else if (error == OTA_END_ERROR) {
				Serial.println("End Failed");
			}
		});
	ArduinoOTA.begin();


/*		// Initialize mDNS
		if (!MDNS.begin("wl90")) {   // Set the hostname to "wl90.local"
			Serial.println("Error setting up MDNS responder!");
			while(1) {
				delay(1000);
			}
		}
		Serial.println("mDNS responder started");
*/



	server.begin();

	ElegantOTA.begin(&server);

	// MQTT
	mqttClient.setServer(mqtt_server, 1883);
	

	// KNX stuff
	knx.buttonPin(5);
	//knx.ledPin(15);
	//knx.ledPinActiveOn(HIGH);

	// read adress table, association table, groupobject table and parameters from eeprom
	knx.readMemory();

	delay(5000);
	Serial.println("Starting up...");
	Serial.print("KNX configured: ");
	Serial.println(knx.configured());
	knx.setProgLedOffCallback(progLedOff);
	knx.setProgLedOnCallback(progLedOn);

	if (knx.configured()) {
		if (ParamAPP_Heartbeat > 0) {
			Serial.print("Sende Neartbeat alle "); Serial.print(ParamAPP_Heartbeat); Serial.println("s");
			// ParamAPP_Heartbeat=10:
			KoAPP_Heartbeat.dataPointType(Dpt(1, 1));
			//runner.addTask(task_heartbeat);
			//task_heartbeat.setInterval(ParamAPP_Heartbeat*1000);
			//task_heartbeat.enable();
			Serial.println("Task(s) enabled");
		} else {
			Serial.println("Sende keinen Heartbeat");
		}

		// convert Params to char arrays
		mqtt_host = (char *) ParamAPP_MQTT_Host;
		mqtt_user = (char *) ParamAPP_MQTT_Host;
		mqtt_pass = (char *) ParamAPP_MQTT_Host;
//		char mqtth[32];
//		char* mqtth = (char*) ParamAPP_MQTT_Host;
		//uint8_t* mqtth[32];
		//mqtth = ParamAPP_MQTT_Host;
		//	mqtth=knx.paramData(ParamAPP_MQTT_Host,32);
		Serial.print("MQTT Host: "); Serial.println( mqtt_host);
//		Serial.printf("Text: %s\n", (char*)mqtth);
//		Serial.printf("[%u] get Text: %s\n", num, payload);

		#pragma region KNX Time
		if (ParamAPP_DateTime_DPTs == 1) {
			Serial.println("Receive time and date from different KOs, registering callbacks");
			KoAPP_Time.dataPointType(DPT_TimeOfDay);
			KoAPP_Time.callback(timeCallback);
			KoAPP_Date.dataPointType(DPT_Date);
			KoAPP_Date.callback(dateCallback);
			if (ParamAPP_Uhrzeit_beim_Start_lesen == 1) {
				Serial.println("Reading time and date from Bus");
				KoAPP_Time.requestObjectRead();
				KoAPP_Date.requestObjectRead();
			}
		} else {
			Serial.println("Receive time and date from a single KO, registering callback");
			KoAPP_DateTime.dataPointType(DPT_DateTime);
			KoAPP_DateTime.callback(dateTimeCallback);
			if (ParamAPP_Uhrzeit_beim_Start_lesen == 1) {
				Serial.println("Reading time and date from Bus");
				KoAPP_DateTime.requestObjectRead();
			}
		}
		#pragma endregion

	}
	knx.start();

	// initialize ringbuffer
	for (u_int8_t i=0; i<RINGBUFFERSIZE; i++) { pressureRing[i] = NAN; }

	Serial.println("Initializing scheduler");
	runner.init();
//	runner.addTask(task_updatePressureRingbuffer);

}

void printLocaltime(bool newline=false) {
	time_t t = now();
	char buf[20];
	sprintf(buf, "%d.%d.%d, %02d:%02d:%02d", day(t), month(t), year(t), hour(t), minute(t), second(t));
	Serial.print(buf);
	if (newline == true) Serial.println();
}

uint8_t read_ws90() {
	Serial.println("Read weather station data");
	uint8_t c=10;
	uint8_t result = node.readHoldingRegisters( 0x165, c );
	Serial.print("result = ");
	Serial.println( result );

	if (result == node.ku8MBSuccess) {
		if ( node.getResponseBuffer(0) != 0xFFFF ) { light.value = node.getResponseBuffer(0) * 10; light.read = true; }
		if ( node.getResponseBuffer(1) != 0xFFFF ) { uvIndex.value = node.getResponseBuffer(1) / 10.0; uvIndex.read = true; }
		if ( node.getResponseBuffer(2) != 0xFFFF ) { temperature.value = node.getResponseBuffer(2) / 10.0 - 40; temperature.read = true; }
		if ( node.getResponseBuffer(3) != 0xFFFF ) { humidity.value = node.getResponseBuffer(3); humidity.read = true; }
		if ( node.getResponseBuffer(4) != 0xFFFF ) { windSpeed.value = node.getResponseBuffer(4) / 10.0; windSpeed.read = true; }
		if ( node.getResponseBuffer(5) != 0xFFFF ) { gustSpeed.value = node.getResponseBuffer(5) / 10.0; gustSpeed.read = true; }
		if ( node.getResponseBuffer(6) != 0xFFFF ) { windDirection.value = node.getResponseBuffer(6); windDirection.read = true; }
		if ( node.getResponseBuffer(7) != 0xFFFF ) { rainFall.value = node.getResponseBuffer(7) / 10.0; rainFall.read = true; }
		if ( node.getResponseBuffer(8) != 0xFFFF ) { pressure.value = node.getResponseBuffer(8) / 10.0; pressure.read = true; }
		if ( node.getResponseBuffer(9) != 0xFFFF ) { rainCounter.value = node.getResponseBuffer(9) / 100.0; rainCounter.read = true; }
		if ( temperature.read && humidity.read ) { dewPoint.value = dewpoint (temperature.value, humidity.value); dewPoint.read = true; }

		Serial.print("Localtime...... "); printLocaltime(true);
		Serial.print("Light.......... "); Serial.print(light.value ); Serial.println(" Lux");
		Serial.print("UVI............ "); Serial.print(uvIndex.value , 1); Serial.println("");
		Serial.print("Temperature.... "); Serial.print(temperature.value, 1); Serial.println(" °C");
		Serial.print("Humidity....... "); Serial.print(humidity.value); Serial.println(" %");
		Serial.print("Wind Speed..... "); Serial.print(windSpeed.value, 1); Serial.println(" m/s");
		Serial.print("Gust Speed..... "); Serial.print(gustSpeed.value , 1); Serial.println(" m/s");
		Serial.print("Wind Direction. "); Serial.print(windDirection.value); Serial.println(" °");
		Serial.print("Rainfall....... "); Serial.print(rainFall.value , 1); Serial.println(" mm");
		Serial.print("ABS Pressure... "); Serial.print(pressure.value , 1); Serial.println(" mbar");
		Serial.print("Rain Counter... "); Serial.print(rainCounter.value, 2); Serial.println(" mm");
		Serial.print("Dewpoint....... "); Serial.print(dewPoint.value, 2); Serial.println(" °C");
		if ( pressureTrend1.read ) {
			Serial.print("Trend (1h)..... "); Serial.println(pressureTrend1.value);
		}
		if ( pressureTrend3.read ) {
			Serial.print("Trend (3h)..... "); Serial.println(pressureTrend3.value);
		}
	}
	return result;
}

void RS485_Mode(int Mode) {
	digitalWrite(RS485_CON_PIN, Mode);
}

void RS485_TX() {
	RS485_Mode(RS485_TX_ENABLE);
}

void RS485_RX() {
	RS485_Mode(RS485_RX_ENABLE);
}

void updatePressureRingbuffer() {
	Serial.print("Update ringbuffer: ");
	for (u_int8_t i=1; i<RINGBUFFERSIZE; i++) {
		if ( pressureRing[i] > 0 ) pressureRing[i-1]=pressureRing[i];
		Serial.print(pressureRing[i]); Serial.print(", ");
	}
	pressureRing[RINGBUFFERSIZE-1] = pressure.value*10;
	Serial.println(pressureRing[RINGBUFFERSIZE-1]);
}

u_int8_t publish() {
	if (light.read) mqttMsg.add("light", light.value );
/*	if (uvIndex != NAN) { mqttMsg.add("uvIndex", uvIndex ); }
	if (temperature != NAN) { mqttMsg.add("temperature", temperature ); }
	if (humidity != NAN) { mqttMsg.add("humidity", humidity ); }
	if (windSpeed != NAN) { mqttMsg.add("windSpeed", windSpeed ); }
	if (gustSpeed != NAN) { mqttMsg.add("gustSpeed", gustSpeed ); }
	if (windDirection != NAN) { mqttMsg.add("windDirection", windDirection ); }
	if (rainfall != NAN) { mqttMsg.add("rainFall", rainfall ); }
	if (absPressure != NAN) { mqttMsg.add("pressure", absPressure ); }
	if (rainCounter != NAN) { mqttMsg.add("rainCounter", rainCounter ); }
	if (dewPoint != NAN) { mqttMsg.add("dewPoint", dewPoint ); }
*/
	if (!mqttClient.connected()) {
	mqtt_reconnect();
	}

	if (mqttClient.connected()) {
	String msg = mqttMsg.toString();
	uint16_t msgLength = msg.length() + 1;
	char msgChar[msgLength];
	msg.toCharArray(msgChar, msgLength);
	mqttClient.publish("environmental/wn90", msgChar );
	}

	Serial.println();
}

unsigned long lastChange = 0;
unsigned long delayTime  = 5000;

void loop() {
	time_t t = now();
	server.handleClient();
	ArduinoOTA.handle();
	ElegantOTA.loop();
	runner.execute();
	knx.loop();
	if(!knx.configured()) return;


	if (millis()-lastChange >= delayTime) {
		lastChange = millis();
		read_ws90();



	}

	if ( (minute(t) != lastminute) && (minute(t) % 15 == 0) ) {   // every 15 minutes
		updatePressureRingbuffer();
		lastminute = minute(t);
		// every 15 minutes
		if ( pressureRing[RINGBUFFERSIZE-4] > 0) { pressureTrend1.value = pressureRing[RINGBUFFERSIZE-1] - pressureRing[RINGBUFFERSIZE-4]; pressureTrend1.read = true; } else { pressureTrend1.read = false; }
		// every full hour
		if ( lastminute == 0 && pressureRing[0] > 0 ) { pressureTrend3.value = pressureRing[RINGBUFFERSIZE-1] - pressureRing[0]; pressureTrend3.read = true; }; // else { pressureTrend3.read = false; }
		Serial.println();
	}




}
