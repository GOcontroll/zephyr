/*
 * Copyright (C) 2021 metraTec GmbH
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT simcom_sim7600

#include <zephyr/logging/log.h>
#include <zephyr/net/offloaded_netdev.h>
LOG_MODULE_REGISTER(modem_simcom_sim7600, CONFIG_MODEM_LOG_LEVEL);

#include <zephyr/drivers/modem/simcom-sim7600.h>
#include "sim7600.h"

struct sim7600_data mdata;
struct modem_context mctx;

static struct k_thread modem_rx_thread;
struct k_work_q modem_workq;

static K_KERNEL_STACK_DEFINE(modem_rx_stack, CONFIG_MODEM_SIMCOM_SIM7600_RX_STACK_SIZE);
static K_KERNEL_STACK_DEFINE(modem_workq_stack, CONFIG_MODEM_SIMCOM_SIM7600_RX_WORKQ_STACK_SIZE);
NET_BUF_POOL_DEFINE(mdm_recv_pool, MDM_RECV_MAX_BUF, MDM_RECV_BUF_SIZE, 0, NULL);

/* pin settings */
static const struct gpio_dt_spec power_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_power_gpios, {});
static const struct gpio_dt_spec disable_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_disable_gpios, {});
static const struct gpio_dt_spec reset_gpio = GPIO_DT_SPEC_INST_GET_OR(0, mdm_disable_gpios, {});

static inline uint8_t *modem_get_mac(const struct device *dev)
{
	return mdata.mac_addr;
}

/* Setup the Modem NET Interface. */
static void modem_net_iface_init(struct net_if *iface)
{
	const struct device *dev = net_if_get_device(iface);
	struct sim7600_data *data = dev->data;

	net_if_set_link_addr(iface, modem_get_mac(dev), sizeof(data->mac_addr), NET_LINK_ETHERNET);

	data->netif = iface;

	socket_offload_dns_register(&offload_dns_ops);

	net_if_socket_offload_set(iface, sim7600_offload_socket);
}

/**
 * Changes the operating state of the sim7600.
 *
 * @param state The new state.
 */
void sim7600_change_state(enum sim7600_state state)
{
	LOG_DBG("Changing state to (%d)", state);
	mdata.state = state;
}

/**
 * Get the current operating state of the sim7600.
 *
 * @return The current state.
 */
enum sim7600_state sim7600_get_state(void)
{
	return mdata.state;
}

enum sim7600_state mdm_sim7600_get_state(void)
{
	return sim7600_get_state();
}

static struct offloaded_if_api api_funcs = {
	.iface_api.init = modem_net_iface_init,
};

static bool offload_is_supported(int family, int type, int proto)
{
	if (family != AF_INET &&
	    family != AF_INET6) {
		return false;
	}

	if (type != SOCK_DGRAM &&
	    type != SOCK_STREAM) {
		return false;
	}

	if (proto != IPPROTO_TCP &&
	    proto != IPPROTO_UDP) {
		return false;
	}

	return true;
}

/*
 * Process all messages received from the modem.
 */
static void modem_rx(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		/* Wait for incoming data */
		modem_iface_uart_rx_wait(&mctx.iface, K_FOREVER);

		modem_cmd_handler_process(&mctx.cmd_handler, &mctx.iface);
	}
}

int mdm_sim7600_power_on(void)
{
	LOG_DBG("Powering on sim7600");
	if (gpio_is_ready_dt(&power_gpio)) {
		return gpio_pin_set_dt(&power_gpio, GPIO_OUTPUT_ACTIVE);
	}
	return -EIO;
}

