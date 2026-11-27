/* Host mock of power_monitor.h (no gauge: the ADC/N-C path is taken). */
#ifndef POWER_GATE_POWER_MONITOR_H
#define POWER_GATE_POWER_MONITOR_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    POWER_MONITOR_CHARGE_UNKNOWN = 0,
    POWER_MONITOR_CHARGE_CHARGING,
    POWER_MONITOR_CHARGE_DISCHARGING,
    POWER_MONITOR_CHARGE_IDLE,
    POWER_MONITOR_CHARGE_FULL,
} power_monitor_charge_t;
typedef struct {
    int bus_mv;
    int shunt_uv;
    int current_raw;
    int current_ma;
    int power_mw;
    uint16_t config;
    uint16_t cal;
    int chg_stat;
} power_monitor_diag_t;
esp_err_t power_monitor_init(void);
bool power_monitor_available(void);
power_monitor_charge_t power_monitor_classify_charge(int pack_mv, int current_ma);
const char *power_monitor_charge_name(power_monitor_charge_t state);
bool power_monitor_pack_present(int pack_mv, int current_ma, const int *recent_mv,
                                int recent_count, int chg_stat, int chg_stat_prev);
esp_err_t power_monitor_battery_read(int *pack_mv_out, int *percent_out, bool *charging_out);
esp_err_t power_monitor_read_sample(int *pack_mv_out, int *percent_out,
                                    int *current_ma_out, int *power_mw_out,
                                    bool *charging_out);
esp_err_t power_monitor_read_diag(power_monitor_diag_t *out);
#ifdef __cplusplus
}
#endif
#endif
