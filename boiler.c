/**
 * Very basic example showing usage of access point mode and the DHCP server.
 * The ESP in the example runs a telnet server on 172.16.0.1 (port 23) that
 * outputs some status information if you connect to it, then closes
 * the connection.
 *
 * This example code is in the public domain.
 */
#include <espressif/esp_common.h>
#include <esp/uart.h>

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <FreeRTOS.h>
#include <task.h>

#include <esp8266.h>
#include <queue.h>
#include <lwip/api.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>
#include <ssid_config.h>
#include "espressif/user_interface.h"

#include "ota-tftp.h"
#include "rboot-api.h"

#include "inc/ds18b20.h"

#define APP_VER	4

#define CALLBACK_DEBUG

#ifdef CALLBACK_DEBUG
#define debug(s, ...) printf("%s: " s "\n", "Cb:", ## __VA_ARGS__)
#else
#define debug(s, ...)
#endif

#define vTaskDelayMs(ms)	vTaskDelay((ms)/portTICK_PERIOD_MS)
#define UNUSED_ARG(x)	(void)x

#define ECHO_PORT_1 50
#define ECHO_PORT_2 100
#define EVENTS_QUEUE_SIZE 100

#define MAX_SENSORS 4
#define RESCAN_INTERVAL 8
#define LOOP_DELAY_MS 250
#define SENSOR_GPIO 5

typedef struct {
	bool state;
	float temp;
} sensor_t;

const int boiler = 4;
const int light1 = 14;
const int light2 = 16;
sensor_t temp_device = { false, 0 };
sensor_t temp_out = { false, 0 };
sensor_t temp_room = { false, 0 };
sensor_t temp_water = { false, 0 };

bool in_delta = false;
bool out_delta = false;
float set_room_temp = 22, set_room_temp_delta = 1;
float set_out_temp = 10, set_out_temp_delta = 2;
bool modework = false; // false = auto, true = remote

#define ADDR_DEVICE 0x300000027d06ba28
#define ADDR_OUTSIDE 0x86000000a642d928
#define ADDR_ROOM 0x3700000263eb4828
#define ADDR_WATER 0x0d000000a678df28

QueueHandle_t xQueue_events;
typedef struct {
    struct netconn *nc;
    uint8_t type;
} netconn_events;

static const char * const auth_modes [] = {
    [AUTH_OPEN]         = "Open",
    [AUTH_WEP]          = "WEP",
    [AUTH_WPA_PSK]      = "WPA/PSK",
    [AUTH_WPA2_PSK]     = "WPA2/PSK",
    [AUTH_WPA_WPA2_PSK] = "WPA/WPA2/PSK"
};

const char str_help[] = {
		"status\n"
		"boileron/boileroff\n"
		"auto/remote\n"
		"setroomtemp=xx.x\n"
		"setroomdelta=xx.x\n"
		"setouttemp=xx.x\n"
		"setoutdelta=xx.x\n"
		"light1on/light1off\n"
		"light2on/light2off\n"
		"=>"
};

/*
 * This function will be call in Lwip in each event on netconn
 */
static void netCallback(struct netconn *conn, enum netconn_evt evt, uint16_t length)
{
    //Show some callback information (debug)
    //debug("sock:%u\tsta:%u\tevt:%u\tlen:%u\ttyp:%u\tfla:%02X",
            //(uint32_t)conn, conn->state, evt, length, conn->type, conn->flags);

    netconn_events events ;

    //If netconn got error, it is close or deleted, dont do treatments on it.
    if (conn->pending_err) {
        return;
    }
    //Treatments only on rcv events.
    switch (evt) {
		case NETCONN_EVT_RCVPLUS:
			events.nc = conn ;
			events.type = evt ;
			break;
		default:
			return;
    }

    //Send the event to the queue
    xQueueSend(xQueue_events, &events, 100);
}

/*
 *  Initialize a server netconn and listen port
 */
static void set_tcp_server_netconn(struct netconn **nc, uint16_t port, netconn_callback callback)
{
    if(nc == NULL)
    {
        debug("%s: netconn missing .\n",__FUNCTION__);
        return;
    }
    *nc = netconn_new_with_callback(NETCONN_TCP, netCallback);
    if(!*nc) {
        debug("Status monitor: Failed to allocate netconn.\n");
        return;
    }
    netconn_set_nonblocking(*nc,NETCONN_FLAG_NON_BLOCKING);
    //netconn_set_recvtimeout(*nc, 10);
    netconn_bind(*nc, IP_ADDR_ANY, port);
    netconn_listen(*nc);
}

/*
 *  Close and delete a socket properly
 */
