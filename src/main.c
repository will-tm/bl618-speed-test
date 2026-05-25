/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/logging/log.h>
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
#include "supp_events.h"
#endif

LOG_MODULE_REGISTER(speed_test, LOG_LEVEL_INF);

#define AP_SSID    "WiFi_SpeedTest"
#define AP_PSK     "SpeedTest42"

#define AP_IP      "10.15.84.1"
#define AP_NETMASK "255.255.255.0"
#define DHCP_BASE  "10.15.84.10"

#define DOWNLOAD_TOTAL  (2U * 1024U * 1024U)
#define CHUNK_SIZE      4096

static K_SEM_DEFINE(ap_ready, 0, 1);

static struct net_mgmt_event_callback wifi_cb;

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
static K_SEM_DEFINE(supp_ready, 0, 1);
static struct net_mgmt_event_callback supp_cb;

static void supp_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	k_sem_give(&supp_ready);
}
#endif

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_WIFI_AP_ENABLE_RESULT) {
		LOG_INF("AP enabled");
		k_sem_give(&ap_ready);
	} else if (mgmt_event == NET_EVENT_WIFI_AP_STA_CONNECTED) {
		const struct wifi_ap_sta_info *sta =
			(const struct wifi_ap_sta_info *)cb->info;

		LOG_INF("Station connected: %02x:%02x:%02x:%02x:%02x:%02x",
			sta->mac[0], sta->mac[1], sta->mac[2],
			sta->mac[3], sta->mac[4], sta->mac[5]);
	} else if (mgmt_event == NET_EVENT_WIFI_AP_STA_DISCONNECTED) {
		const struct wifi_ap_sta_info *sta =
			(const struct wifi_ap_sta_info *)cb->info;

		LOG_INF("Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x",
			sta->mac[0], sta->mac[1], sta->mac[2],
			sta->mac[3], sta->mac[4], sta->mac[5]);
	}
}

static int start_ap(struct net_if *iface)
{
	struct wifi_connect_req_params params = {0};

	params.ssid = (uint8_t *)AP_SSID;
	params.ssid_length = strlen(AP_SSID);
	params.psk = (uint8_t *)AP_PSK;
	params.psk_length = strlen(AP_PSK);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;

	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params,
			   sizeof(params));
	if (ret) {
		LOG_ERR("AP enable failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&ap_ready, K_SECONDS(10))) {
		LOG_ERR("AP enable timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

static int setup_network(struct net_if *iface)
{
	struct in_addr addr, netmask, dhcp_base;

	net_addr_pton(AF_INET, AP_IP, &addr);
	net_addr_pton(AF_INET, AP_NETMASK, &netmask);
	net_addr_pton(AF_INET, DHCP_BASE, &dhcp_base);

	if (!net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("Failed to add IP address");
		return -ENODEV;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

	int ret = net_dhcpv4_server_start(iface, &dhcp_base);
	if (ret) {
		LOG_ERR("DHCP server start failed: %d", ret);
		return ret;
	}

	LOG_INF("Network: %s/%s DHCP from %s", AP_IP, AP_NETMASK, DHCP_BASE);
	return 0;
}

static uint8_t speed_buf[CHUNK_SIZE];

static size_t dl_sent;

static int download_handler(struct http_client_ctx *client,
			     enum http_transaction_status status,
			     const struct http_request_ctx *request_ctx,
			     struct http_response_ctx *response_ctx,
			     void *user_data)
{
	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		dl_sent = 0;
		return 0;
	}

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		LOG_INF("Download: %zu bytes sent", dl_sent);
		dl_sent = 0;
		return 0;
	}

	size_t chunk = MIN(CHUNK_SIZE, DOWNLOAD_TOTAL - dl_sent);

	response_ctx->body = speed_buf;
	response_ctx->body_len = chunk;
	dl_sent += chunk;
	response_ctx->final_chunk = (dl_sent >= DOWNLOAD_TOTAL);

	return 0;
}

static size_t ul_received;

static int upload_handler(struct http_client_ctx *client,
			  enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx,
			  void *user_data)
{
	static char resp_buf[32];

	if (status == HTTP_SERVER_TRANSACTION_ABORTED) {
		ul_received = 0;
		return 0;
	}

	if (status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		ul_received = 0;
		return 0;
	}

	ul_received += request_ctx->data_len;

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		int len = snprintf(resp_buf, sizeof(resp_buf), "%zu", ul_received);

		response_ctx->body = (uint8_t *)resp_buf;
		response_ctx->body_len = len;
		response_ctx->final_chunk = true;
		LOG_INF("Upload: %zu bytes received", ul_received);
	}

	return 0;
}

static struct http_resource_detail_dynamic download_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = download_handler,
};

static struct http_resource_detail_dynamic upload_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_POST),
	},
	.cb = upload_handler,
};

static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_STATIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		.content_encoding = "gzip",
		.content_type = "text/html",
	},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

static uint16_t http_port = CONFIG_SPEED_TEST_HTTP_PORT;
HTTP_SERVICE_DEFINE(speed_service, NULL, &http_port, 1, 10, NULL, NULL, NULL);
HTTP_RESOURCE_DEFINE(index_resource, speed_service, "/",
		     &index_resource_detail);
HTTP_RESOURCE_DEFINE(download_resource, speed_service, "/api/download",
		     &download_resource_detail);
HTTP_RESOURCE_DEFINE(upload_resource, speed_service, "/api/upload",
		     &upload_resource_detail);

int main(void)
{
	struct net_if *iface = net_if_get_default();

	memset(speed_buf, 'D', sizeof(speed_buf));

	LOG_INF("=== WiFi Speed Test ===");

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
	net_mgmt_init_event_callback(&supp_cb, supp_event_handler,
		NET_EVENT_SUPPLICANT_READY);
	net_mgmt_add_event_callback(&supp_cb);

	LOG_INF("Waiting for supplicant...");
	if (k_sem_take(&supp_ready, K_SECONDS(10))) {
		LOG_ERR("Supplicant init timeout");
		return -1;
	}
	LOG_INF("Supplicant ready");
#endif

	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
				     NET_EVENT_WIFI_AP_STA_CONNECTED |
				     NET_EVENT_WIFI_AP_STA_DISCONNECTED);
	net_mgmt_add_event_callback(&wifi_cb);

	if (start_ap(iface)) {
		LOG_ERR("Failed to start AP");
		return -1;
	}

	if (setup_network(iface)) {
		LOG_ERR("Failed to setup network");
		return -1;
	}

	http_server_start();
	LOG_INF("HTTP server on http://%s/", AP_IP);

	LOG_INF("WiFi: '%s' password '%s'", AP_SSID, AP_PSK);

	return 0;
}
