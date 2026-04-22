#include "console.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "channel.h"
#include "config.h"
#include "cosmo/cosmo.h"
#include "json.gen.h"
#include "mqtt.h"
#include "radio.h"
#include "status_led.h"
#include "utils.h"
#include "wifi_manager.h"

#include "driver/usb_serial_jtag.h"

#define PROMPT_STR "uni-gtw"

static const char *TAG = "console";

/* ── reboot ──────────────────────────────────────────────────────────────── */

static int do_reboot(int argc, char **argv) {
  printf("Saving config and rebooting...\n");
  config_save_now();
  vTaskDelay(pdMS_TO_TICKS(100));
  esp_restart();
  return 0;
}

static void register_reboot(void) {
  const esp_console_cmd_t cmd = {
      .command = "reboot",
      .help = "Save config and reboot the device",
      .hint = NULL,
      .func = &do_reboot,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── wifi_connect ────────────────────────────────────────────────────────── */

static struct {
  struct arg_str *ssid;
  struct arg_str *pass;
  struct arg_end *end;
} wifi_connect_args;

static int do_wifi_connect(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&wifi_connect_args);
  if (nerrors) {
    arg_print_errors(stderr, wifi_connect_args.end, argv[0]);
    return 1;
  }
  const char *ssid = wifi_connect_args.ssid->sval[0];
  const char *pass =
      wifi_connect_args.pass->count ? wifi_connect_args.pass->sval[0] : "";
  esp_err_t err = wifi_manager_set_credentials(ssid, pass);
  if (err != ESP_OK) {
    printf("Failed: %s\n", esp_err_to_name(err));
    return 1;
  }
  printf("Connecting to \"%s\"...\n", ssid);
  return 0;
}

static void register_wifi_connect(void) {
  wifi_connect_args.ssid = arg_str1(NULL, NULL, "<ssid>", "SSID of AP");
  wifi_connect_args.pass =
      arg_str0(NULL, NULL, "<pass>", "Password (omit for open)");
  wifi_connect_args.end = arg_end(2);

  const esp_console_cmd_t cmd = {
      .command = "wifi_connect",
      .help = "Connect to a WiFi AP",
      .hint = NULL,
      .func = &do_wifi_connect,
      .argtable = &wifi_connect_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── wifi_reset ──────────────────────────────────────────────────────────── */

static int do_wifi_reset(int argc, char **argv) {
  esp_err_t err = wifi_manager_clear_credentials();
  if (err != ESP_OK) {
    printf("Failed to clear credentials: %s\n", esp_err_to_name(err));
    return 1;
  }
  printf("WiFi credentials cleared. Rebooting...\n");
  vTaskDelay(pdMS_TO_TICKS(200));
  esp_restart();
  return 0;
}

static void register_wifi_reset(void) {
  const esp_console_cmd_t cmd = {
      .command = "wifi_reset",
      .help = "Remove WiFi credentials and reboot into AP mode",
      .hint = NULL,
      .func = &do_wifi_reset,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── wifi_scan ───────────────────────────────────────────────────────────── */

static int do_wifi_scan(int argc, char **argv) {
  printf("Scanning...\n");
  wifi_scan_result_t results[WIFI_SCAN_MAX_APS];
  uint16_t count = WIFI_SCAN_MAX_APS;
  esp_err_t err = wifi_manager_scan(results, &count);
  if (err != ESP_OK) {
    printf("Scan failed: %s\n", esp_err_to_name(err));
    return 1;
  }
  printf("%-32s  %5s  %s\n", "SSID", "RSSI", "Auth");
  for (int i = 0; i < (int)count; i++) {
    printf("%-32s  %5d  %s\n", results[i].ssid, (int)results[i].rssi,
           results[i].authmode == 0 ? "open" : "WPA/WPA2");
  }
  printf("%d AP(s) found\n", (int)count);
  return 0;
}

static void register_wifi_scan(void) {
  const esp_console_cmd_t cmd = {
      .command = "wifi_scan",
      .help = "Scan for nearby WiFi access points",
      .hint = NULL,
      .func = &do_wifi_scan,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── status ──────────────────────────────────────────────────────────────── */

static int do_status(int argc, char **argv) {
  /* WiFi */
  wifi_mgr_mode_t mode = wifi_manager_get_mode();
  const char *mode_str = (mode == WIFI_MGR_MODE_AP)    ? "AP"
                         : (mode == WIFI_MGR_MODE_STA) ? "STA"
                                                       : "none";
  printf("WiFi mode:  %s\n", mode_str);

  if (mode == WIFI_MGR_MODE_STA) {
    char ssid[33] = {0};
    if (wifi_manager_get_sta_ssid(ssid, sizeof(ssid)))
      printf("WiFi SSID:  %s\n", ssid);

    int8_t rssi;
    if (wifi_manager_get_rssi(&rssi))
      printf("WiFi RSSI:  %d dBm\n", (int)rssi);

    esp_netif_t *sta = wifi_manager_get_sta_netif();
    if (sta) {
      esp_netif_ip_info_t ip;
      if (esp_netif_get_ip_info(sta, &ip) == ESP_OK) {
        printf("IP:         " IPSTR "\n", IP2STR(&ip.ip));
        printf("Gateway:    " IPSTR "\n", IP2STR(&ip.gw));
      }
    }
  }

  /* NTP */
  printf("NTP:        %s\n", utils_time_is_valid() ? "synced" : "not synced");

  /* MQTT */
  const char *mqtt_str;
  switch (g_mqtt_status) {
  case mqtt_status_t_connected:
    mqtt_str = "connected";
    break;
  case mqtt_status_t_connecting:
    mqtt_str = "connecting";
    break;
  case mqtt_status_t_disconnected:
    mqtt_str = "disconnected";
    break;
  default:
    mqtt_str = "unconfigured";
    break;
  }
  printf("MQTT:       %s\n", mqtt_str);

  /* Radio */
  const char *radio_str;
  switch (radio_get_state()) {
  case RADIO_STATE_OK:
    radio_str = "ok";
    break;
  case RADIO_STATE_ERROR:
    radio_str = "error";
    break;
  default:
    radio_str = "not_configured";
    break;
  }
  printf("Radio:      %s\n", radio_str);

  return 0;
}

static void register_status(void) {
  const esp_console_cmd_t cmd = {
      .command = "status",
      .help = "Print WiFi, NTP, MQTT and radio status",
      .hint = NULL,
      .func = &do_status,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── radio_tx ────────────────────────────────────────────────────────────── */

static struct {
  struct arg_str *proto;
  struct arg_str *serial;
  struct arg_int *counter;
  struct arg_str *cmd;
  struct arg_int *extra;
  struct arg_end *end;
} radio_tx_args;

static int do_radio_tx(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&radio_tx_args);
  if (nerrors) {
    arg_print_errors(stderr, radio_tx_args.end, argv[0]);
    return 1;
  }

  /* proto */
  cosmo_proto_t proto;
  const char *proto_str = radio_tx_args.proto->sval[0];
  if (strcmp(proto_str, "1way") == 0 || strcmp(proto_str, "0") == 0) {
    proto = PROTO_COSMO_1WAY;
  } else if (strcmp(proto_str, "2way") == 0 || strcmp(proto_str, "1") == 0) {
    proto = PROTO_COSMO_2WAY;
  } else {
    printf("Invalid proto '%s' — use 1way, 2way, 0, or 1\n", proto_str);
    return 1;
  }

  /* serial — zero last 5 bits */
  char *endp;
  uint32_t serial = (uint32_t)strtoul(radio_tx_args.serial->sval[0], &endp, 0);
  if (*endp != '\0') {
    printf("Invalid serial '%s'\n", radio_tx_args.serial->sval[0]);
    return 1;
  }
  serial &= ~0x1Fu;

  /* cmd */
  cosmo_cmd_t cosmo_cmd;
  if (!utils_str_to_cosmo_cmd(radio_tx_args.cmd->sval[0], &cosmo_cmd)) {
    printf("Unknown cmd '%s'\n", radio_tx_args.cmd->sval[0]);
    printf(
        "Valid names: UP DOWN STOP UP_DOWN STOP_DOWN STOP_HOLD PROG STOP_UP\n"
        "             REQUEST_FEEDBACK REQUEST_POSITION SET_POSITION SET_TILT\n"
        "             TILT_INCREASE TILT_DECREASE DETAILED_FEEDBACK (or "
        "integer)\n");
    return 1;
  }

  uint8_t extra =
      radio_tx_args.extra->count ? (uint8_t)radio_tx_args.extra->ival[0] : 0;

  cosmo_packet_t pkt = {
      .proto = proto,
      .cmd = cosmo_cmd,
      .serial = serial,
      .counter = (uint16_t)radio_tx_args.counter->ival[0],
      .repeat = 1,
      .extra_payload = extra,
  };

  printf("TX proto=%s serial=0x%08" PRIX32 " counter=%u cmd=%s(%d) extra=%u\n",
         proto == PROTO_COSMO_2WAY ? "2way" : "1way", serial,
         (unsigned)pkt.counter, cosmo_cmd_name(cosmo_cmd), (int)cosmo_cmd,
         (unsigned)extra);

  esp_err_t err = radio_request_tx(&pkt);
  if (err != ESP_OK) {
    printf("radio_request_tx failed: %s\n", esp_err_to_name(err));
    return 1;
  }
  return 0;
}

static void register_radio_tx(void) {
  radio_tx_args.proto =
      arg_str1(NULL, NULL, "<1way|2way>", "Protocol (1way or 2way, or 0/1)");
  radio_tx_args.serial =
      arg_str1(NULL, NULL, "<serial>",
               "Serial number (hex 0x… or decimal); last 5 bits zeroed");
  radio_tx_args.counter = arg_int1(NULL, NULL, "<counter>", "Packet counter");
  radio_tx_args.cmd =
      arg_str1(NULL, NULL, "<UP|DOWN|STOP|…|N>", "Command name or raw integer");
  radio_tx_args.extra = arg_int0(NULL, NULL, "[extra]",
                                 "Extra payload byte (default 0, 2way only)");
  radio_tx_args.end = arg_end(4);

  const esp_console_cmd_t cmd = {
      .command = "radio_tx",
      .help = "Transmit a Cosmo RF packet",
      .hint = " <1way|2way> <serial> <counter> <cmd> [extra]",
      .func = &do_radio_tx,
      .argtable = &radio_tx_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── dump_config ─────────────────────────────────────────────────────────── */

static int do_dump_config(int argc, char **argv) {
  config_lock();
  sstr_t saved_pw = g_config.web_password;
  g_config.web_password = sstr("");
  sstr_t json = sstr_new();
  json_marshal_gateway_config_t(&g_config, json);
  sstr_free(g_config.web_password);
  g_config.web_password = saved_pw;
  config_unlock();

  printf("%s\n", sstr_cstr(json));
  sstr_free(json);
  return 0;
}

static void register_dump_config(void) {
  const esp_console_cmd_t cmd = {
      .command = "dump_config",
      .help = "Print the current config as JSON (password hash omitted)",
      .hint = NULL,
      .func = &do_dump_config,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── config_set ──────────────────────────────────────────────────────────── */

static struct {
  struct arg_str *name;
  struct arg_str *value;
  struct arg_end *end;
} config_set_args;

static int do_config_set(int argc, char **argv) {
  int nerrors = arg_parse(argc, argv, (void **)&config_set_args);
  if (nerrors) {
    arg_print_errors(stderr, config_set_args.end, argv[0]);
    return 1;
  }

  const char *name = config_set_args.name->sval[0];
  const char *val = config_set_args.value->sval[0];
  bool need_radio = false;
  bool need_led = false;

  config_lock();
  if (strcmp(name, "gpio_status_led") == 0) {
    g_config.gpio_status_led = (int)strtol(val, NULL, 0);
    need_led = true;
  } else if (strcmp(name, "radio.enabled") == 0) {
    g_config.radio.enabled =
        (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_miso") == 0) {
    g_config.radio.gpio_miso = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_mosi") == 0) {
    g_config.radio.gpio_mosi = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_sck") == 0) {
    g_config.radio.gpio_sck = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_csn") == 0) {
    g_config.radio.gpio_csn = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_gdo0") == 0) {
    g_config.radio.gpio_gdo0 = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.spi_freq_hz") == 0) {
    g_config.radio.spi_freq_hz = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.type") == 0) {
    if (strcmp(val, "sx1262") == 0) {
      g_config.radio.type = radio_type_t_sx1262;
    } else {
      g_config.radio.type = radio_type_t_cc1101;
    }
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_rst") == 0) {
    g_config.radio.gpio_rst = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else if (strcmp(name, "radio.gpio_busy") == 0) {
    g_config.radio.gpio_busy = (int)strtol(val, NULL, 0);
    need_radio = true;
  } else {
    config_unlock();
    printf("Unknown setting: %s\n", name);
    printf("Valid: gpio_status_led, radio.enabled, radio.type,\n"
           "       radio.gpio_miso, radio.gpio_mosi, radio.gpio_sck,\n"
           "       radio.gpio_csn, radio.gpio_gdo0, radio.spi_freq_hz,\n"
           "       radio.gpio_rst (SX1262), radio.gpio_busy (SX1262)\n");
    return 1;
  }
  config_unlock();

  config_mark_dirty();
  if (need_radio)
    radio_apply_config();
  if (need_led)
    status_led_apply_config();
  printf("OK\n");
  return 0;
}

static void register_config_set(void) {
  config_set_args.name =
      arg_str1(NULL, NULL, "<name>", "Setting name (e.g. radio.enabled)");
  config_set_args.value = arg_str1(NULL, NULL, "<value>", "New value");
  config_set_args.end = arg_end(2);

  const esp_console_cmd_t cmd = {
      .command = "config_set",
      .help = "Set a runtime config value",
      .hint = " <gpio_status_led|radio.enabled|radio.type|radio.gpio_*|radio.spi_freq_hz> "
              "<value>",
      .func = &do_config_set,
      .argtable = &config_set_args,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── dump_channels ───────────────────────────────────────────────────────── */

static const char *channel_state_str(int s) {
  switch (s) {
  case channel_state_t_unknown:
    return "unknown";
  case channel_state_t_closing:
    return "closing";
  case channel_state_t_closed:
    return "closed";
  case channel_state_t_opening:
    return "opening";
  case channel_state_t_open:
    return "open";
  case channel_state_t_comfort:
    return "comfort";
  case channel_state_t_partially_open:
    return "partial";
  case channel_state_t_obstruction:
    return "obstruction";
  case channel_state_t_in_motion:
    return "in_motion";
  default:
    return "?";
  }
}

static int do_dump_channels(int argc, char **argv) {
  config_lock();
  printf("%-10s  %-20s  %-5s  %-12s  %5s  %5s  %7s\n", "Serial", "Name",
         "Proto", "State", "RSSI", "Pos", "Counter");
  for (int i = 0; i < g_config.channels_len; i++) {
    struct cosmo_channel_t *ch = &g_config.channels[i];
    char pos[8];
    if (ch->has_position)
      snprintf(pos, sizeof(pos), "%d%%", (int)ch->position);
    else
      strlcpy(pos, "n/a", sizeof(pos));

    printf("0x%08" PRIX32 "  %-20.*s  %-5s  %-12s  %5d  %5s  %7u\n", ch->serial,
           (int)sstr_length(ch->name), sstr_cstr(ch->name),
           sstr_cstr(ch->proto), channel_state_str(ch->state), (int)ch->rssi,
           pos, (unsigned)ch->counter);
  }
  if (g_config.channels_len == 0)
    printf("(no channels)\n");
  config_unlock();
  return 0;
}

static void register_dump_channels(void) {
  const esp_console_cmd_t cmd = {
      .command = "dump_channels",
      .help = "Print all channels and their current state",
      .hint = NULL,
      .func = &do_dump_channels,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ── shell task ──────────────────────────────────────────────────────────── */

static void usb_shell_task(void *arg) {
  esp_console_repl_t *repl = NULL;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = PROMPT_STR ">";
  repl_config.max_cmdline_length = 256;

  esp_console_dev_usb_serial_jtag_config_t hw_config =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

  ESP_ERROR_CHECK(
      esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

  register_reboot();
  register_wifi_connect();
  register_wifi_reset();
  register_wifi_scan();
  register_status();
  register_radio_tx();
  register_dump_config();
  register_config_set();
  register_dump_channels();

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
  vTaskDelete(NULL);
}

/* ── console_init ────────────────────────────────────────────────────────── */

void console_init(void) {
  if (xTaskCreate(usb_shell_task, "console", 4096, NULL, 3, NULL) != pdPASS)
    ESP_LOGE(TAG, "Failed to create console task");
}
