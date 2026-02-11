/** @file
 * @brief Simcom SIM7600 modem public API header file.
 *
 * Copyright (C) 2021 metraTec GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MODEM_SIMCOM_SIM7600_H
#define ZEPHYR_INCLUDE_DRIVERS_MODEM_SIMCOM_SIM7600_H

#include <zephyr/types.h>
#include <zephyr/drivers/gnss.h>

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum Length of GNSS UTC data */
#define SIM7600_GNSS_DATA_UTC_LEN 20
/** Maximum SMS length */
#define SIM7600_SMS_MAX_LEN 160
/** Maximum UE system information band size */
#define SIM7600_UE_SYS_INFO_BAND_SIZE 32
/** Maximum number of DNS retries */
#define SIM7600_DNS_MAX_RECOUNT 10
/** Maximum timeout for DNS queries in milliseconds */
#define SIM7600_DNS_MAX_TIMEOUT_MS 60000

/** Sim7600 modem state */
enum sim7600_state {
	SIM7600_STATE_INIT = 0, /**< Initial modem state */
	SIM7600_STATE_IDLE, /**< Modem idle */
	SIM7600_STATE_NETWORKING, /**< Network active */
	SIM7600_STATE_OFF, /**< Modem off */
};

enum sim7600_gnss_state {
	SIM7600_GNSS_STATE_INIT = 0,
	SIM7600_GNSS_STATE_ON,
	SIM7600_GNSS_STATE_OFF,
};

/** Sim7600 gnss data structure */
struct sim7600_gnss_data {
	struct gnss_info info;
	struct gnss_time utc;
	struct navigation_data nav_data;
};

/** Possible ftp return codes. */
enum sim7600_ftp_rc {
	SIM7600_FTP_RC_OK = 0, /**< Operation finished correctly. */
	SIM7600_FTP_RC_FINISHED, /**< Session finished. */
	SIM7600_FTP_RC_ERROR, /**< An error occurred. */
};

/** UE system mode */
enum sim7600_ue_sys_mode {
	SIM7600_UE_SYS_MODE_NO_SERVICE, /**< No service */
	SIM7600_UE_SYS_MODE_GSM, /**< GSM */
	SIM7600_UE_SYS_MODE_LTE_CAT_M1, /**< LTE CAT M1 */
	SIM7600_UE_SYS_MODE_LTE_NB_IOT, /**< LTE NB IOT */
};

/** UE operating mode */
enum sim7600_ue_op_mode {
	SIM7600_UE_OP_MODE_ONLINE, /**< Online */
	SIM7600_UE_OP_MODE_OFFLINE, /**< Offline */
	SIM7600_UE_OP_MODE_FACTORY_TEST_MODE, /**< Factory test mode */
	SIM7600_UE_OP_MODE_RESET, /**< Reset */
	SIM7600_UE_OP_MODE_LOW_POWER_MODE, /**< Low power mode */
};

/**
 * Sim7600 ue system information structure for gsm.
 */
struct sim7600_ue_sys_info_gsm {
	/** Mobile country code */
	uint16_t mcc;
	/** Mobile network code */
	uint16_t mcn;
	/** Location area code */
	uint16_t lac;
	/** Cell ID */
	uint16_t cid;
	/** Absolute radio frequency channel number */
	uint8_t arfcn[SIM7600_UE_SYS_INFO_BAND_SIZE + 1];
	/** RX level in dBm */
	int16_t rx_lvl;
	/** Track LO adjust */
	int16_t track_lo_adjust;
	/** C1 coefficient */
	uint16_t c1;
	/** C2 coefficient */
	uint16_t c2;
};

/**
 * Sim7600 ue system information structure for LTE.
 */
