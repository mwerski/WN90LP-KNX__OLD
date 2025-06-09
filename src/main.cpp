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
void MQTTpublish();
void sendTemperature();
void sendHumidity();
//Task task_updatePressureRingbuffer(RINGBUFFER_UPDATE, TASK_FOREVER, &updatePressureRingbuffer);	// update ringbuffer for trends of pressure readings
Task task_MQTTpublish(10000, TASK_FOREVER, &MQTTpublish);
Task task_sendTemperature(60000, TASK_FOREVER, &sendTemperature);
//Task task_sendHumidity(60000, TASK_FOREVER, &sendHumidity);
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

void callback_Temperature(GroupObject& go) {
	sendTemperature();
}

/*
char* hostname = "wn90";
char* ip_addr;
char* mqtt_server = "broker.localnet";
char *mqtt_username = "";
char *mqtt_password = "";
*/

u_int8_t	lastHour = NAN;	// last hour (if this changes, a full hour has passed)

WebServer server(80);

Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Modbus
ModbusMaster node;
bool RS485mode = true;
void RS485_Mode(int Mode);
void RS485_TX();
void RS485_RX();

struct netconfig {
	IPAddress ip;
	IPAddress gateway;
	IPAddress netmask;
	IPAddress dns;
	bool dhcp = true;
	char*	hostname;
	bool mqtt = false;
	char* mqttHost;
	u_int16_t mqttPort = 1883;
	char* mqttUser;
	char* mqttPass;
	char* mqttTopic;
	u_int16_t mqttFreq;
};
netconfig net;