static void close_tcp_netconn(struct netconn *nc)
{
	debug("WiFi: tcp netconn close\n\r");
    nc->pending_err = ERR_CLSD; // It is hacky way to be sure than callback will don't do treatment on a netconn closed and deleted
    netconn_close(nc);
    netconn_delete(nc);
}

static void scan_done_cb(void *arg, sdk_scan_status_t status)
{
    char ssid[33]; // max SSID length + zero byte

    if (status != SCAN_OK)
    {
        debug("Error: WiFi scan failed\n");
        return;
    }

    struct sdk_bss_info *bss = (struct sdk_bss_info *)arg;
    // first one is invalid
    bss = bss->next.stqe_next;

    debug("\n----------------------------------------------------------------------------------\n");
    debug("                             Wi-Fi networks\n");
    debug("----------------------------------------------------------------------------------\n");

    while (NULL != bss)
    {
        size_t len = strlen((const char *)bss->ssid);
        memcpy(ssid, bss->ssid, len);
        ssid[len] = 0;

        debug("%32s (" MACSTR ") RSSI: %02d, security: %s\n", ssid,
            MAC2STR(bss->bssid), bss->rssi, auth_modes[bss->authmode]);

        bss = bss->next.stqe_next;
    }
}

static void socketsTask(void *pvParameters)
{
	uint8_t status  = 0;
	UNUSED_ARG(pvParameters);
	struct netconn *nc = NULL; // To create servers
	struct netbuf *netbuf = NULL; // To store incoming Data
	struct netconn *nc_in = NULL; // To accept incoming netconn
	char buf[50];
	char* buffer;
	uint16_t len_buf;
	netconn_events events;
	struct ip_info static_ip_info;
	struct sdk_station_config config = {
		.ssid = WIFI_SSID,
		.password = WIFI_PASS,
		.bssid_set = 0
	};

	gpio_enable(boiler, GPIO_OUTPUT);
	gpio_enable(light1, GPIO_OUTPUT);
	gpio_enable(light2, GPIO_OUTPUT);
	gpio_write(light1, 0);
	gpio_write(light2, 0);

	set_tcp_server_netconn(&nc, ECHO_PORT_1, netCallback);
	debug("Server netconn %u ready on port %u.\n",(uint32_t)nc, ECHO_PORT_1);
	set_tcp_server_netconn(&nc, ECHO_PORT_2, netCallback);
	debug("Server netconn %u ready on port %u.\n",(uint32_t)nc, ECHO_PORT_2);

	debug("ssid: %s\n", config.ssid);
	debug("password: %s\n", config.password);

	sdk_wifi_station_disconnect();
	sdk_wifi_set_opmode(NULL_MODE);
	vTaskDelay(500);
	sdk_wifi_station_dhcpc_stop();
	debug("dhcp status : %d", sdk_wifi_station_dhcpc_status());
	//IP4_ADDR(&static_ip_info.ip, 10,42,0,200);
	//IP4_ADDR(&static_ip_info.gw, 10,42,0,1);
	IP4_ADDR(&static_ip_info.ip, 192,168,0,200);
	IP4_ADDR(&static_ip_info.gw, 192,168,0,1);
	IP4_ADDR(&static_ip_info.netmask, 255,255,255,0);
	debug("static ip set status : %d", sdk_wifi_set_ip_info(STATION_IF, &static_ip_info));
	vTaskDelay(500);
	sdk_wifi_set_opmode(STATION_MODE);
	sdk_wifi_station_set_config(&config);
	sdk_wifi_station_connect();

	while (status != STATION_GOT_IP) {
		sdk_wifi_station_scan(NULL, scan_done_cb);
		vTaskDelayMs(5000);
		status = sdk_wifi_station_get_connect_status();
		debug("%s: status = %d\n\r", __func__, status );
		switch (status) {
			case STATION_WRONG_PASSWORD: {
				debug("WiFi: wrong password\n\r");
				break;
			}
			case STATION_NO_AP_FOUND: {
				debug("WiFi: AP not found\n\r");
				break;
			}
			case STATION_CONNECT_FAIL: {
				debug("WiFi: connection failed\r\n");
				break;
			}
			case STATION_GOT_IP: {
				debug("WiFi: Connected\n\r");
				break;
			}
		}
	}
	if (status == STATION_GOT_IP)
		while (1) {

		xQueueReceive(xQueue_events, &events, portMAX_DELAY); // Wait here an event on netconn

		if (events.nc->state == NETCONN_LISTEN) // If netconn is a server and receive incoming event on it
		{
			debug("Client incoming on server %u.\n", (uint32_t)events.nc);
			int err = netconn_accept(events.nc, &nc_in);
			if (err != ERR_OK)
			{
				if(nc_in)
					netconn_delete(nc_in);
			}
			debug("New client is %u.\n",(uint32_t)nc_in);
			ip_addr_t client_addr; //Address port
			uint16_t client_port; //Client port
			netconn_peer(nc_in, &client_addr, &client_port);
			snprintf(buf, sizeof(buf), "Your address is %d.%d.%d.%d:%u.\r\n=>",
					ip4_addr1(&client_addr), ip4_addr2(&client_addr),
					ip4_addr3(&client_addr), ip4_addr4(&client_addr),
					client_port);
			netconn_write(nc_in, buf, strlen(buf), NETCONN_COPY);
		}
		else if(events.nc->state != NETCONN_LISTEN) // If netconn is the client and receive data
		{
			err_t err = (netconn_recv(events.nc, &netbuf));
			//debug("************ err = %d", err);
			switch (err) {
				case ERR_OK: { // data incoming ?
					do {
						netbuf_data(netbuf, (void*)&buffer, &len_buf);
						//netconn_write(events.nc, buffer, strlen(buffer), NETCONN_COPY);
						debug("Client %u send: %s\n",(uint32_t)events.nc, buffer);
						if (strstr(buffer, "boileron") != 0) {
							if (modework) {
								gpio_write(boiler, 1);
								netconn_write(events.nc, "Boiler on\n=>", strlen("Boiler on\n=>"), NETCONN_COPY);
								debug("Boiler on\n");
							} else {
								netconn_write(events.nc, "Boiler don't on, because modework auto\n=>",
										strlen("Boiler don't on, because modework auto\n=>"), NETCONN_COPY);
								debug("Boiler don't on, because modework auto\n");
							}
						} else if (strstr(buffer, "boileroff") != 0) {
							if (modework) {
								gpio_write(boiler, 0);
								netconn_write(events.nc, "Boiler off\n=>", strlen("Boiler off\n=>"), NETCONN_COPY);
								debug("Boiler off\n");
							} else {
								netconn_write(events.nc, "Boiler don't off, because mode work auto\n=>",
										strlen("Boiler don't off, because mode work auto\n=>"), NETCONN_COPY);
								debug("Boiler don't off, because mode work auto\n");
							}
						} else if (strstr(buffer, "auto") != 0) {
							modework = false;
							netconn_write(events.nc, "Mode work auto\n=>", strlen("Mode work auto\n=>"), NETCONN_COPY);
							debug("Mode work auto\n");
						} else if (strstr(buffer, "remote") != 0) {
							modework = true;
							netconn_write(events.nc, "Mode work remote\n=>", strlen("Mode work remote\n=>"), NETCONN_COPY);
							debug("Mode work remote\n");
						} else if (strstr(buffer, "help") != 0) {
							netconn_write(events.nc, str_help, strlen(str_help), NETCONN_COPY);
							debug("Help\n");
						} else if (strstr(buffer, "setroomtemp=") != 0) {
							char str[50] = "";
							set_room_temp = atof(&buffer[12]);
							sprintf(str, "Set room temp: %.01f\n=>", set_room_temp);
							netconn_write(events.nc, str, strlen(str), NETCONN_COPY);
							debug("%s", str);
						} else if (strstr(buffer, "setroomdelta=") != 0) {
							char str[50] = "";
							set_room_temp_delta = atof(&buffer[13]);
							sprintf(str, "Set room temp delta: %.01f\n=>", set_room_temp_delta);
							netconn_write(events.nc, str, strlen(str), NETCONN_COPY);
							debug("%s", str);
						} else if (strstr(buffer, "setouttemp=") != 0) {
							char str[50] = "";
							set_out_temp = atof(&buffer[11]);
							sprintf(str, "Set out temp: %.01f\n=>", set_out_temp);
							netconn_write(events.nc, str, strlen(str), NETCONN_COPY);
							debug("%s", str);
						} else if (strstr(buffer, "setoutdelta=") != 0) {
							char str[50] = "";
							set_out_temp_delta = atof(&buffer[12]);
							sprintf(str, "Set out temp delta: %.01f\n=>", set_out_temp_delta);
							netconn_write(events.nc, str, strlen(str), NETCONN_COPY);
							debug("%s", str);
						} else if (strstr(buffer, "light1on") != 0) {
							gpio_write(light1, 1);
							netconn_write(events.nc, "Light 1 on\n=>", strlen("Light 1 on\n=>"), NETCONN_COPY);
							debug("Light 1 on\n");
						} else if (strstr(buffer, "light1off") != 0) {
							gpio_write(light1, 0);
							netconn_write(events.nc, "Light 1 off\n=>", strlen("Light 1 off\n=>"), NETCONN_COPY);
							debug("Light 1 off\n");
						} else if (strstr(buffer, "light2on") != 0) {
							gpio_write(light2, 1);
							netconn_write(events.nc, "Light 2 on\n=>", strlen("Light 2 on\n=>"), NETCONN_COPY);
							debug("Light 2 on\n");
						} else if (strstr(buffer, "light2off") != 0) {
							gpio_write(light2, 0);
							netconn_write(events.nc, "Light 2 off\n=>", strlen("Light 2 off\n=>"), NETCONN_COPY);
							debug("Light 2 off\n");
						} else if (strstr(buffer, "status") != 0) {
							char str[500] = "";
							sprintf(str, "Ver: %d\n"
										"Operating mode: %s\n"
										"Boiler: %s\n"
										"Outside temperature: %.01f (%s)\n"
										"Room temperature: %.01f (%s)\n"
										"Device temperature: %.01f (%s)\n"
										"Water temperature: %.01f (%s)\n"
										"Set room temperature (up): %.01f\n"
										"Set room temperature (down): %.01f\n"
										"Set room temperature delta: %.01f\n"
										"Set outside temperature (up): %.01f\n"
										"Set outside temperature (down): %.01f\n"
										"Set outside temperature delta: %.01f\n"
										"Light 1: %s\n"
										"Light 2: %s\n"
										"=>", APP_VER,
										modework ? "Remote" : "Auto",
										gpio_read(boiler) ? "on" : "off",
										temp_out.temp, temp_out.state ? "work" : "error",
										temp_room.temp, temp_room.state ? "work" : "error",
										temp_device.temp, temp_device.state ? "work" : "error",
										temp_water.temp, temp_water.state ? "work" : "error",
										set_room_temp, set_room_temp-set_room_temp_delta, set_room_temp_delta,
										set_out_temp, set_out_temp-set_out_temp_delta, set_out_temp_delta,
										gpio_read(light1) ? "on" : "off",
										gpio_read(light2) ? "on" : "off"
										);
							netconn_write(events.nc, str, strlen(str), NETCONN_COPY);
							debug("%s", str);
						}
					}
					while (netbuf_next(netbuf) >= 0);
					netbuf_delete(netbuf);
					break;
				}
				case ERR_CONN: { // Not connected
					debug("Not connected netconn %u, close it \n",(uint32_t)events.nc);
					close_tcp_netconn(events.nc);
					break;
				}
				default: {
					debug("Error read netconn %u\n",(uint32_t)events.nc);
				}
			}
		}
	}
}