struct sim7600_ue_sys_info_lte {
	/** Mobile country code */
	uint16_t mcc;
	/** Mobile network code */
	uint16_t mcn;
	/** Tracing area code */
	uint16_t tac;
	/** Serving Cell ID */
	uint32_t sci;
	/** Physical Cell ID */
	uint16_t pci;
	/** Frequency band */
	uint8_t band[SIM7600_UE_SYS_INFO_BAND_SIZE + 1];
	/** E-UTRA absolute radio frequency channel number */
	uint16_t earfcn;
	/** Downlink bandwidth in MHz */
	uint16_t dlbw;
	/** Uplink bandwidth in MHz */
	uint16_t ulbw;
	/** Reference signal received quality in dB */
	int16_t rsrq;
	/** Reference signal received power in dBm */
	int16_t rsrp;
	/** Received signal strength indicator in dBm */
	int16_t rssi;
	/** Reference signal signal to noise ratio in dB */
	int16_t rssnr;
	/** Signal to interference plus noise ratio in dB */
	int16_t sinr;
};

/**
 * Sim7600 ue system information structure.
 */
struct sim7600_ue_sys_info {
	/** Refer to sim7600_ue_sys_mode */
	enum sim7600_ue_sys_mode sys_mode;
	/** Refer to sim7600_ue_op_mode */
	enum sim7600_ue_op_mode op_mode;
	union {
		/** Only set if sys_mode is GSM */
		struct sim7600_ue_sys_info_gsm gsm;
		/** Only set if sys mode is LTE CAT-M1/NB-IOT */
		struct sim7600_ue_sys_info_lte lte;
	} cell; /**< Cell information */
};

/**
 * Get the current state of the modem.
 *
 * @return The current state.
 */
enum sim7600_state mdm_sim7600_get_state(void);

/**
 * @brief Power on the Sim7600.
 *
 * @return 0 on success. Otherwise -1 is returned.
 */
int mdm_sim7600_power_on(void);

/**
 * @brief Power off the Sim7600.
 *
 * @return 0 on success. Otherwise -1 is returned.
 */
int mdm_sim7600_power_off(void);

/**
 * Forcefully reset the modem by pulling pwrkey for 15 seconds.
 * @note The state of the modem may be undefined after calling
 * this function. Call mdm_sim7600_power_on after force reset.
 */
void mdm_sim7600_force_reset(void);

/**
 * @brief Activates the network operation mode of the modem.
 *
 * @return 0 on success. Otherwise <0 is returned.
 * @note The modem needs to be booted for this function to work.
 * Concurrent use of network and gnss is not possible.
 */
int mdm_sim7600_start_network(void);

/**
 * @brief Stops the networking operation mode of the modem.
 *
 * @return 0 on success. Otherwise <0 is returned.
 */
int mdm_sim7600_stop_network(void);

/**
 * @brief Starts the modem in gnss operation mode.
 *
 * @return 0 on success. Otherwise <0 is returned.
 * @note The modem needs to be booted for this function to work.
 * Concurrent use of network and gnss is not possible.
 */
int mdm_sim7600_start_gnss(void);

/**
 * @brief Starts the modem in gnss operation mode with xtra functionality.
 *
 * @return 0 on success. Otherwise <0 is returned.
 * @note The modem needs to be booted for this function to work.
 * Concurrent use of network and gnss is not possible.
 * @note If enabling xtra functionality fails a normal cold start will be performed.
 */
int mdm_sim7600_start_gnss_xtra(void);

/**
 * @brief Stops the modem gnss operation mode.
 *
 * @return 0 on success. Otherwise <0 is returned.
 */
int mdm_sim7600_stop_gnss(void);

/**
 * @brief Download the XTRA file for assisted gnss.
 *
 * @param server_id Id of the server to download XTRA file from.
 * @param f_name The name of the XTRA file to download.
 * @return 0 on success. Otherwise <0 is returned.
 */
int mdm_sim7600_download_xtra(uint8_t server_id, const char *f_name);

