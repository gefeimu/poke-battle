// components/bsp/src/bsp_button.c
// 官方 BSP 的 ADC 采样路径(裁剪说明见 bsp_button.h)。
#include "bsp_button.h"
#include "bsp_pins.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "bsp_btn";

// 与官方一致:量程约 0~3100mV,覆盖松开态。
#define BSP_BTN_ATTEN ADC_ATTEN_DB_12

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;

esp_err_t bsp_button_init(void)
{
    if (s_adc && s_cali) {
        return ESP_OK;
    }
    // 半初始化状态不覆盖:先释放干净再重试,避免拿到 "adc1 is already in use"。
    if (s_adc || s_cali) {
        return ESP_ERR_INVALID_STATE;
    }

    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BSP_BTN_ADC_UNIT };
    esp_err_t e = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit 创建失败 (%s)", esp_err_to_name(e));
        s_adc = NULL;
        return e;
    }

    const adc_oneshot_chan_cfg_t ch = {
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    e = adc_oneshot_config_channel(s_adc, BSP_BTN_ADC_CHANNEL, &ch);
    if (e != ESP_OK) {
        goto fail;
    }

    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = BSP_BTN_ADC_UNIT,
        .chan = BSP_BTN_ADC_CHANNEL,
        .atten = BSP_BTN_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    e = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    if (e != ESP_OK) {
        // 校准不可用时返回错误,不把原始码猜成mV —— 猜错的方向是「一直按住上键」。
        ESP_LOGE(TAG, "ADC 校准创建失败: %s", esp_err_to_name(e));
        s_cali = NULL;
        goto fail;
    }

    ESP_LOGI(TAG, "按键 ADC 就绪:ADC1_CH%d", BSP_BTN_ADC_CHANNEL);
    return ESP_OK;

fail:
    if (s_cali) {
        (void)adc_cali_delete_scheme_curve_fitting(s_cali);
        s_cali = NULL;
    }
    if (s_adc) {
        (void)adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
    }
    return e;
}

int bsp_button_read_mv(void)
{
    if (!s_adc || !s_cali) {
        return -1;
    }
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BSP_BTN_ADC_CHANNEL, &raw) != ESP_OK) {
        return -1;
    }
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
        return -1;
    }
    return mv;
}