void sensor(void *pvParameters)
{
    ds18b20_addr_t addrs[MAX_SENSORS];
    float temps[MAX_SENSORS];
    int sensor_count;

    // There is no special initialization required before using the ds18b20
    // routines.  However, we make sure that the internal pull-up resistor is
    // enabled on the GPIO pin so that one can connect up a sensor without
    // needing an external pull-up (Note: The internal (~47k) pull-ups of the
    // ESP8266 do appear to work, at least for simple setups (one or two sensors
    // connected with short leads), but do not technically meet the pull-up
    // requirements from the DS18B20 datasheet and may not always be reliable.
    // For a real application, a proper 4.7k external pull-up resistor is
    // recommended instead!)

    //gpio_set_pullup(SENSOR_GPIO, true, true);

    while(1) {
        // Every RESCAN_INTERVAL samples, check to see if the sensors connected
        // to our bus have changed.
        sensor_count = ds18b20_scan_devices(SENSOR_GPIO, addrs, MAX_SENSORS);

        if (sensor_count < 1) {
        	printf("\nNo sensors detected!\n");
          vTaskDelay(LOOP_DELAY_MS * 10 / portTICK_PERIOD_MS);
          modework = true;
        } else {
        	printf("\n%d sensors detected:\n", sensor_count);
            // If there were more sensors found than we have space to handle,
            // just report the first MAX_SENSORS..
            if (sensor_count > MAX_SENSORS) sensor_count = MAX_SENSORS;

            // Do a number of temperature samples, and print the results.
            for (int8_t i = 0; i < RESCAN_INTERVAL; i++) {
                if (ds18b20_measure_and_read_multi(SENSOR_GPIO, addrs, sensor_count, temps) == false) {
                	printf("Temp error, sensor: %d\n", sensor_count);
                }
                for (int8_t j = 0; j < sensor_count; j++) {
                    // The DS18B20 address is a 64-bit integer, but newlib-nano
                    // printf does not support printing 64-bit values, so we
                    // split it up into two 32-bit integers and print them
                    // back-to-back to make it look like one big hex number.
                    //uint32_t addr0 = addrs[j] >> 32;
                    //uint32_t addr1 = addrs[j];
                    uint64_t addr = addrs[j];
                    float temp_c = temps[j];
                    //float temp_f = (temp_c * 1.8) + 32;
                    //printf("  Sensor %08x%08x reports %f deg C (%f deg F)\n", addr0, addr1, temp_c, temp_f);
                    switch (addr) {
						case ADDR_DEVICE: {
							if ((uint32_t)temp_c == 0xffffffff) {
								temp_device.state = false;
							} else {
								temp_device.state = true;
								temp_device.temp = temp_c;
							}
							printf("Temp device, addr: %08x%08x, state: %d, temp: %.01f\n",
									(uint32_t)(addr >> 32), (uint32_t)(addr), temp_device.state, temp_device.temp);
							break;
						}
						case ADDR_OUTSIDE: {
							if ((uint32_t)temp_c == 0xffffffff) {
								temp_out.state = false;
							} else {
								temp_out.state = true;
								temp_out.temp = temp_c;
							}
							printf("Temp out, addr: %08x%08x, state: %d, temp: %.01f\n",
									(uint32_t)(addr >> 32), (uint32_t)(addr), temp_out.state, temp_out.temp);
							break;
						}
						case ADDR_ROOM: {
							if ((uint32_t)temp_c == 0xffffffff) {
								temp_room.state = false;
							} else {
								temp_room.state = true;
								temp_room.temp = temp_c;
							}
							printf("Temp room, addr: %08x%08x, state: %d, temp: %.01f\n",
									(uint32_t)(addr >> 32), (uint32_t)(addr), temp_room.state, temp_room.temp);
							break;
						}
						case ADDR_WATER: {
							if ((uint32_t)temp_c == 0xffffffff) {
								temp_water.state = false;
							} else {
								temp_water.state = true;
								temp_water.temp = temp_c;
							}
							printf("Temp water, addr: %08x%08x, state: %d, temp: %.01f\n",
									(uint32_t)(addr >> 32), (uint32_t)(addr), temp_water.state, temp_water.temp);
							break;
						}
                    }
                }
                printf("\n");

                // Wait for a little bit between each sample (note that the
                // ds18b20_measure_and_read_multi operation already takes at
                // least 750ms to run, so this is on top of that delay).
                vTaskDelay(LOOP_DELAY_MS / portTICK_PERIOD_MS);
            }
            if (modework == false) {
				if (temp_out.state == false && temp_room.state == false) {
					modework = true;
					gpio_write(boiler, 1);
				} else {
					if ((temp_out.temp < set_out_temp && out_delta == false) ||
						(temp_out.temp < set_out_temp - set_out_temp_delta && out_delta == true) ) {
						out_delta = false;
						if (in_delta) {
							if (temp_room.temp < (set_room_temp - set_room_temp_delta)) {
								in_delta = false;
								gpio_write(boiler, 1);
							}
						} else {
							if (temp_room.temp < set_room_temp) {
								gpio_write(boiler, 1);
							} else {
								in_delta = true;
								gpio_write(boiler, 0);
							}
						}
					} else {
						out_delta = true;
						in_delta = false;
						gpio_write(boiler, 0);
					}
				}
        	}
        }
    }
}

void user_init(void)
{
    gpio_set_iomux_function(2, IOMUX_GPIO2_FUNC_UART1_TXD);
    uart_set_baud(0, 115200);

    printf("============================ Start ==================================\n");
	debug("SDK version:%s, App version %d\n", sdk_system_get_sdk_version(), APP_VER);

	rboot_config conf = rboot_get_config();
	printf("OTA Boiler.\r\nCurrently running on flash slot %d / %d.\r\n",
		   conf.current_rom, conf.count);

	printf("Image addresses in flash:\r\n");
	for(int i = 0; i <conf.count; i++) {
		printf("%c%d: offset 0x%08x\r\n", i == conf.current_rom ? '*':' ', i, conf.roms[i]);
	}

	printf("Starting TFTP server...\n");
	ota_tftp_init_server(TFTP_PORT);

	//Create a queue to store events on netconns
	xQueue_events = xQueueCreate(EVENTS_QUEUE_SIZE, sizeof(netconn_events));
    xTaskCreate(socketsTask, "socketsTask", 512, NULL, 2, NULL);
    xTaskCreate(sensor, "sensor", 512, NULL, 2, NULL);
}
