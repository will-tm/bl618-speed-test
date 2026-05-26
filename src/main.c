/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#ifdef CONFIG_NET_DHCPV4_SERVER
#include <zephyr/net/dhcpv4_server.h>
#endif
#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
#include "supp_events.h"
#endif

LOG_MODULE_REGISTER(speed_test, LOG_LEVEL_INF);

#ifdef CONFIG_SPEED_TEST_STA_MODE
#define STA_SSID CONFIG_SPEED_TEST_STA_SSID
#define STA_PSK  CONFIG_SPEED_TEST_STA_PSK
#else
#define AP_SSID    "WiFi_SpeedTest"
#define AP_PSK     "SpeedTest42"
#define AP_IP      "10.0.8.1"
#define AP_NETMASK "255.255.255.0"
#define DHCP_BASE  "10.0.8.10"
#endif

#define NUM_STREAMS     4
#define DOWNLOAD_TOTAL  (4U * 1024U * 1024U)
#define DOWNLOAD_PER_STREAM (DOWNLOAD_TOTAL / NUM_STREAMS)
#define CHUNK_SIZE      (8 * 1204)

static struct net_mgmt_event_callback wifi_cb;

#ifdef CONFIG_SPEED_TEST_STA_MODE
static K_SEM_DEFINE(sta_connected, 0, 1);
static K_SEM_DEFINE(got_ip, 0, 1);
static struct net_mgmt_event_callback ipv4_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
		const struct wifi_status *status = (const struct wifi_status *)cb->info;

		if (status->status) {
			LOG_ERR("Connect failed: %d", status->status);
		} else {
			LOG_INF("Connected to AP");
			k_sem_give(&sta_connected);
		}
	} else if (mgmt_event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
		LOG_INF("Disconnected from AP");
	}
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		k_sem_give(&got_ip);
	}
}
#else
static K_SEM_DEFINE(ap_ready, 0, 1);

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_WIFI_AP_ENABLE_RESULT) {
		LOG_INF("AP enabled");
		k_sem_give(&ap_ready);
	} else if (mgmt_event == NET_EVENT_WIFI_AP_STA_CONNECTED) {
		const struct wifi_ap_sta_info *sta = (const struct wifi_ap_sta_info *)cb->info;

		LOG_INF("Station connected: %02x:%02x:%02x:%02x:%02x:%02x", sta->mac[0],
			sta->mac[1], sta->mac[2], sta->mac[3], sta->mac[4], sta->mac[5]);
	} else if (mgmt_event == NET_EVENT_WIFI_AP_STA_DISCONNECTED) {
		const struct wifi_ap_sta_info *sta = (const struct wifi_ap_sta_info *)cb->info;

		LOG_INF("Station disconnected: %02x:%02x:%02x:%02x:%02x:%02x", sta->mac[0],
			sta->mac[1], sta->mac[2], sta->mac[3], sta->mac[4], sta->mac[5]);
	}
}
#endif

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
static K_SEM_DEFINE(supp_ready, 0, 1);
static struct net_mgmt_event_callback supp_cb;

static void supp_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	k_sem_give(&supp_ready);
}
#endif

