#include "f_adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "f_adc";

struct f_adc {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    uint16_t configured_channels; /* bitmask of already-configured channels */
};

esp_err_t f_adc_init(f_adc_handle_t *handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    f_adc_handle_t h = calloc(1, sizeof(struct f_adc));
    if (h == NULL) return ESP_ERR_NO_MEM;

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = ADC_UNIT_1,
        .clk_src  = ADC_DIGI_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &h->unit));

    /* Create calibration handle */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id  = ADC_UNIT_1,
            .chan     = ADC_CHANNEL_0,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &h->cali) == ESP_OK) {
            ESP_LOGI(TAG, "ADC calibration: curve fitting");
        }
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id  = ADC_UNIT_1,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        if (adc_cali_create_scheme_line_fitting(&cali_cfg, &h->cali) == ESP_OK) {
            ESP_LOGI(TAG, "ADC calibration: line fitting");
        }
    }
#endif
    if (h->cali == NULL) {
        ESP_LOGW(TAG, "ADC calibration not available, using linear fallback");
    }

    *handle = h;
    ESP_LOGI(TAG, "ADC oneshot initialized (ADC1)");
    return ESP_OK;
}

esp_err_t f_adc_read_raw(f_adc_handle_t handle, uint8_t gpio, int *raw_out)
{
    if (handle == NULL || raw_out == NULL) return ESP_ERR_INVALID_ARG;

    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel((int)gpio, &unit, &channel) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (unit != ADC_UNIT_1) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (!(handle->configured_channels & (1 << (unsigned)channel))) {
        ESP_ERROR_CHECK(adc_oneshot_config_channel(handle->unit, channel, &chan_cfg));
        handle->configured_channels |= (1 << (unsigned)channel);
    }
    ESP_ERROR_CHECK(adc_oneshot_read(handle->unit, channel, raw_out));
    return ESP_OK;
}

esp_err_t f_adc_raw_to_voltage(f_adc_handle_t handle, int raw, uint16_t vref_mv, float *voltage_out)
{
    if (handle == NULL || voltage_out == NULL) return ESP_ERR_INVALID_ARG;

    if (handle->cali) {
        int voltage_mv;
        esp_err_t err = adc_cali_raw_to_voltage(handle->cali, raw, &voltage_mv);
        if (err != ESP_OK) return err;
        *voltage_out = (float)voltage_mv / 1000.0f;
    } else {
        /* Linear fallback */
        *voltage_out = (float)raw * (float)vref_mv / 4095.0f / 1000.0f;
    }
    return ESP_OK;
}

esp_err_t f_adc_ntc_temp(float voltage, float vcc, float r_divider, float beta, float r0,
                         float t0_k, float *temp_c_out)
{
    if (temp_c_out == NULL || voltage >= vcc) return ESP_ERR_INVALID_ARG;
    /* Voltage divider: Vout = Vcc * R_ntc / (R_divider + R_ntc) */
    float r_ntc = r_divider * voltage / (vcc - voltage);
    /* Beta equation: 1/T = 1/T0 + 1/B * ln(R/R0) */
    float inv_t = 1.0f / t0_k + (1.0f / beta) * logf(r_ntc / r0);
    *temp_c_out = 1.0f / inv_t - 273.15f;
    return ESP_OK;
}