int mdm_sim7600_power_off(void)
{
	int ret = -EALREADY;

	k_work_cancel_delayable(&mdata.rssi_query_work);

	if ((mdata.status_flags & SIM7600_STATUS_FLAG_POWER_ON) == 0) {
		LOG_WRN("Modem already off");
		goto out;
	}

	k_sem_reset(&mdata.boot_sem);

	/* Pull pwrkey to turn off */
	if (gpio_is_ready_dt(&power_gpio)) {
		gpio_pin_set_dt(&power_gpio, GPIO_OUTPUT_INACTIVE);
	}

	/* Wait for power down indication */
	ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
	if (ret != 0) {
		LOG_ERR("No power down indication");
		goto out;
	}

	if ((mdata.status_flags & SIM7600_STATUS_FLAG_POWER_ON) != 0) {
		LOG_ERR("Modem not powered down!");
		ret = -1;
		goto out;
	}

	LOG_DBG("Modem turned off");
	mdata.status_flags = 0;
	sim7600_change_state(SIM7600_STATE_OFF);

out:
	return ret;
}

void mdm_sim7600_force_reset(void)
{
	LOG_DBG("Forcefully resetting modem");
	mdm_sim7600_power_off();
	k_sleep(K_SECONDS(15));
	mdm_sim7600_power_on();
}

int mdm_sim7600_soft_reset(void)
{
	int ret;
	/* Reset modem and wait for ready indication */
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CFUN=1,1",
				&mdata.sem_response, K_MSEC(500));
	if (ret != 0) {
		LOG_ERR("Reset failed");
		return -1;
	}

	ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
	if (ret != 0)
		LOG_ERR("No RDY received!");
	return ret;
}

int mdm_sim7600_set_reset(void)
{
	LOG_DBG("Resetting sim7600");
	if (gpio_is_ready_dt(&reset_gpio)) {
		gpio_pin_set_dt(&reset_gpio, GPIO_OUTPUT_ACTIVE);
		mdata.status_flags = 0;
		sim7600_change_state(SIM7600_STATE_OFF);
	}
	return -EIO;
}

int mdm_sim7600_release_reset(void)
{
	LOG_DBG("releasing reset on sim7600");
	if (gpio_is_ready_dt(&reset_gpio)) {
		gpio_pin_set_dt(&reset_gpio, GPIO_OUTPUT_INACTIVE);
		sim7600_change_state(SIM7600_STATE_IDLE);
	}
	return -EIO;
}

int mdm_sim7600_set_disable(void)
{
	LOG_DBG("Disabling sim7600");
	if (gpio_is_ready_dt(&disable_gpio)) {
		return gpio_pin_set_dt(&disable_gpio, GPIO_OUTPUT_ACTIVE);
	}
	return -EIO;
}

int mdm_sim7600_release_disable(void)
{
	LOG_DBG("releasing disable on sim7600");
	if (gpio_is_ready_dt(&disable_gpio)) {
		return gpio_pin_set_dt(&disable_gpio, GPIO_OUTPUT_INACTIVE);
	}
	return -EIO;
}