#ifdef CONFIG_SPEED_TEST_STA_MODE
static int connect_sta(struct net_if *iface)
{
	struct wifi_connect_req_params params = {0};

	params.ssid = (uint8_t *)STA_SSID;
	params.ssid_length = strlen(STA_SSID);
	params.psk = (uint8_t *)STA_PSK;
	params.psk_length = strlen(STA_PSK);
	params.security = WIFI_SECURITY_TYPE_PSK;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_UNKNOWN;

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (ret) {
		LOG_ERR("WiFi connect failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&sta_connected, K_SECONDS(30))) {
		LOG_ERR("WiFi connect timeout");
		return -ETIMEDOUT;
	}

	return 0;
}
#else
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

	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
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

static int setup_ap_network(struct net_if *iface)
{
	struct in_addr addr, netmask;

	net_addr_pton(AF_INET, AP_IP, &addr);
	net_addr_pton(AF_INET, AP_NETMASK, &netmask);

	if (!net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0)) {
		LOG_ERR("Failed to add IP address");
		return -ENODEV;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
	net_if_ipv4_set_gw(iface, &addr);

#ifdef CONFIG_NET_DHCPV4_SERVER
	struct in_addr dhcp_base;

	net_addr_pton(AF_INET, DHCP_BASE, &dhcp_base);

	int ret = net_dhcpv4_server_start(iface, &dhcp_base);

	if (ret) {
		LOG_ERR("DHCP server start failed: %d", ret);
		return ret;
	}
#endif

	LOG_INF("Network: %s/%s DHCP from %s", AP_IP, AP_NETMASK, DHCP_BASE);
	return 0;
}
#endif

static uint8_t speed_buf[CHUNK_SIZE];

struct stream_state {
	struct http_client_ctx *client;
	size_t bytes;
	char resp[32];
};

static struct stream_state dl_streams[NUM_STREAMS];
static struct stream_state ul_streams[NUM_STREAMS];

static struct stream_state *stream_get(struct stream_state *arr, struct http_client_ctx *client)
{
	for (int i = 0; i < NUM_STREAMS; i++) {
		if (arr[i].client == client) {
			return &arr[i];
		}
	}
	for (int i = 0; i < NUM_STREAMS; i++) {
		if (arr[i].client == NULL) {
			arr[i].client = client;
			arr[i].bytes = 0;
			return &arr[i];
		}
	}
	return NULL;
}

static int download_handler(struct http_client_ctx *client, enum http_transaction_status status,
			    const struct http_request_ctx *request_ctx,
			    struct http_response_ctx *response_ctx, void *user_data)
{
	struct stream_state *s = stream_get(dl_streams, client);

	if (!s) {
		return -ENOMEM;
	}

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		s->client = NULL;
		return 0;
	}

	if (s->bytes == 0) {
		int opt = 1;

		zsock_setsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
	}

	size_t chunk = MIN(CHUNK_SIZE, DOWNLOAD_PER_STREAM - s->bytes);

	response_ctx->body = speed_buf;
	response_ctx->body_len = chunk;
	s->bytes += chunk;
	response_ctx->final_chunk = (s->bytes >= DOWNLOAD_PER_STREAM);

	return 0;
}

static int ping_handler(struct http_client_ctx *client, enum http_transaction_status status,
			const struct http_request_ctx *request_ctx,
			struct http_response_ctx *response_ctx, void *user_data)
{
	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		return 0;
	}

	static const uint8_t pong[] = "pong";

	response_ctx->body = pong;
	response_ctx->body_len = sizeof(pong) - 1;
	response_ctx->final_chunk = true;

	return 0;
}

static int upload_handler(struct http_client_ctx *client, enum http_transaction_status status,
			  const struct http_request_ctx *request_ctx,
			  struct http_response_ctx *response_ctx, void *user_data)
{
	struct stream_state *s = stream_get(ul_streams, client);

	if (!s) {
		return -ENOMEM;
	}

	if (status == HTTP_SERVER_TRANSACTION_ABORTED ||
	    status == HTTP_SERVER_TRANSACTION_COMPLETE) {
		s->client = NULL;
		return 0;
	}

	s->bytes += request_ctx->data_len;

	if (status == HTTP_SERVER_REQUEST_DATA_FINAL) {
		int len = snprintf(s->resp, sizeof(s->resp), "%zu", s->bytes);

		response_ctx->body = (uint8_t *)s->resp;
		response_ctx->body_len = len;
		response_ctx->final_chunk = true;
	}

	return 0;
}

static struct http_resource_detail_dynamic ping_resource_detail = {
	.common = {
		.type = HTTP_RESOURCE_TYPE_DYNAMIC,
		.bitmask_of_supported_http_methods = BIT(HTTP_GET),
	},
	.cb = ping_handler,
};

static struct http_resource_detail_dynamic dl_resource_detail[NUM_STREAMS] = {
	[0 ... NUM_STREAMS - 1] = {
		.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		},
		.cb = download_handler,
	},
};

static struct http_resource_detail_dynamic ul_resource_detail[NUM_STREAMS] = {
	[0 ... NUM_STREAMS - 1] = {
		.common = {
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_POST),
		},
		.cb = upload_handler,
	},
};

static uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static uint8_t favicon_png[] = {
#include "favicon.png.inc"
};

static struct http_resource_detail_static index_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_encoding = "gzip",
			.content_type = "text/html",
		},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

