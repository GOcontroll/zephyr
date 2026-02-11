/*
 * Copyright (C) 2025 Metratec GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(modem_sim7600_gps, CONFIG_MODEM_LOG_LEVEL);

#include "sim7600.h"

static struct gnss_data gnss_data = {0};

/**
 * Parses cgnsinf response into the gnss_data structure.
 *
 * @param gps_buf Null terminated buffer containing the response.
 * @return 0 on successful parse. Otherwise <0 is returned.
 */
static int parse_cgpsinfo(char *gps_buf)
{
	char* comma_split = NULL;
	char *data_end, *data_comma, *token;
	int64_t longitude, latitude;
	int32_t altitude;
	uint32_t velocity, bearing;
	int temp_int;
	uint8_t year, month, day, hour, minute;
	uint16_t millisecond;

	token = strtok_r(gps_buf, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (strnlen(token, 11) >= 4) {
		latitude = (((int64_t)(strtof(token + 2, NULL) * 1000000)) * 1000) / 60;
		/* divide by 100 to shift the number right 2 decimal places */
		latitude = latitude + ((strtoll(token, NULL, 10) / 100))*1000000000;
	} else {
		/* incomplete message */
		goto no_msg;
	}
	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (!strcmp(token, "S")) latitude = -latitude;

	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (strnlen(token, 12) >= 5) {
		longitude = (((int64_t)(strtof(token + 3, NULL) * 1000000)) * 1000) / 60;
		/* divide by 100 to shift the number right 2 decimal places */
		longitude = longitude + ((strtoll(token, NULL, 10) / 100))*1000000000;
	}
	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (!strcmp(token, "W")) longitude = -longitude;

	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (strnlen(token, 6) == 6) {
		temp_int = atoi(token);
		year = (temp_int % 100);
		month = (temp_int % 10000) / 100;
		day = temp_int / 10000;
	}

	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	if (strnlen(token, 6) == 6) {
		temp_int = atoi(token);
		millisecond = (temp_int % 100)*1000;
		minute = (temp_int % 10000) / 100;
		hour = temp_int / 10000;
	}

	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	altitude = (int32_t)(strtof(token, NULL)*1000);

	/* velocity is in knots for some reason */
	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL) goto no_msg;
	velocity = (uint32_t)(strtof(token, NULL) * 514.44444444);

	token = strtok_r(NULL, ",", &comma_split);
	if (token == NULL)
		bearing = 0;
	if (token[0] == '\r')
		bearing = 0;
	bearing = (uint32_t)(strtof(token, NULL) * 1000);

	gnss_data.nav_data.longitude = longitude;
	gnss_data.nav_data.latitude = latitude;
	gnss_data.nav_data.altitude = altitude;
	gnss_data.nav_data.speed = velocity;
	gnss_data.nav_data.bearing = bearing;
	gnss_data.utc.century_year = year;
	gnss_data.utc.month = month;
	gnss_data.utc.month_day = day;
	gnss_data.utc.hour = hour;
	gnss_data.utc.minute = minute;
	gnss_data.utc.millisecond = millisecond;
	gnss_data.info.fix_status = GNSS_FIX_STATUS_GNSS_FIX;
	return 0;

no_msg:
	memset(&gnss_data, 0, sizeof(struct gnss_data));
	return -ENODATA;
}

/*
 * Parses the +CGPSINFO Gnss response.
 *
 * The CGPSINFO command has the following parameters but
 * not all parameters are set by the module:
 *
 * +CGPSINFO: [<lat>],[<N/S>],[<log>],[<E/W>],[<date>],[<UTC time>],[<alt>],
 *            [<speed>],[<course>]
 *
 */
MODEM_CMD_DEFINE(on_cmd_cgpsinfo)
{
	char gps_buf[MDM_GNSS_PARSER_MAX_LEN];
	size_t out_len = net_buf_linearize(gps_buf, sizeof(gps_buf) - 1, data->rx_buf, 0, len);

	gps_buf[out_len] = '\0';
	return parse_cgpsinfo(gps_buf);
}

int mdm_sim7600_query_gnss(struct gnss_data *data)
{
	int ret;
	struct modem_cmd cmds[] = { MODEM_CMD("+CGPSINFO: ", on_cmd_cgpsinfo, 9U, ",") };

	if (sim7600_get_gnss_state() != SIM7600_GNSS_STATE_ON) {
		LOG_ERR("GNSS functionality is not enabled!!");
		return -1;
	}

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, cmds, ARRAY_SIZE(cmds), "AT+CGPSINFO",
				 &mdata.sem_response, K_SECONDS(2));
	if (ret < 0) {
		return ret;
	}

	if (!gnss_data.info.fix_status) {
		return -EAGAIN;
	}

	if (data) {
		memcpy(data, &gnss_data, sizeof(gnss_data));
	}

	return ret;
}

static int sim7600_start_gnss_ext(bool xtra)
{
	int ret = -EALREADY;

	if (sim7600_get_gnss_state() == SIM7600_GNSS_STATE_ON) {
		LOG_WRN("Modem already in gnss state");
		goto out;
	}

	/* Power GNSS unit */
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CGPS=1",
				 &mdata.sem_response, K_SECONDS(2));
	if (ret < 0) {
		LOG_ERR("Failed to power on gnss: %d", ret);
		goto out;
	}

	if (xtra) {
		/* Enable xtra functionality */
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CGPSXE=1",
					&mdata.sem_response, K_SECONDS(5));
		if (ret < 0) {
			LOG_WRN("Failed to enable xtra. Using regular gps");
		}
	}

	sim7600_change_gnss_state(SIM7600_GNSS_STATE_ON);
out:
	return ret;
}

int mdm_sim7600_start_gnss(void)
{
	return sim7600_start_gnss_ext(false);
}

int mdm_sim7600_start_gnss_xtra(void)
{
	return sim7600_start_gnss_ext(true);
}

int mdm_sim7600_stop_gnss(void)
{
	int ret = -EINVAL;

	if (sim7600_get_gnss_state() != SIM7600_GNSS_STATE_ON) {
		LOG_WRN("GNSS not started");
		goto out;
	}

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CGPS=0",
				 &mdata.sem_response, K_SECONDS(2));
	if (ret < 0) {
		LOG_ERR("Failed to power off gnss: %d", ret);
		goto out;
	}

	sim7600_change_gnss_state(SIM7600_GNSS_STATE_OFF);
out:
	return ret;
}