MODEM_CMD_DEFINE(on_cmd_ok)
{
	modem_cmd_handler_set_error(data, 0);
	k_sem_give(&mdata.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_error)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

MODEM_CMD_DEFINE(on_cmd_exterror)
{
	modem_cmd_handler_set_error(data, -EIO);
	k_sem_give(&mdata.sem_response);
	return 0;
}

/*
 * Read manufacturer identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgmi)
{
	size_t out_len = net_buf_linearize(
		mdata.mdm_manufacturer, sizeof(mdata.mdm_manufacturer) - 1, data->rx_buf, 0, len);
	mdata.mdm_manufacturer[out_len] = '\0';
	LOG_INF("Manufacturer: %s", mdata.mdm_manufacturer);
	return 0;
}

/*
 * Read model identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgmm)
{
	size_t out_len = net_buf_linearize(mdata.mdm_model, sizeof(mdata.mdm_model) - 1,
					   data->rx_buf, 0, len);
	mdata.mdm_model[out_len] = '\0';
	LOG_INF("Model: %s", mdata.mdm_model);
	return 0;
}

/*
 * Read software release.
 *
 * Response will be in format RESPONSE: <revision>.
 */
MODEM_CMD_DEFINE(on_cmd_cgmr)
{
	size_t out_len;
	char *p;

	out_len = net_buf_linearize(mdata.mdm_revision, sizeof(mdata.mdm_revision) - 1,
				    data->rx_buf, 0, len);
	mdata.mdm_revision[out_len] = '\0';

	/* The module prepends a Revision: */
	p = strchr(mdata.mdm_revision, ':');
	if (p) {
		out_len = strlen(p + 1);
		memmove(mdata.mdm_revision, p + 1, out_len + 1);
	}

	LOG_INF("Revision: %s", mdata.mdm_revision);
	return 0;
}

/*
 * Read serial number identification.
 */
MODEM_CMD_DEFINE(on_cmd_cgsn)
{
	size_t out_len =
		net_buf_linearize(mdata.mdm_imei, sizeof(mdata.mdm_imei) - 1, data->rx_buf, 0, len);
	mdata.mdm_imei[out_len] = '\0';
	LOG_INF("IMEI: %s", mdata.mdm_imei);
	return 0;
}

/*
 * Read MAC address
 */
MODEM_CMD_DEFINE(on_cmd_cwmacaddr)
{
	size_t bytes_read;
	int ret;
	uint8_t temp_mac[19];
	uint8_t *mac = mdata.mac_addr;
	bytes_read = net_buf_linearize(temp_mac, sizeof(temp_mac), data->rx_buf, 0, len);
	if (bytes_read != 19)
		return -ENODATA;
	ret = sscanf(temp_mac, "0,%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
	if (ret != 6)
		return -EIO;
	return 0;
}

#if defined(CONFIG_MODEM_SIM_NUMBERS)
/*
 * Read international mobile subscriber identity.
 */
MODEM_CMD_DEFINE(on_cmd_cimi)
{
	size_t out_len =
		net_buf_linearize(mdata.mdm_imsi, sizeof(mdata.mdm_imsi) - 1, data->rx_buf, 0, len);
	mdata.mdm_imsi[out_len] = '\0';

	/* Log the received information. */
	LOG_INF("IMSI: %s", mdata.mdm_imsi);
	return 0;
}

/*
 * Read iccid.
 */
MODEM_CMD_DEFINE(on_cmd_ccid)
{
	size_t out_len = net_buf_linearize(mdata.mdm_iccid, sizeof(mdata.mdm_iccid) - 1,
					   data->rx_buf, 0, len);
	mdata.mdm_iccid[out_len] = '\0';

	/* Log the received information. */
	LOG_INF("ICCID: %s", mdata.mdm_iccid);
	return 0;
}
#endif /* defined(CONFIG_MODEM_SIM_NUMBERS) */

/*
 * Unlock the tx ready semaphore if '> ' is received.
 */
MODEM_CMD_DIRECT_DEFINE(on_cmd_tx_ready)
{
	k_sem_give(&mdata.sem_tx_ready);
	return len;
}

/*
 * Handles pdp context urc.
 *
 * The urc has the form +APP PDP: <index>,<state>.
 * When activated ACTIVE is reported as state.
 * All other states will be treated as deactivated.
 */
MODEM_CMD_DEFINE(on_urc_app_pdp)
{
	bool active = strcmp(argv[1], "ACTIVE") == 0;

	if (active) {
		mdata.status_flags |= SIM7600_STATUS_FLAG_PDP_ACTIVE;
	} else {
		mdata.status_flags &= ~SIM7600_STATUS_FLAG_PDP_ACTIVE;
	}

	LOG_INF("PDP context: %u", active);
	k_sem_give(&mdata.pdp_sem);
	return 0;
}

MODEM_CMD_DEFINE(on_urc_pdp_deact)
{
	LOG_INF("PDP context deactivated by network");

	mdata.status_flags &= ~SIM7600_STATUS_FLAG_PDP_ACTIVE;
	return 0;
}

/*
 * Handles socket data notification.
 *
 * The sim modem sends and unsolicited +CIPRXGET: 1,<cid>
 * if data can be read from a socket.
 */
MODEM_CMD_DEFINE(on_urc_ciprxget)
{
	int sock_fd;

	sock_fd = atoi(argv[1]);

	sim7600_handle_sock_data_indication(sock_fd);
	return 0;
}

/* TODO figure out what this is on the 7600
 * Handles the castate response.
 *
 * +CASTATE: <cid>,<state>
 *
 * Cid is the connection id (socket fd) and
 * state can be:
 *  0 - Closed by remote server or error
 *  1 - Connected to remote server
 *  2 - Listening
 */
// MODEM_CMD_DEFINE(on_urc_castate)
// {
// 	int sockfd, state;

// 	sockfd = atoi(argv[0]);
// 	state = atoi(argv[1]);

// 	sim7600_handle_sock_state(sockfd, state);
// 	return 0;
// }

/**
 * Handles the ftpget urc.
 *
 * +FTPGET: <mode>,<error>
 *
 * Mode can be 1 for opening a session and
 * reporting that data is available or 2 for
 * reading data. This urc handler will only handle
 * mode 1 because 2 will not occur as urc.
 *
 * Error can be either:
 *  - 1 for data available/opened session.
 *  - 0 If transfer is finished.
 *  - >0 for some error.
 */
MODEM_CMD_DEFINE(on_urc_ftpget)
{
	int error = atoi(argv[0]);

	LOG_INF("+FTPGET: 1,%d", error);

	/* Transfer finished. */
	if (error == 0) {
		mdata.ftp.state = SIM7600_FTP_CONNECTION_STATE_FINISHED;
	} else if (error == 1) {
		mdata.ftp.state = SIM7600_FTP_CONNECTION_STATE_CONNECTED;
	} else {
		mdata.ftp.state = SIM7600_FTP_CONNECTION_STATE_ERROR;
	}

	k_sem_give(&mdata.sem_ftp);

	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_rdy)
{
	LOG_DBG("RDY received");
	mdata.status_flags |= SIM7600_STATUS_FLAG_POWER_ON;
	k_sem_give(&mdata.boot_sem);
	return 0;
}

MODEM_CMD_DIRECT_DEFINE(on_urc_pwr_down)
{
	LOG_DBG("POWER DOWN received");
	mdata.status_flags &= ~SIM7600_STATUS_FLAG_POWER_ON;
	k_sem_give(&mdata.boot_sem);
	return 0;
}

MODEM_CMD_DEFINE(on_urc_cpin)
{
	if (strcmp(argv[0], "READY") == 0) {
		mdata.status_flags |= SIM7600_STATUS_FLAG_CPIN_READY;
	} else {
		mdata.status_flags &= ~SIM7600_STATUS_FLAG_CPIN_READY;
	}

	k_sem_give(&mdata.boot_sem);

	LOG_INF("CPIN: %s", argv[0]);
	return 0;
}

/*
 * Possible responses by the sim7600.
 */
static const struct modem_cmd response_cmds[] = {
	MODEM_CMD("OK", on_cmd_ok, 0U, ""),
	MODEM_CMD("ERROR", on_cmd_error, 0U, ""),
	MODEM_CMD("+CME ERROR: ", on_cmd_exterror, 1U, ""),
	MODEM_CMD_DIRECT(">", on_cmd_tx_ready),
};

/*
 * Possible unsolicited commands.
 */
static const struct modem_cmd unsolicited_cmds[] = {
	MODEM_CMD("RDY", on_urc_rdy, 0U, ""),
	MODEM_CMD("+CPIN: ", on_urc_cpin, 1U, ","),
	MODEM_CMD("+CIPRXGET: ", on_urc_ciprxget, 2U, ","),
};

/*
 * Toggles the modems power pin.
 */
static void sim7600_enable(void)
{
	LOG_DBG("Enabling sim7600");
	gpio_pin_set_dt(&power_gpio, GPIO_OUTPUT_ACTIVE);
}

static int modem_set_baudrate(uint32_t baudrate)
{
	char buf[sizeof("AT+IPR=##########")] = {0};

	int ret = snprintk(buf, sizeof(buf), "AT+IPR=%u", baudrate);

	if (ret < 0) {
		LOG_ERR("Failed to build command");
		goto out;
	}

	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, buf, &mdata.sem_response,
		K_SECONDS(2));
	if (ret != 0) {
		LOG_ERR("Failed to set baudrate");
	}

out:
	return ret;
}

/**
 * Performs the autobaud sequence until modem answers or limit is reached.
 *
 * @return On successful boot 0 is returned. Otherwise <0 is returned.
 */
int modem_autobaud(void)
{
	int counter = 0;
	int ret = -1;

	/*
	 * The sim7600 has a autobaud function.
	 * On startup multiple AT's are sent until
	 * a OK is received.
	 */
	counter = 0;
	while (counter++ <= MDM_MAX_AUTOBAUD) {
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT",
				     &mdata.sem_response, K_MSEC(500));
		if (ret != 0) {
			LOG_DBG("No response to autobaud AT");
			continue;
		}
		break;
	}

	return ret;
}