static struct http_resource_detail_static favicon_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_type = "image/png",
		},
	.static_data = favicon_png,
	.static_data_len = sizeof(favicon_png),
};

static uint16_t http_port = CONFIG_SPEED_TEST_HTTP_PORT;
HTTP_SERVICE_DEFINE(speed_service, NULL, &http_port, 6, 10, NULL, NULL, NULL);
HTTP_RESOURCE_DEFINE(index_resource, speed_service, "/", &index_resource_detail);
HTTP_RESOURCE_DEFINE(favicon_resource, speed_service, "/favicon.png", &favicon_resource_detail);

HTTP_RESOURCE_DEFINE(ping_res, speed_service, "/api/ping", &ping_resource_detail);
HTTP_RESOURCE_DEFINE(dl_res0, speed_service, "/api/download/0", &dl_resource_detail[0]);
HTTP_RESOURCE_DEFINE(dl_res1, speed_service, "/api/download/1", &dl_resource_detail[1]);
HTTP_RESOURCE_DEFINE(dl_res2, speed_service, "/api/download/2", &dl_resource_detail[2]);
HTTP_RESOURCE_DEFINE(dl_res3, speed_service, "/api/download/3", &dl_resource_detail[3]);
HTTP_RESOURCE_DEFINE(ul_res0, speed_service, "/api/upload/0", &ul_resource_detail[0]);
HTTP_RESOURCE_DEFINE(ul_res1, speed_service, "/api/upload/1", &ul_resource_detail[1]);
HTTP_RESOURCE_DEFINE(ul_res2, speed_service, "/api/upload/2", &ul_resource_detail[2]);
HTTP_RESOURCE_DEFINE(ul_res3, speed_service, "/api/upload/3", &ul_resource_detail[3]);

int main(void)
{
	struct net_if *iface = net_if_get_default();

	memset(speed_buf, 'D', sizeof(speed_buf));

	LOG_INF("=== WiFi Speed Test ===");

#ifdef CONFIG_WIFI_NM_WPA_SUPPLICANT
	net_mgmt_init_event_callback(&supp_cb, supp_event_handler, NET_EVENT_SUPPLICANT_READY);
	net_mgmt_add_event_callback(&supp_cb);

	LOG_INF("Waiting for supplicant...");
	if (k_sem_take(&supp_ready, K_SECONDS(10))) {
		LOG_ERR("Supplicant init timeout");
		return -1;
	}
	LOG_INF("Supplicant ready");
#endif

#ifdef CONFIG_SPEED_TEST_STA_MODE
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_cb);

	LOG_INF("Connecting to '%s'...", STA_SSID);
	if (connect_sta(iface)) {
		LOG_ERR("Failed to connect");
		return -1;
	}

	LOG_INF("Waiting for IP address...");
	if (k_sem_take(&got_ip, K_SECONDS(30))) {
		LOG_ERR("DHCP timeout");
		return -1;
	}

	char ip_str[NET_IPV4_ADDR_LEN];
	struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

	if (ipv4) {
		net_addr_ntop(AF_INET, &ipv4->unicast[0].ipv4.address.in_addr, ip_str,
			      sizeof(ip_str));
	}
#else
	net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
				     NET_EVENT_WIFI_AP_ENABLE_RESULT |
					     NET_EVENT_WIFI_AP_STA_CONNECTED |
					     NET_EVENT_WIFI_AP_STA_DISCONNECTED);
	net_mgmt_add_event_callback(&wifi_cb);

	if (start_ap(iface)) {
		LOG_ERR("Failed to start AP");
		return -1;
	}

	if (setup_ap_network(iface)) {
		LOG_ERR("Failed to setup network");
		return -1;
	}
#endif

	struct wifi_ps_params ps_params = {.enabled = WIFI_PS_DISABLED};

	net_mgmt(NET_REQUEST_WIFI_PS, iface, &ps_params, sizeof(ps_params));
	LOG_INF("WiFi power save disabled");

	http_server_start();

#ifdef CONFIG_SPEED_TEST_STA_MODE
	LOG_INF("HTTP server on http://%s/", ip_str);
#else
	LOG_INF("HTTP server on http://%s/", AP_IP);
	LOG_INF("WiFi: '%s' (WPA2)", AP_SSID);
#endif

	return 0;
}
