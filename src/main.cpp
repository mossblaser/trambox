#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <WiFiClient.h>
#include <Ticker.h>
#include <math.h>

#include <jsmn.h>

#include "metrolink.h"

////////////////////////////////////////////////////////////////////////////////
// Configuration
////////////////////////////////////////////////////////////////////////////////

// The pin number of the display
const int DISPLAY_PIN = D1;

// The largest number which can be displayed by the output display.
const int DISPLAY_MAX_VALUE = 12;

// The maximum distance the needle may wobble from its nominal position
const float DISPLAY_WOBBLE_MAGNITUDE = 0.5;

// Number of seconds between display updates
const float DISPLAY_UPDATE_INTERVAL = 0.1;

// A magic string written to the start of the EEPROM to validate that the data
// in EEPROM was actually written by this program.
const char *EEPROM_MAGIC_STRING = "IOT0";
const size_t EEPROM_MAGIC_STRING_LENGTH = 4;

const char *TFGM_HTTP_HOST = "metrolink.jhnet.co.uk";
const char *TFGM_API_PATH = "/odata/Metrolinks";

////////////////////////////////////////////////////////////////////////////////
// State
////////////////////////////////////////////////////////////////////////////////

Ticker timer;

// The value to be shown on the display
float display_value = DISPLAY_WOBBLE_MAGNITUDE;

// Should the needle wobble around the current value?
bool display_wobble = true;

// Should the display_value be gradually reduced (by 1 minute per minute)? If
// the displayed value reaches more than 1+DISPLAY_WOBBLE_MAGNITUDE,
// display_wobble will be enabled and display_auto_decrement disabled.
bool display_auto_decrement = false;

// The latest time reported by the Metrolink API
int last_metrolink_wait = -1;

// Time at which the last_metrolink_wait value was changed
unsigned long last_metrolink_change_time = 0;


typedef struct {
	// Magic string, should be equal to EEPROM_MAGIC_STRING
	char magic_string[EEPROM_MAGIC_STRING_LENGTH];
	
	// The PWM value to set which makes the display show a given number of minutes.
	// Values are provided from 0 to DISPLAY_MAX_VALUE (inclusive).
	int display_pwm_values[DISPLAY_MAX_VALUE + 1];
	
	// Wifi connection details
	char wifi_ssid[32];
	char wifi_password[64];
	
	// TFGM API key
	char tfgm_api_key[64];
	
	// Start/end station name
	char station_start[32];
	char station_end[32];
} eeprom_config_t;

eeprom_config_t config;


////////////////////////////////////////////////////////////////////////////////
// Implementation
////////////////////////////////////////////////////////////////////////////////

/**
 * Read the configuration from eeprom into config struct, returns false if no
 * valid configuration was found in EEPROM, true otherwise.
 */
bool eeprom_load() {
	for (size_t i = 0 ; i < sizeof(eeprom_config_t); i++) {
		((char *)&config)[i] = EEPROM.read(i);
	}
	
	if (strcmp(config.magic_string, EEPROM_MAGIC_STRING) == 0) {
		// Valid data read!
		return true;
	} else {
		// Invalid data, fill the config with a blank initial configuration
		memcpy(config.magic_string, EEPROM_MAGIC_STRING, EEPROM_MAGIC_STRING_LENGTH);
		
		for (size_t i = 0; i < DISPLAY_MAX_VALUE + 1; i++) {
			config.display_pwm_values[i] = 0;
		}
		
		strcpy(config.wifi_ssid, "");
		strcpy(config.wifi_password, "");
		
		strcpy(config.tfgm_api_key, "");
		
		strcpy(config.station_start, "");
		strcpy(config.station_end, "");
		
		return false;
	}
}

/**
 * Store the current configuration into EEPROM.
 */
void eeprom_store() {
	for (size_t i = 0 ; i < sizeof(eeprom_config_t); i++) {
		EEPROM.write(i, ((char *)&config)[i]);
	}
	EEPROM.commit();
}

/**
 * (Re-)Connect to wifi.
 */