/**
 * Power on the modem and wait for operational sim card.
 *
 * @param allow_autobaud Allow autobaud functionality.
 * @return 0 on success. Otherwise <0.
 *
 * @note Autobaud is only allowed during driver setup.
 * In any other case a fixed baudrate should be used.
 */
static int modem_boot(bool allow_autobaud)
{
	uint8_t boot_tries = 0;
	int ret = -1;

	/* Reset the status flags */
	mdata.status_flags = 0;

	/* Try boot multiple times in case modem was already on */
	while (boot_tries++ <= MDM_BOOT_TRIES) {

		k_sem_reset(&mdata.boot_sem);

		mdm_sim7600_release_reset();

		ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
		if (ret == 0) {
			if (mdata.status_flags & SIM7600_STATUS_FLAG_POWER_ON) {
				LOG_INF("Modem booted");
				break;
			}

			LOG_INF("Modem turned off");
			k_sleep(K_SECONDS(1));
			continue;
		}

		LOG_WRN("No modem response after pwrkey");

		if (allow_autobaud == false) {
			continue;
		}

		LOG_INF("Trying autobaud");

		ret = modem_autobaud();
		if (ret != 0) {
			LOG_WRN("Autobaud failed");
			continue;
		}

		/* Set baudrate to disable autobaud on next startup */
		ret = modem_set_baudrate(CONFIG_MODEM_SIMCOM_SIM7600_BAUDRATE);
		if (ret != 0) {
			LOG_ERR("Failed to disable echo");
			continue;
		}

		/* Reset modem and wait for ready indication */
		ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "AT+CFUN=1,1",
			     &mdata.sem_response, K_MSEC(500));
		if (ret != 0) {
			LOG_ERR("Reset failed");
			break;
		}

		ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
		if (ret != 0) {
			LOG_ERR("No RDY received!");
			break;
		}

		if ((mdata.status_flags & SIM7600_STATUS_FLAG_POWER_ON) == 0) {
			LOG_ERR("Modem not powered");
			break;
		}

		break;
	}

	if (ret != 0) {
		LOG_ERR("Modem boot failed!");
		goto out;
	}

	/* Wait for sim card status */
	ret = k_sem_take(&mdata.boot_sem, K_SECONDS(5));
	if (ret != 0) {
		LOG_ERR("Timeout while waiting for sim status");
		goto out;
	}

	if ((mdata.status_flags & SIM7600_STATUS_FLAG_CPIN_READY) == 0) {
		LOG_ERR("Sim card not ready!");
		goto out;
	}

	/* Disable echo on successful boot */
	ret = modem_cmd_send(&mctx.iface, &mctx.cmd_handler, NULL, 0U, "ATE0",
				 &mdata.sem_response, K_MSEC(500));
	if (ret != 0) {
		LOG_ERR("Disabling echo failed");
		goto out;
	}

	sim7600_change_state(SIM7600_STATE_IDLE);

