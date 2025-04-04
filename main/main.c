/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "esp_rom_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_board_init.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
//#include "soc/gpio_num.h"
#include "speech_commands_action.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_system.h>
#include <sys/_types.h>
typedef struct {
  int flag_plug_1;
  int flag_plug_2;
  int flag_plug_3;
} flag_device_t;
static const int RX_BUF_SIZE = 1024;

#define TXD_PIN (GPIO_NUM_43)
#define RXD_PIN (GPIO_NUM_44)
#define UART UART_NUM_2
#define GPIO_LED GPIO_NUM_1
void led_config() {
  gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_LED, 1);
}
void uart_init(void);
void send_uart_command(const char *device1, const char *device2,
                       const char *device3);
int string_cmp(char str1[], char str2[]);
int detect_flag = 0;
static esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;
srmodel_list_t *models = NULL;
static int play_voice = -2;

void feed_Task(void *arg) {
  esp_afe_sr_data_t *afe_data = arg;
  int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
  int nch = afe_handle->get_channel_num(afe_data);
  int feed_channel = esp_get_feed_channel();
  assert(nch <= feed_channel);
  int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
  assert(i2s_buff);

  while (task_flag) {
    esp_get_feed_data(false, i2s_buff,
                      audio_chunksize * sizeof(int16_t) * feed_channel);

    afe_handle->feed(afe_data, i2s_buff);
  }
  if (i2s_buff) {
    free(i2s_buff);
    i2s_buff = NULL;
  }
  vTaskDelete(NULL);
}

void detect_Task(void *arg) {
  uart_init();
  led_config();
  esp_afe_sr_data_t *afe_data = arg;
  int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
  char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  printf("multinet:%s\n", mn_name);
  esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
  model_iface_data_t *model_data = multinet->create(mn_name, 6000);
  int mu_chunksize = multinet->get_samp_chunksize(model_data);
  esp_mn_commands_update_from_sdkconfig(
      multinet, model_data); // Add speech commands from sdkconfig
  assert(mu_chunksize == afe_chunksize);
  // print active speech commands
  multinet->print_active_speech_commands(model_data);

  flag_device_t flag_detect = {
      .flag_plug_1 = 0, .flag_plug_2 = 0, .flag_plug_3 = 0};
  printf("------------detect start------------\n");
  while (task_flag) {
    afe_fetch_result_t *res = afe_handle->fetch(afe_data);
    if (!res || res->ret_value == ESP_FAIL) {
      printf("fetch error!\n");
      break;
    }

    if (res->wakeup_state == WAKENET_DETECTED) {
      printf("WAKEWORD DETECTED\n");
      multinet->clean(model_data);
    } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
      play_voice = -1;
      detect_flag = 1;
      printf("AFE_FETCH_CHANNEL_VERIFIED, channel index: %d\n",
             res->trigger_channel_id);
      // afe_handle->disable_wakenet(afe_data);
      // afe_handle->disable_aec(afe_data);
      gpio_set_level(GPIO_LED, 0);
    }

    if (detect_flag == 1) {
      esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

      if (mn_state == ESP_MN_STATE_DETECTING) {
        continue;
      }

      if (mn_state == ESP_MN_STATE_DETECTED) {
        esp_mn_results_t *mn_result = multinet->get_results(model_data);
        for (int i = 0; i < mn_result->num; i++) {

          char *Device = (char *)malloc(100);
          printf(
              "TOP %d, command_id: %d, phrase_id: %d, string: %s, prob: %f\n",
              i + 1, mn_result->command_id[i], mn_result->phrase_id[i],
              mn_result->string, mn_result->prob[i]);
          if (string_cmp(mn_result->string, "TkN nN PLcG WcN") == 1 ||
              string_cmp(mn_result->string, "Mb b MeT") == 1) {
            flag_detect.flag_plug_1 = 1;
            if (flag_detect.flag_plug_1 == 1 && flag_detect.flag_plug_2 == 0 &&
                flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "0", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "1", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "0", "1");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "1", "1");
            }

          } else if (string_cmp(mn_result->string, "TkN nN PLcG To") == 1 ||
                     string_cmp(mn_result->string, "Mb b hi") == 1) {
            flag_detect.flag_plug_2 = 1;
            if (flag_detect.flag_plug_1 == 0 && flag_detect.flag_plug_2 == 1 &&
                flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "1", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "1", "0");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "1", "1");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "1", "1");
            }

          } else if (string_cmp(mn_result->string, "TkN nN PLcG vRm") == 1 ||
                     string_cmp(mn_result->string, "Mb b Bn") == 1) {
            flag_detect.flag_plug_3 = 1;
            if (flag_detect.flag_plug_1 == 0 && flag_detect.flag_plug_2 == 0 &&
                flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "0", "1");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "0", "1");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "1", "1");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "1", "1");
            }

          } else if (string_cmp(mn_result->string, "TkN eF PLcG WcN") == 1 ||
                     string_cmp(mn_result->string, "TaT b MeT") == 1) {
            flag_detect.flag_plug_1 = 0;
            if (flag_detect.flag_plug_1 == 0 && flag_detect.flag_plug_2 == 0 &&
                flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "0", "0");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "1", "0");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "0", "1");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "1", "1");
            }

          } else if (string_cmp(mn_result->string, "TkN eF PLcG To") == 1 ||
                     string_cmp(mn_result->string, "TaT b hi") == 1) {
            flag_detect.flag_plug_2 = 0;
            if (flag_detect.flag_plug_1 == 0 && flag_detect.flag_plug_2 == 0 &&
                flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "0", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "0", "0");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("0", "0", "1");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 1) {
              send_uart_command("1", "0", "1");
            }

          } else if (string_cmp(mn_result->string, "TkN eF PLcG vRm") == 1 ||
                     string_cmp(mn_result->string, "TaT b Bn") == 1) {
            flag_detect.flag_plug_3 = 0;
            if (flag_detect.flag_plug_1 == 0 && flag_detect.flag_plug_2 == 0 &&
                flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "0", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "1", "0");
            } else if (flag_detect.flag_plug_1 == 1 &&
                       flag_detect.flag_plug_2 == 0 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("1", "0", "0");
            } else if (flag_detect.flag_plug_1 == 0 &&
                       flag_detect.flag_plug_2 == 1 &&
                       flag_detect.flag_plug_3 == 0) {
              send_uart_command("0", "1", "0");
            }
          } else if (string_cmp(mn_result->string, "TkN nN eL PLcGZ") ||
                     string_cmp(mn_result->string, "Mb TaT Kc")) {
            flag_detect.flag_plug_1 = 1;
            flag_detect.flag_plug_2 = 1;
            flag_detect.flag_plug_3 = 1;
            send_uart_command("1", "1", "1");
          } else if (string_cmp(mn_result->string, "TkN eF eL PLcGZ") ||
                     string_cmp(mn_result->string, "TaT TaT Kc")) {
            flag_detect.flag_plug_1 = 0;
            flag_detect.flag_plug_2 = 0;
            flag_detect.flag_plug_3 = 0;
            send_uart_command("0", "0", "0");
          }
        }
        printf("-----------listening-----------\n");
      }

      if (mn_state == ESP_MN_STATE_TIMEOUT) {
        esp_mn_results_t *mn_result = multinet->get_results(model_data);
        printf("timeout, string:%s\n", mn_result->string);
        afe_handle->enable_wakenet(afe_data);
        detect_flag = 0;
        printf("\n-----------awaits to be waken up-----------\n");
        gpio_set_level(GPIO_LED, 1);

        continue;
      }
    }
  }
  if (model_data) {
    multinet->destroy(model_data);
    model_data = NULL;
  }
  printf("detect exit\n");
  vTaskDelete(NULL);
}

