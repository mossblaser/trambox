#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <jsmn.h>

#include "metrolink.h"

#define PWM_MAX = 107
#define VALUE_MAX = 10

int PWM_VALUES[] = {
	0, // 0
	6, // 1
	13, // 2
	20, // 3
	27, // 4
	34, // 5
	44, // 6
	56, // 7
	70, // 8
	87, // 9
	106, // 10
};

int MAX_VALUE = sizeof(PWM_VALUES)/sizeof(PWM_VALUES[0]) - 1;

const char *wifiSSID = "name";
const char *wifiPassword = "xxx";
const char *tfgmApiKey = "zzz";
const char *tfgmApiHttpHost = "metrolink.jhnet.co.uk";
const char *tfgmApiPath = "/odata/Metrolinks";

void setupWifi() {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(wifiSSID);

  WiFi.begin(wifiSSID, wifiPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}



int pin = D1;
int val = 0;

void setup() {
	Serial.begin(9600);
	
	setupWifi();
	
	// Hack: By setting this as an input we use the internal pull-up reisitor
	// (enabled/disabled when we write to the pin) to limit current into the
	// capacitor/display!
	pinMode(pin, INPUT);
	analogWrite(pin, val);
	
	metrolink_init();
	metrolink_set_journey("MediaCityUK", "Piccadilly");
}

void loop() {
	//int c = Serial.read();
	//if (c > 0) {
	//	if (c == '-') {
	//		if (val > 0) {
	//			val--;
	//		}
	//	} else {
	//		if (val < 1023) {
	//			val++;
	//		}
	//	}
	//	Serial.println(val);
	//	analogWrite(pin, val);
	//}
	
	Serial.println("HTTP Connection attempt...");
	
	WiFiClient client;
	if (!client.connect(tfgmApiHttpHost, 80)) {
		Serial.println("HTTPS connection failed!");
		return;
	}
	
	client.print("GET ");
	client.print(tfgmApiPath);
	client.print(" HTTP/1.1\r\n");
	
	client.print("Host: ");
	client.print(tfgmApiHttpHost);
	client.print("\r\n");
	
	client.print("Connection: close\r\n");
	
	client.print("User-Agent: InternetOfTrams\r\n");
	
	client.print("Ocp-Apim-Subscription-Key: ");
	client.print(tfgmApiKey);
	client.print("\r\n");
	
	client.print("\r\n");
	
	// Read past headers
	while (client.connected() && client.readStringUntil('\n') != "\r")
		;
	
	// Read up to start of data
	client.readStringUntil('[');
	
	// Read data entries
	int numObjects = 0;
	String best_destination;
	int best_wait = 999;
	while (client.connected()) {
		String object = client.readStringUntil('}') + "}";
		object.trim();
		numObjects++;
		
		jsmn_parser parser;
		jsmn_init(&parser);
		int numTokens = jsmn_parse(&parser, object.c_str(), object.length(), NULL, 0);
		if (numTokens < 1) {
			// Bad JSON!
			Serial.print("Bad JSON: ");
			Serial.println(numTokens);
			Serial.println(object.c_str());
			continue;
		}
		
		jsmn_init(&parser);
		jsmntok_t *tokens = new jsmntok_t[numTokens];
		numTokens = jsmn_parse(&parser, object.c_str(), object.length(), tokens, numTokens);
		if (numTokens < 1) {
			// Bad JSON!
			Serial.print("Bad JSON: ");
			Serial.println(numTokens);
			Serial.println(object.c_str());
			continue;
		}
		
		if (tokens[0].type != JSMN_OBJECT) {
			// Expected an object...
			Serial.print("Didn't get object: ");
			Serial.println(tokens[0].type);
			delete[] tokens;
			continue;
		}
		
		size_t numEntries = tokens[0].size;
		String stationLocation;
		
		struct {
			String name;
			int wait;
		} destinations[4];
		size_t num_destinations = sizeof(destinations)/sizeof(destinations[0]);
		
		for (size_t i = 1; i < (numEntries*2)+1; i += 2) {
			if (tokens[i].type != JSMN_STRING ||
			    (tokens[i+1].type != JSMN_STRING &&
			     tokens[i+1].type != JSMN_PRIMITIVE)) {
				// Expected all key-value pairs!
				Serial.print("Didn't key value pairs: ");
				Serial.print(tokens[i].type);
				Serial.print(", ");
				Serial.println(tokens[i+1].type);
				break;
			}
			
			String key = object.substring(tokens[i].start, tokens[i].end);
			String value= object.substring(tokens[i+1].start, tokens[i+1].end);
			if (key == "StationLocation") {
				stationLocation = value;
			} else if (key == "Dest0") {
				destinations[0].name = value;
			} else if (key == "Dest1") {
				destinations[1].name = value;
			} else if (key == "Dest2") {
				destinations[2].name = value;
			} else if (key == "Dest3") {
				destinations[3].name = value;
			} else if (key == "Wait0") {
				destinations[0].wait = atoi(value.c_str());
			} else if (key == "Wait1") {
				destinations[1].wait = atoi(value.c_str());
			} else if (key == "Wait2") {
				destinations[2].wait = atoi(value.c_str());
			} else if (key == "Wait3") {
				destinations[3].wait = atoi(value.c_str());
			}
		}
		
		if (stationLocation == "MediaCityUK") {
			for (size_t i = 0; i < num_destinations; i++) {
				if (destinations[i].name.length() &&
				    destinations[i].wait < best_wait &&
				    metrolink_is_destination_valid(destinations[i].name.c_str())) {
					best_destination = destinations[i].name;
					best_wait = destinations[i].wait;
				}
			}
		}
		
		delete[] tokens;
		
		// Skip adjoining comma
		client.readStringUntil(',');
	}
	
	client.stop();
	
	if (best_destination.length()) {
		Serial.print("Next tram to ");
		Serial.print(best_destination);
		Serial.print(" is in ");
		Serial.print(best_wait);
		Serial.println(" min.");
		
		if (best_wait > MAX_VALUE) {
			best_wait = MAX_VALUE;
		}
		analogWrite(pin, PWM_VALUES[best_wait]);
	} else {
		Serial.println("No suitable trams found...");
	}
}