void wifi_connect() {
	WiFi.disconnect(false);
	
	if (strlen(config.wifi_ssid) == 0) {
		Serial.println("Can't connect to WiFi: No WiFi credentials configured.");
		return;
	}
	Serial.println();
	Serial.print("Connecting to WiFi ");
	Serial.println(config.wifi_ssid);

	WiFi.begin(config.wifi_ssid, config.wifi_password);

	for (int i = 0; i < 60; i++) {
		if (WiFi.status() != WL_CONNECTED) {
			delay(1000);
			Serial.print(".");
		} else {
			break;
		}
	}
	Serial.println("");
	
	if (WiFi.status() == WL_CONNECTED) {
		Serial.println("WiFi connected");
		Serial.print("IP address: ");
		Serial.println(WiFi.localIP());
	} else {
		Serial.println("WiFi connection timed out (will keep trying in the background)");
	}
}

/**
 * Called regullarly by the timer to update the displayed value.
 */
void update_display() {
	static float wobble_phase = 0.0;
	
	// Auto-decrement the time displayed
	if (display_auto_decrement) {
		display_value -= DISPLAY_UPDATE_INTERVAL / 60.0;
		
		if (display_value < last_metrolink_wait - (1 + DISPLAY_WOBBLE_MAGNITUDE)) {
			// Maximum deviation from the reported time reached, start wobbling
			display_auto_decrement = false;
			display_wobble = true;
			
			// Start the wobble from where we are and centered on the 'next' expected
			// minute
			display_value = last_metrolink_wait;
			wobble_phase = 3.14 * 1.5;
		}
	}
	
	// Wobble the value if required
	float value = display_value;
	if (display_wobble) {
		value += sin(wobble_phase) * DISPLAY_WOBBLE_MAGNITUDE;
		
		wobble_phase += 3.14 * DISPLAY_UPDATE_INTERVAL;
		while (wobble_phase > (3.14*2)) {
			wobble_phase -= 3.14;
		}
	} else {
		wobble_phase = 0.0;
	}
	
	// Clamp the value to the displayable range
	if (value < 0.0) {
		value = 0.0;
	} else if (value > DISPLAY_MAX_VALUE) {
		value = DISPLAY_MAX_VALUE;
	}
	
	// Interpolate PWM values
	int value_low = floorf(value);
	int value_high = ceilf(value);
	
	int pwm_low = config.display_pwm_values[value_low];
	int pwm_high = config.display_pwm_values[value_high];
	int pwm_range = pwm_high - pwm_low;
	
	int pwm = pwm_low + (pwm_range * (value - value_low));
	
	analogWrite(DISPLAY_PIN, pwm);
}


/**
 * Parse a JSON object defining the display of a tram information screen.
 * Returns the lowest number of minutes until the next tram departure or -1 if
 * no value was found.
 */