/**
 * @brief Query the validity of the XTRA file.
 *
 * @param diff_h Difference between the local time and the XTRA inject time in hours.
 * @param duration_h Valid time of the XTRA file in hours.
 * @param inject Injection time of the XTRA file.
 */
int mdm_sim7600_query_xtra_validity(int16_t *diff_h, int16_t *duration_h, struct tm *inject);

/**
 * @brief Query gnss position form the modem.
 *
 * @return 0 on success. If no fix is acquired yet -EAGAIN is returned.
 *         Otherwise <0 is returned.
 */
int mdm_sim7600_query_gnss(struct gnss_data *data);

/**
 * Get the sim7600 manufacturer.
 */
const char *mdm_sim7600_get_manufacturer(void);

/**
 * Get the sim7600 model information.
 */
const char *mdm_sim7600_get_model(void);

/**
 * Get the sim7600 revision.
 */
const char *mdm_sim7600_get_revision(void);

/**
 * Get the sim7600 imei number.
 */
const char *mdm_sim7600_get_imei(void);

/**
 * Get the sim7600 iccid number.
 */
const char *mdm_sim7600_get_iccid(void);

/**
 * Set the level of one of the module's GPIO pins
 *
 * @param gpio GPIO pin number
 * @param level New logical level of the GPIO
 * @return 0 on success. Otherwise -1 is returned.
 *
 * @note The GPIO will be configured as output implicitly.
 */
int mdm_sim7600_set_gpio(int gpio, int level);

/**
 * Start a ftp get session.
 *
 * @param server The ftp servers address.
 * @param user User name for the ftp server.
 * @param passwd Password for the ftp user.
 * @param file File to be downloaded.
 * @param path Path to the file on the server.
 * @return 0 if the session was started. Otherwise -1 is returned.
 */
int mdm_sim7600_ftp_get_start(const char *server, const char *user, const char *passwd,
				  const char *file, const char *path);

/**
 * Read data from a ftp get session.
 *
 * @param dst The destination buffer.
 * @param size Initialize to the size of dst. Gets set to the number
 *             of bytes actually read.
 * @return According sim7600_ftp_rc.
 */
int mdm_sim7600_ftp_get_read(char *dst, size_t *size);

/**
 * Read voltage, charge status and battery connection level.
 *
 * @param bcs [out] Charge status.
 * @param bcl [out] Battery connection level.
 * @param voltage [out] Battery voltage in mV.
 * @return 0 on success. Otherwise a negative error is returned.
 */
int mdm_sim7600_get_battery_charge(uint8_t *bcs, uint8_t *bcl, uint16_t *voltage);

/**
 * Read the ue system information
 *
 * @param info Destination buffer for information.
 * @return 0 on success. Otherwise a negative error is returned.
 */
int mdm_sim7600_get_ue_sys_info(struct sim7600_ue_sys_info *info);

/**
 * Get the local time of the modem.
 *
 * @param t Time structure to fill.
 * @return 0 on success. Otherwise a negative error is returned.
 * @note Time is set by network. It may take some time for it to get valid.
 */
int mdm_sim7600_get_local_time(struct tm *t);

/**
 * Set the dns query lookup parameters.
 *
 * @param recount Number of retries per query.
 *				  Maximum @c SIM7600_DNS_MAX_RECOUNT
 * @param timeout Timeout for a dns query in milliseconds.
 *				  Maximum @c SIM7600_DNS_MAX_TIMEOUT_MS
 * @return 0 on success. Otherwise a negative error is returned.
 */
int mdm_sim7600_dns_set_lookup_params(uint8_t recount, uint16_t timeout);

/**
 * Get the dns query lookup parameters.
 *
 * @param recount [out] Number of retries per query.
 * @param timeout [out] Timeout for a dns query in milliseconds.
 */
void mdm_sim7600_dns_get_lookup_params(uint8_t *recount, uint16_t *timeout);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_MODEM_SIMCOM_SIM7600_H */