out:
	return ret;
}

/*
 * Commands to be sent at setup.
 */
static const struct setup_cmd setup_cmds[] = {
	SETUP_CMD("AT+CGMI", "", on_cmd_cgmi, 0U, ""),
	SETUP_CMD("AT+CGMM", "", on_cmd_cgmm, 0U, ""),
	SETUP_CMD("AT+CGMR", "", on_cmd_cgmr, 0U, ""),
	SETUP_CMD("AT+CGSN", "", on_cmd_cgsn, 0U, ""),
	SETUP_CMD("AT+CWMACADDR?", "", on_cmd_cwmacaddr, 0U, ""),
#if defined(CONFIG_MODEM_SIM_NUMBERS)
	SETUP_CMD("AT+CIMI", "", on_cmd_cimi, 0U, ""),
	SETUP_CMD("AT+CICCID", "", on_cmd_ccid, 0U, ""),
#endif /* defined(CONFIG_MODEM_SIM_NUMBERS) */
};

/*
 * Does the modem setup by starting it and
 * bringing the modem to a PDP active state.
 */
static int modem_setup(void)
{
	int ret = 0;

	k_work_cancel_delayable(&mdata.rssi_query_work);

	ret = modem_boot(true);
	if (ret < 0) {
		LOG_ERR("Booting modem failed!!");
		return ret;
	}

	ret = modem_cmd_handler_setup_cmds(&mctx.iface, &mctx.cmd_handler, setup_cmds,
					   ARRAY_SIZE(setup_cmds), &mdata.sem_response,
					   MDM_REGISTRATION_TIMEOUT);
	if (ret < 0) {
		LOG_ERR("Failed to send init commands!");
		return ret;
	}

	if (strcmp(mdata.mdm_model, "SIMCOM_SIM7600") != 0) {
		LOG_ERR("Wrong modem model: %s", mdata.mdm_model);
		ret = -EINVAL;
		return ret;
	}

#if IS_ENABLED(CONFIG_MODEM_SIMCOM_SIM7600_BOOT_TYPE_CONSTRAINED)
	ret = mdm_sim7600_power_off();
#elif IS_ENABLED(CONFIG_MODEM_SIMCOM_SIM7600_BOOT_TYPE_NORMAL)
	ret = sim7600_pdp_activate();
#else
#error No boot type selected
#endif

	return ret;
}