int parse_value(String &object) {
	// Determine number of JSON tokens in object
	jsmn_parser parser;
	jsmn_init(&parser);
	int numTokens = jsmn_parse(&parser, object.c_str(), object.length(), NULL, 0);
	if (numTokens < 1) {
		// Bad JSON!
		Serial.println("WARNING: Failed to parse response JSON.");
		return -1;
	}
	
	jsmn_init(&parser);
	jsmntok_t *tokens = new jsmntok_t[numTokens];
	numTokens = jsmn_parse(&parser, object.c_str(), object.length(), tokens, numTokens);
	if (numTokens < 1) {
		// Bad JSON!
		Serial.println("WARNING: Failed to parse response JSON.");
		delete[] tokens;
		return -1;
	}
	
	if (tokens[0].type != JSMN_OBJECT) {
		Serial.println("WARNING: JSON response did not contain expected object.");
		delete[] tokens;
		return -1;
	}
	
	
	// Extract the StationLocation, DestN and WaitN fields from the JSON
	String station_location;
	const size_t destinations_per_object = 4;
	struct {
		String name;
		int wait;
	} destinations[destinations_per_object];
	
	for (size_t i = 1; i < (tokens[0].size*2)+1; i += 2) {
		if (tokens[i].type != JSMN_STRING ||
		    (tokens[i+1].type != JSMN_STRING &&
		     tokens[i+1].type != JSMN_PRIMITIVE)) {
			// Expected all key-value pairs!
			Serial.println("WARNING: Unexpected value in JSON object.");
			break;
		}
		
		String key = object.substring(tokens[i].start, tokens[i].end);
		String value= object.substring(tokens[i+1].start, tokens[i+1].end);
		if (key == "StationLocation") {
			station_location = value;
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
	
	int min_wait = -1;
	if (metrolink_station_names_equal(station_location.c_str(), config.station_start)) {
		for (size_t i = 0; i < destinations_per_object; i++) {
			if (destinations[i].name.length() &&
			    (destinations[i].wait < min_wait || min_wait == -1) &&
			    metrolink_is_destination_valid(destinations[i].name.c_str())) {
				min_wait = destinations[i].wait;
			}
		}
	}
	
	delete[] tokens;
	return min_wait;
}

/**
 * Indicate a problem by bouncing the needle around between 0 and 1.
 */
void show_error_display() {
	display_value = DISPLAY_WOBBLE_MAGNITUDE;
	display_wobble = true;
	display_auto_decrement = false;
}

/**
 * Attempt to fetch the number of minutes until the next departure or -1 if no
 * departure time is known.
 */
int get_next_departure_wait() {
	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("WiFi not connected, not fetching times...");
		show_error_display();
		return -1;
	}
	
	Serial.println("Fetching tram times...");
	
	WiFiClient client;
	if (!client.connect(TFGM_HTTP_HOST, 80)) {
		Serial.println("ERROR: HTTP connection failed!");
		return -1;
	}
	
	// Send headers
	client.print("GET ");
	client.print(TFGM_API_PATH);
	client.print(" HTTP/1.1\r\n");
	
	client.print("Host: ");
	client.print(TFGM_HTTP_HOST);
	client.print("\r\n");
	
	client.print("Connection: close\r\n");
	
	client.print("User-Agent: InternetOfTrams\r\n");
	
	client.print("Ocp-Apim-Subscription-Key: ");
	client.print(config.tfgm_api_key);
	client.print("\r\n");
	
	client.print("\r\n");
	
	// Read past response headers
	while (client.connected() && client.readStringUntil('\n') != "\r")
		;
	
	// Read up to start of data (the response is an object containing an array of
	// data values)
	client.readStringUntil('[');
	
	// Read data entries one at a time.
	int min_wait = -1;
	while (client.connected()) {
		String object = client.readStringUntil('}') + "}";
		
		int this_wait = parse_value(object);
		if (this_wait >= 0 &&
		    (this_wait < min_wait || min_wait == -1)) {
			min_wait = this_wait;
		}
		
		// Skip past adjoining comma between objects
		client.readStringUntil(',');
	}
	
	// Done!
	client.stop();
	if (min_wait >= 0) {
		Serial.print("Wait time is ");
		Serial.print(min_wait);
		Serial.println(" min");
	} else {
		Serial.println("No next tram time found...");
	}
	return min_wait;
}

/**
 * If the display is already showing the current wait time, update it with any
 * new value.
 */
void update_wait_display() {
	int wait = get_next_departure_wait();
	if (wait != last_metrolink_wait) {
		last_metrolink_wait = wait;
		last_metrolink_change_time = millis();
		
		if (wait >= 0) {
			// Wait is valid
			display_value = wait;
			display_auto_decrement = true;
			display_wobble = false;
		} else {
			// Wait is invalid, just bounce around between 0 and 1.
			show_error_display();
		}
	}
}

/**
 * Re-show the current wait on the display, having been showing other values.
 */
void show_wait_display() {
	if (last_metrolink_wait < 0) {
		show_error_display();
	} else {
		float delta = ((float)millis()) - ((float)last_metrolink_change_time);
		delta /= (60 * 1000); // Scale to minutes
		if (delta > 1.0 + DISPLAY_WOBBLE_MAGNITUDE) { // Clamp to a reasonable size
			delta = 1.0 + DISPLAY_WOBBLE_MAGNITUDE;
		}
		
		// Show the updated time
		display_auto_decrement = true;
		display_wobble = false;
		display_value = last_metrolink_wait - delta;
	}
}

/**
 * Allow the user to enter a new WiFi SSID and password.
 */
void wifi_menu() {
	Serial.println("Enter WiFi SSID and press return:");
	String ssid = Serial.readStringUntil('\n');
	ssid.trim();
	strncpy(config.wifi_ssid, ssid.c_str(), sizeof(config.wifi_ssid) - 1);
	config.wifi_ssid[sizeof(config.wifi_ssid) - 1] = '\0';
	
	Serial.println("Enter WiFi password and press return:");
	String password = Serial.readStringUntil('\n');
	password.trim();
	strncpy(config.wifi_password, password.c_str(), sizeof(config.wifi_password) - 1);
	config.wifi_password[sizeof(config.wifi_password) - 1] = '\0';
	
	Serial.println("WiFi credentials changed.");
	
	eeprom_store();
	
	wifi_connect();
}

/**
 * Allow the user to enter a new TFGM API key.
 */
void tfgm_api_key_menu() {
	Serial.println("Enter TFGM API key and press return:");
	String key = Serial.readStringUntil('\n');
	key.trim();
	strncpy(config.tfgm_api_key, key.c_str(), sizeof(config.tfgm_api_key) - 1);
	config.tfgm_api_key[sizeof(config.tfgm_api_key) - 1] = '\0';
	
	Serial.println("TFGM API key changed.");
	
	eeprom_store();
}

/**
 * Allow the user to enter a new start/end station
 */
void route_menu() {
	Serial.println("Enter starting station name:");
	String start = Serial.readStringUntil('\n');
	start.trim();
	strncpy(config.station_start, start.c_str(), sizeof(config.station_start) - 1);
	config.station_start[sizeof(config.station_start) - 1] = '\0';
	
	Serial.println("Enter destination station name:");
	String end = Serial.readStringUntil('\n');
	end.trim();
	strncpy(config.station_end, end.c_str(), sizeof(config.station_end) - 1);
	config.station_end[sizeof(config.station_end) - 1] = '\0';
	
	Serial.println("Metrolink route updated");
	
	eeprom_store();
	
	metrolink_set_journey(config.station_start, config.station_end);
}

/**
 * Allow the user to calibrate the display
 */
void display_calibration_menu() {
	display_value = 0;
	display_wobble = false;
	display_auto_decrement = false;
	
	Serial.println("Adjust needle position using j and k. Confirm with 'enter'.");
	
	for (size_t i = 0; i < DISPLAY_MAX_VALUE + 1; i++) {
		Serial.print("  Move to ");
		Serial.println(i);
		
		if (i == 0) {
			config.display_pwm_values[i] = 0;
		} else {
			config.display_pwm_values[i] = config.display_pwm_values[i - 1];
		}
		display_value = i;
		
		int c = 0;
		while (c != '\n') {
			c = Serial.read();
			switch (c) {
				case 'j':
					config.display_pwm_values[i]--;
					if (config.display_pwm_values[i] < 0) {
						config.display_pwm_values[i] = 0;
					}
					break;
				
				case 'k':
					config.display_pwm_values[i]++;
					if (config.display_pwm_values[i] >= 1023) {
						config.display_pwm_values[i] = 1023;
					}
					break;
			}
			
			// Keep watchdog fed...
			delay(1);
		}
	}
	
	Serial.println("Display calibration complete!");
	
	eeprom_store();
	
	show_wait_display();
}


/**
 * Shows the main menu on the serial terminal, timing out after a few seconds
 * of inactivity.
 */
void main_menu() {
	Serial.println("Press one of the following keys to configure:");
	Serial.println("  w: Set WiFi SSID and password");
	Serial.println("  t: Set TFGM API key");
	Serial.println("  r: Set metrolink route");
	Serial.println("  d: Calibrate display");
	Serial.println("[press a key]");
	
	unsigned long stop = millis() + 2000;
	while (millis() < stop) {
		int c = Serial.read();
		switch (c) {
			case 'w':
				wifi_menu();
				return;
			case 't':
				tfgm_api_key_menu();
				return;
			case 'r':
				route_menu();
				return;
			case 'd':
				display_calibration_menu();
				return;
			
			default:
				// Do nothing, keep waiting
				break;
		}
		
		// Keep watchdog fed...
		delay(1);
	}
	
	Serial.println("[no option selected, continuing]");
}

void setup() {
	Serial.begin(9600);
	Serial.setTimeout(60*1000);
	EEPROM.begin(sizeof(eeprom_config_t));
	
	// Load the network graph
	metrolink_init();
	
	// Load stored configuration
	eeprom_load();
	
	// Setup display pin.
	// Hack: By setting this as an input we use the internal pull-up reisitor
	// (enabled/disabled when we write to the pin) to limit current into the
	// capacitor/display!
	pinMode(DISPLAY_PIN, INPUT);
	analogWrite(DISPLAY_PIN, 0);
	
	// Initially show an 'error' status while we connect to wifi and get the
	// initial time
	show_error_display();
	timer.attach(DISPLAY_UPDATE_INTERVAL, update_display);
	
	metrolink_set_journey(config.station_start, config.station_end);
	wifi_connect();
}

void loop() {
	main_menu();
	update_wait_display();
}