void app_main() {
  models =
      esp_srmodel_init("model"); // partition label defined in partitions.csv
  ESP_ERROR_CHECK(esp_board_init(AUDIO_HAL_16K_SAMPLES, 1, 16));
  // ESP_ERROR_CHECK(esp_sdcard_init("/sdcard", 10));
#if defined CONFIG_ESP32_KORVO_V1_1_BOARD
  led_init();
#endif

#if CONFIG_IDF_TARGET_ESP32
  printf("This demo only support ESP32S3\n");
  return;
#else
  afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
#endif

  afe_config_t afe_config = AFE_CONFIG_DEFAULT();
  afe_config.wakenet_model_name =
      esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
  ;
#if defined CONFIG_ESP32_S3_BOX_BOARD || defined CONFIG_ESP32_S3_EYE_BOARD ||  \
    CONFIG_ESP32_S3_DEVKIT_C
  afe_config.aec_init = false;
#if defined CONFIG_ESP32_S3_EYE_BOARD || CONFIG_ESP32_S3_DEVKIT_C
  afe_config.pcm_config.total_ch_num = 2;
  afe_config.pcm_config.mic_num = 1;
  afe_config.pcm_config.ref_num = 1;
#endif
#endif
  esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(&afe_config);

  task_flag = 1;

  xTaskCreatePinnedToCore(&detect_Task, "detect", 8 * 1024, (void *)afe_data, 5,
                          NULL, 1);
  xTaskCreatePinnedToCore(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5,
                          NULL, 0);
#if defined CONFIG_ESP32_S3_KORVO_1_V4_0_BOARD
  xTaskCreatePinnedToCore(&led_Task, "led", 2 * 1024, NULL, 5, NULL, 0);
#endif
#if defined CONFIG_ESP32_S3_KORVO_1_V4_0_BOARD ||                              \
    CONFIG_ESP32_S3_KORVO_2_V3_0_BOARD || CONFIG_ESP32_KORVO_V1_1_BOARD ||     \
    CONFIG_ESP32_S3_BOX_BOARD
  xTaskCreatePinnedToCore(&play_music, "play", 4 * 1024, NULL, 5, NULL, 1);
#endif

  // // You can call afe_handle->destroy to destroy AFE.
  // task_flag = 0;

  // printf("destroy\n");
  // afe_handle->destroy(afe_data);
  // afe_data = NULL;
  // printf("successful\n");
}
// Set up for uart
void uart_init(void) {
  const uart_config_t uart_config = {
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_APB,
  };

  // We won't use a buffer for sending data.
  uart_driver_install(UART, RX_BUF_SIZE * 2, 0, 0, NULL, 0);
  uart_param_config(UART, &uart_config);
  uart_set_pin(UART, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

void send_uart_command(const char *device1, const char *device2,
                       const char *device3) {
  cJSON *dataout = cJSON_CreateObject();
  cJSON_AddStringToObject(dataout, "plug1", device1);
  cJSON_AddStringToObject(dataout, "plug2", device2);
  cJSON_AddStringToObject(dataout, "plug3", device3);

  char *json_string = cJSON_PrintUnformatted(dataout);
  uart_write_bytes(UART, json_string, strlen(json_string));
  printf("\n");

  // Giải phóng bộ nhớ
  cJSON_Delete(dataout);
  free(json_string);
}

int string_cmp(char str1[], char str2[]) {
  int result = strcmp(str1, str2);
  if (result == 0) {
    return 1;
  } else {
    return 0;
  }
}