const char *mdm_sim7600_get_manufacturer(void)
{
	return mdata.mdm_manufacturer;
}

const char *mdm_sim7600_get_model(void)
{
	return mdata.mdm_model;
}

const char *mdm_sim7600_get_revision(void)
{
	return mdata.mdm_revision;
}

const char *mdm_sim7600_get_imei(void)
{
	return mdata.mdm_imei;
}

#if defined(CONFIG_MODEM_SIM_NUMBERS)
const char *mdm_sim7600_get_iccid(void)
{
	return mdata.mdm_iccid;
}
#else
const char *mdm_sim7600_get_iccid(void)
{
	return NULL;
}
#endif /* #if defined(CONFIG_MODEM_SIM_NUMBERS) */

/*
 * Initializes modem handlers and context.
 * After successful init this function calls
 * modem_setup.
 */
static int modem_init(const struct device *dev)
{
	int ret;

	ARG_UNUSED(dev);

	k_sem_init(&mdata.sem_response, 0, 1);
	k_sem_init(&mdata.sem_tx_ready, 0, 1);
	k_sem_init(&mdata.sem_dns, 0, 1);
	k_sem_init(&mdata.sem_ftp, 0, 1);
	k_sem_init(&mdata.sem_http, 0, 1);
	k_sem_init(&mdata.boot_sem, 0, 1);
	k_sem_init(&mdata.pdp_sem, 0, 1);
	k_work_queue_start(&modem_workq, modem_workq_stack,
			   K_KERNEL_STACK_SIZEOF(modem_workq_stack), K_PRIO_COOP(7), NULL);

	/* Assume the modem is not registered to the network. */
	mdata.mdm_registration = 0;
	mdata.status_flags = 0;

	/* Socket config. */
	ret = modem_socket_init(&mdata.socket_config, &mdata.sockets[0], ARRAY_SIZE(mdata.sockets),
				MDM_BASE_SOCKET_NUM, true, &offload_socket_fd_op_vtable);
	if (ret < 0) {
		goto error;
	}

	sim7600_change_state(SIM7600_STATE_OFF);

	/* Command handler. */
	const struct modem_cmd_handler_config cmd_handler_config = {
		.match_buf = &mdata.cmd_match_buf[0],
		.match_buf_len = sizeof(mdata.cmd_match_buf),
		.buf_pool = &mdm_recv_pool,
		.alloc_timeout = BUF_ALLOC_TIMEOUT,
		.eol = "\r\n",
		.user_data = NULL,
		.response_cmds = response_cmds,
		.response_cmds_len = ARRAY_SIZE(response_cmds),
		.unsol_cmds = unsolicited_cmds,
		.unsol_cmds_len = ARRAY_SIZE(unsolicited_cmds),
	};

	ret = modem_cmd_handler_init(&mctx.cmd_handler, &mdata.cmd_handler_data,
				     &cmd_handler_config);
	if (ret < 0) {
		goto error;
	}

	/* Uart handler. */
	const struct modem_iface_uart_config uart_config = {
		.rx_rb_buf = &mdata.iface_rb_buf[0],
		.rx_rb_buf_len = sizeof(mdata.iface_rb_buf),
		.dev = MDM_UART_DEV,
		.hw_flow_control = DT_PROP(MDM_UART_NODE, hw_flow_control),
	};

	ret = modem_iface_uart_init(&mctx.iface, &mdata.iface_data, &uart_config);
	if (ret < 0) {
		goto error;
	}

	mdata.current_sock_fd = -1;
	mdata.current_sock_written = 0;

	mdata.dns.recount = CONFIG_MODEM_SIMCOM_SIM7600_DNS_DEFAULT_RECOUNT;
	mdata.dns.timeout = CONFIG_MODEM_SIMCOM_SIM7600_DNS_DEFAULT_TIMEOUT;

	mdata.ftp.read_buffer = NULL;
	mdata.ftp.nread = 0;
	mdata.ftp.state = SIM7600_FTP_CONNECTION_STATE_INITIAL;

	/* Modem data storage. */
	mctx.data_manufacturer = mdata.mdm_manufacturer;
	mctx.data_model = mdata.mdm_model;
	mctx.data_revision = mdata.mdm_revision;
	mctx.data_imei = mdata.mdm_imei;
#if defined(CONFIG_MODEM_SIM_NUMBERS)
	mctx.data_imsi = mdata.mdm_imsi;
	mctx.data_iccid = mdata.mdm_iccid;
#endif /* #if defined(CONFIG_MODEM_SIM_NUMBERS) */
	mctx.data_rssi = &mdata.mdm_rssi;

	mdm_sim7600_set_reset();
	mdm_sim7600_power_on();
	mdm_sim7600_release_disable();

	mctx.driver_data = &mdata;

	ret = modem_context_register(&mctx);
	if (ret < 0) {
		LOG_ERR("Error registering modem context: %d", ret);
		goto error;
	}

	k_thread_create(&modem_rx_thread, modem_rx_stack, K_KERNEL_STACK_SIZEOF(modem_rx_stack),
			modem_rx, NULL, NULL, NULL, K_PRIO_COOP(7), 0, K_NO_WAIT);

	/* Init RSSI query */
	k_work_init_delayable(&mdata.rssi_query_work, sim7600_rssi_query_work);

	return modem_setup();
error:
	return ret;
}

/* Register device with the networking stack. */
NET_DEVICE_DT_INST_OFFLOAD_DEFINE(0, modem_init, NULL, &mdata, NULL,
				  CONFIG_MODEM_SIMCOM_SIM7600_INIT_PRIORITY, &api_funcs,
				  MDM_MAX_DATA_LENGTH);

NET_SOCKET_OFFLOAD_REGISTER(simcom_sim7600, CONFIG_NET_SOCKETS_OFFLOAD_PRIORITY,
			    AF_UNSPEC, offload_is_supported, sim7600_offload_socket);