#define RINGBUFFERSIZE 12
u_int8_t lasthour = NAN;	// last ringbuffer update hour
u_int8_t lastminute = NAN; // last ringbuffer update minute
int16_t pressureRing[RINGBUFFERSIZE];	// Ringbuffer for calculating pressure tendencies
#pragma region Weather data structures and variables
struct wsdata_float {
	double value;
	bool read = false;
	double last; // value that was last sent to the bus
	bool lastread = false;
};
struct wsdata_integer {
	int32_t value;
	bool read = false;
	int32_t last; // value that was last sent to the bus
	bool lastread = false;
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

#pragma region MQTT
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Json mqttMsg;
void mqtt_reconnect() {
	Serial.print("Attempting MQTT connection...");
	// Attempt to connect
	String client_id = net.hostname + String(WiFi.macAddress());
	if (mqttClient.connect(client_id.c_str(), net.mqttUser, net.mqttPass)) {
		Serial.println("connected");
	} else {
		Serial.print("failed, rc=");
		Serial.print(mqttClient.state());
		Serial.println(" try again next time");
	}
}
#pragma endregion


double dewpoint(float t, float f) {
	// calculates dewpoint from given temperature and relative hjmidity
	float a, b;
  if (t >= 0) {
    a = 7.5;
    b = 237.3;
  } else {
    a = 7.6;
    b = 240.7;
  }
  double sdd = 6.1078 * pow(10, (a*t)/(b+t));  // Sättigungsdampfdruck in hPa
  double dd = sdd * (f/100);  // Dampfdruck in mBar
  double v = log10(dd/6.1078);  // v-Parameter
  double dp = (b*v) / (a-v);  // Taupunkttemperatur (°C)
//	return dp;
	double m = powf( 10.0f, 2 ); // truncate to x.yz
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

	#pragma region KNX stuff
	knx.buttonPin(5);

	// read adress table, association table, groupobject table and parameters from eeprom
	knx.readMemory();

	Serial.println("Starting up...");
	Serial.print("KNX configured: ");
	Serial.println(knx.configured());
	knx.setProgLedOffCallback(progLedOff);
	knx.setProgLedOnCallback(progLedOn);

	if (knx.configured()) {
		if ( ParamAPP_UseDHCP == false ) {
			// we are using a static IP config stored as KNX paramaeters
			net.dhcp = false;
			// convert IPs from little to big endian
			net.ip = IPAddress((ParamAPP_IP & 0xFF) << 24) | ((ParamAPP_IP & 0xFF00) << 8) | ((ParamAPP_IP & 0xFF0000) >> 8) | ((ParamAPP_IP & 0xFF000000) >> 24);
			net.netmask = IPAddress((ParamAPP_Netzmaske & 0xFF) << 24) | ((ParamAPP_Netzmaske & 0xFF00) << 8) | ((ParamAPP_Netzmaske & 0xFF0000) >> 8) | ((ParamAPP_Netzmaske & 0xFF000000) >> 24);
			net.gateway = IPAddress((ParamAPP_Gateway & 0xFF) << 24) | ((ParamAPP_Gateway & 0xFF00) << 8) | ((ParamAPP_Gateway & 0xFF0000) >> 8) | ((ParamAPP_Gateway & 0xFF000000) >> 24);
			net.dns = IPAddress((ParamAPP_DNS & 0xFF) << 24) | ((ParamAPP_DNS & 0xFF00) << 8) | ((ParamAPP_DNS & 0xFF0000) >> 8) | ((ParamAPP_DNS & 0xFF000000) >> 24);
			Serial.println("Static network configuration:");
			Serial.print("IP....... "); Serial.println(net.ip);
			Serial.print("Netmask.. "); Serial.println(net.netmask);
			Serial.print("Gateway.. "); Serial.println(net.gateway);
			Serial.print("DNS...... "); Serial.println(net.dns);
		} else {
			net.dhcp = true;
		}
		net.hostname = (char *) ParamAPP_Hostname;
		net.mqtt = ParamAPP_useMQTT;
		if ( net.mqtt == true ) {
			Serial.println("MQTT configured");
			// convert data paraeters to char arrays
			net.mqttHost = (char *) ParamAPP_MQTT_Host;
			Serial.print("Broker... "); Serial.println(net.mqttHost);
			net.mqttUser = (char *) ParamAPP_MQTT_User;
			net.mqttPass = (char *) ParamAPP_MQTT_Password;
			net.mqttTopic = (char *) ParamAPP_MQTT_Topic;
			net.mqttPort = ParamAPP_MQTT_Port;
			net.mqttFreq = ParamAPP_MQTT_Frequency;
			Serial.print("Broker..... "); Serial.print(net.mqttHost); Serial.print(":"); Serial.println(net.mqttPort);
			Serial.print("User....... "); Serial.println(net.mqttUser);
			Serial.print("Topic...... "); Serial.println(net.mqttTopic);
			Serial.print("Frequency.. "); Serial.println(net.mqttFreq);
		} else {
			Serial.println("MQTT disabled");
		}

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

		switch ( ParamAPP_Temperatur_DPT ) {
			case 0: 
				KoAPP_Temperatur_DPT9.dataPointType(Dpt(9, 1));
				KoAPP_Temperatur_DPT9.callback(callback_Temperature);
				break;
			case 1:
				KoAPP_Temperatur_DPT14.dataPointType(Dpt(14, 1));
				KoAPP_Temperatur_DPT14.callback(callback_Temperature);
				break;
		}

		// convert Params to char arrays
//		ip_addr   = (char *) ParamAPP_IP;
//		mqtt_host = (char *) ParamAPP_MQTT_Host;
//		mqtt_user = (char *) ParamAPP_MQTT_Host;
//		mqtt_pass = (char *) ParamAPP_MQTT_Host;
//		char mqtth[32];
//		char* mqtth = (char*) ParamAPP_MQTT_Host;
		//uint8_t* mqtth[32];
		//mqtth = ParamAPP_MQTT_Host;
		//	mqtth=knx.paramData(ParamAPP_MQTT_Host,32);
//		Serial.print("MQTT Host: "); Serial.println( mqtt_host);
//		Serial.printf("Text: %s\n", (char*)mqtth);
//		Serial.printf("[%u] get Text: %s\n", num, payload);


	}
	#pragma endregion


  //WiFiManager
  WiFiManager wifiManager;
  //wifiManager.resetSettings();

	if ( net.dhcp == false ) {
		wifiManager.setSTAStaticIPConfig( net.ip, net.gateway, net.netmask, net.dns );
		Serial.print("static setup: ");
	}

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
	ArduinoOTA.setHostname(net.hostname);
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
	mqttClient.setServer(net.mqttHost, net.mqttPort);
	

	

	// initialize ringbuffer
	for (u_int8_t i=0; i<RINGBUFFERSIZE; i++) { pressureRing[i] = NAN; }

	Serial.println("Initializing scheduler");
	runner.init();

	if ( net.mqtt == true ) {
		runner.addTask(task_MQTTpublish);
		task_MQTTpublish.setInterval(net.mqttFreq*1000);
		task_MQTTpublish.enable();
	}
	if (ParamAPP_Temperatur_Senden_zyklisch > 0) {
		Serial.print("Send temperature values every "); Serial.print(ParamAPP_Temperatur_Senden_zyklisch); Serial.println("s");
		runner.addTask(task_sendTemperature);
		task_sendTemperature.setInterval(ParamAPP_Temperatur_Senden_zyklisch*1000);
		task_sendTemperature.enable();
	}
//	runner.addTask(task_updatePressureRingbuffer);

	// define callbacks
/*	if (ParamAPP_Temperatur_DPT == 0) {
		// DPT9
		KoAPP_Temperatur_DPT9.callback(callback_sendTemperature);
	} else {
		// DPT14
		KoAPP_Temperatur_DPT14.callback(sendTemperature));
	} */

	knx.start();
	#pragma region KNX Request Time
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
		Serial.printf("Temperature.... %0.1f (%0.1f) °C\n", temperature.value, temperature.last);
		Serial.printf("Dewpoint....... %0.2f (%0.2f) °C\n", dewPoint.value, dewPoint.last);
		Serial.printf("Humidity....... %d (%d) %%\n", humidity.value, humidity.last);
		Serial.printf("Wind Speed..... %0.2f (%0.2f) m/s\n", windSpeed.value, windSpeed.last);
		Serial.printf("Gust Speed..... %0.2f (%0.2f) m/s\n", gustSpeed.value, gustSpeed.last);
		Serial.printf("Wind Direction. %d (%d) degreee\n", windDirection.value, windDirection.last);
		Serial.printf("Pressure....... %0.1f (%0.1f) hPa\n", pressure.value, pressure.last);
		if ( pressureTrend1.read ) Serial.printf("Trend (1h)..... %df (%d)\n", pressureTrend1.value, pressureTrend1.last);
		if ( pressureTrend3.read ) Serial.printf("Trend (3h)..... %df (%d)\n", pressureTrend3.value, pressureTrend3.last);
		Serial.printf("Rainfall....... %0.1f (%0.1f) mm\n", rainFall.value, rainFall.last);
		Serial.printf("Raincounter.... %0.2f (%0.2f) mm\n", rainCounter.value, rainCounter.last);
		Serial.printf("Brightness..... %d (%d) mm\n", light.value, light.last);
		Serial.printf("UV Index....... %0.1f (%0.1f) mm\n", uvIndex.value, uvIndex.last);
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

#pragma region KNX functions
void sendTemperature() {
	if (temperature.read) {
		Serial.print("Sending temperature ("); Serial.print(temperature.value, 2); Serial.println(") to bus");
		switch (ParamAPP_Temperatur_DPT) {
			case 0: // DPT9
				KoAPP_Temperatur_DPT9.value(temperature.value); break;
			case 1: // DPT14
				KoAPP_Temperatur_DPT14.value(temperature.value); break;
		}
		temperature.last = temperature.value;
	} else {
		Serial.println("Temperature not yet available, won't send to bus - delay task by 3s");
		task_sendTemperature.delay(3000);
	}
	Serial.println();
}

void MQTTpublish() {
	if ( temperature.read == true ) mqttMsg.add("temperature", temperature.value );
	if ( humidity.read == true ) mqttMsg.add("humidity", humidity.value );
	if ( dewPoint.read == true ) mqttMsg.add("dewpoint", dewPoint.value );
	if ( pressure.read == true ) mqttMsg.add("pressure", pressure.value );
	if ( pressureTrend1.read == true ) mqttMsg.add("pressuretrend1", pressureTrend1.value );
	if ( pressureTrend3.read == true ) mqttMsg.add("pressuretrend3", pressureTrend3.value );
	if ( humidity.read == true ) mqttMsg.add("humidity", humidity.value );
	if ( light.read == true ) mqttMsg.add("light", light.value );
	if ( uvIndex.read == true ) mqttMsg.add("uvindex", uvIndex.value );
	if ( windSpeed.read == true ) mqttMsg.add("windspeed", windSpeed.value );
	if ( gustSpeed.read == true ) mqttMsg.add("gustspeed", gustSpeed.value );
	if ( windDirection.read == true ) mqttMsg.add("winddirection", windDirection.value );
	if ( rainFall.read == true ) mqttMsg.add("rainfall", rainFall.value );
	if ( rainCounter.read == true ) mqttMsg.add("raincounter", rainCounter.value );

	if (!mqttClient.connected()) {
	mqtt_reconnect();
	}

	if (mqttClient.connected()) {
		String msg = mqttMsg.toString();
		uint16_t msgLength = msg.length() + 1;
		if (msgLength > 10) {
			// only publish if messgae is not empty
			Serial.println("Publish results to MQTT broker");
			char msgChar[msgLength];
			msg.toCharArray(msgChar, msgLength);
			mqttClient.publish(net.mqttTopic, msgChar );
		}
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
