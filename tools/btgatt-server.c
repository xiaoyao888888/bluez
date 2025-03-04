// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Google Inc.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "lib/uuid.h"
#include "lib/sdp.h"
#include "lib/sdp_lib.h"

#include "src/shared/mainloop.h"
#include "src/shared/util.h"
#include "src/shared/att.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/gatt-db.h"
#include "src/shared/gatt-server.h"

//#include "src/uuid-helper.h"
#include "lib/mgmt.h"

#include "src/shared/io.h"
#include "src/shared/mgmt.h"
//#include "src/shared/shell.h"

#define UUID_GAP			0x1800
#define UUID_GATT			0x1801
#define UUID_HEART_RATE			0x180d
#define UUID_HEART_RATE_MSRMT		0x2a37
#define UUID_HEART_RATE_BODY		0x2a38
#define UUID_HEART_RATE_CTRL		0x2a39

#define UUID_WIFI			0x2222
#define UUID_WIFI_CHR		0x3333

#define ATT_CID 4

#define PRLOG(...) \
	do { \
		printf(__VA_ARGS__); \
		print_prompt(); \
	} while (0)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define COLOR_OFF	"\x1B[0m"
#define COLOR_RED	"\x1B[0;91m"
#define COLOR_GREEN	"\x1B[0;92m"
#define COLOR_YELLOW	"\x1B[0;93m"
#define COLOR_BLUE	"\x1B[0;94m"
#define COLOR_MAGENTA	"\x1B[0;95m"
#define COLOR_BOLDGRAY	"\x1B[1;30m"
#define COLOR_BOLDWHITE	"\x1B[1;37m"

static const char test_device_name[] = "Very Long Test Device Name For Testing "
				"ATT Protocol Operations On GATT Server";
static bool verbose = false;

static struct mgmt *mgmt = NULL;
static uint16_t mgmt_index = MGMT_INDEX_NONE;

struct server {
	int fd;
	struct bt_att *att;
	struct gatt_db *db;
	struct bt_gatt_server *gatt;

	uint8_t *device_name;
	size_t name_len;

	uint16_t gatt_svc_chngd_handle;
	bool svc_chngd_enabled;

	uint16_t hr_handle;
	uint16_t hr_msrmt_handle;
	uint16_t hr_energy_expended;

	//wifi
	uint16_t wifi_handle;
	uint16_t wifi_chr_handle;
	bool wifi_chngd_enabled;

	bool hr_visible;
	bool hr_msrmt_enabled;
	int hr_ee_count;
	unsigned int hr_timeout_id;
};

#define print(fmt, arg...) do { \
		printf(fmt "\n", ## arg); \
} while (0)

#define error(fmt, arg...) do { \
		printf(COLOR_RED fmt "\n" COLOR_OFF, ## arg); \
} while (0)

static void print_prompt(void)
{
	printf(COLOR_BLUE "[GATT server]" COLOR_OFF "# ");
	fflush(stdout);
}

static void att_disconnect_cb(int err, void *user_data)
{
	printf("Device disconnected: %s\n", strerror(err));

	mainloop_quit();
}

static void att_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_BOLDGRAY "%s" COLOR_BOLDWHITE "%s\n" COLOR_OFF, prefix,
									str);
}

static void gatt_debug_cb(const char *str, void *user_data)
{
	const char *prefix = user_data;

	PRLOG(COLOR_GREEN "%s%s\n" COLOR_OFF, prefix, str);
}

static void wifi_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;

	printf("wifi_read_cb called: offset: %d\n", offset);

	len = 5;
	value = "hello";

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void wifi_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	uint8_t val[10];


	printf("wifi_write_cb called: offset: %d\n", offset);

	if (value) {
		memset(val, 0, 10);
		memcpy(val + offset, value, len);
		printf("value: %s\n", val);
	}

done:
	gatt_db_attribute_write_result(attrib, id, error);

	printf("server: %p\n", server);
	bt_gatt_server_send_notification(server->gatt,
						server->wifi_chr_handle,
						"notify", 6, false);
}

static void gap_device_name_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;
	size_t len = 0;
	const uint8_t *value = NULL;

	PRLOG("GAP Device Name Read called\n");

	len = server->name_len;

	if (offset > len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	len -= offset;
	value = len ? &server->device_name[offset] : NULL;

done:
	gatt_db_attribute_read_result(attrib, id, error, value, len);
}

static void gap_device_name_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t error = 0;

	PRLOG("GAP Device Name Write called\n");

	/* If the value is being completely truncated, clean up and return */
	if (!(offset + len)) {
		free(server->device_name);
		server->device_name = NULL;
		server->name_len = 0;
		goto done;
	}

	/* Implement this as a variable length attribute value. */
	if (offset > server->name_len) {
		error = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (offset + len != server->name_len) {
		uint8_t *name;

		name = realloc(server->device_name, offset + len);
		if (!name) {
			error = BT_ATT_ERROR_INSUFFICIENT_RESOURCES;
			goto done;
		}

		server->device_name = name;
		server->name_len = offset + len;
	}

	if (value)
		memcpy(server->device_name + offset, value, len);

done:
	gatt_db_attribute_write_result(attrib, id, error);
}

static void gap_device_name_ext_prop_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	uint8_t value[2];

	PRLOG("Device Name Extended Properties Read called\n");

	value[0] = BT_GATT_CHRC_EXT_PROP_RELIABLE_WRITE;
	value[1] = 0;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void gatt_service_changed_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	PRLOG("Service Changed Read called\n");

	gatt_db_attribute_read_result(attrib, id, 0, NULL, 0);
}

static void gatt_svc_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	PRLOG("Service Changed CCC Read called\n");

	value[0] = server->svc_chngd_enabled ? 0x02 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void gatt_svc_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	PRLOG("Service Changed CCC Write called\n");

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->svc_chngd_enabled = false;
	else if (value[0] == 0x02)
		server->svc_chngd_enabled = true;
	else
		ecode = 0x80;

	PRLOG("Service Changed Enabled: %s\n",
				server->svc_chngd_enabled ? "true" : "false");

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void hr_msrmt_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	value[0] = server->hr_msrmt_enabled ? 0x01 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, 2);
}

static bool hr_msrmt_cb(void *user_data)
{
	struct server *server = user_data;
	bool expended_present = !(server->hr_ee_count % 10);
	uint16_t len = 2;
	uint8_t pdu[4];
	uint32_t cur_ee;
	uint32_t val;

	if (util_getrandom(&val, sizeof(val), 0) < 0)
		return false;

	pdu[0] = 0x06;
	pdu[1] = 90 + (val % 40);

	if (expended_present) {
		pdu[0] |= 0x08;
		put_le16(server->hr_energy_expended, pdu + 2);
		len += 2;
	}

	bt_gatt_server_send_notification(server->gatt,
						server->hr_msrmt_handle,
						pdu, len, false);


	cur_ee = server->hr_energy_expended;
	server->hr_energy_expended = MIN(UINT16_MAX, cur_ee + 10);
	server->hr_ee_count++;

	return true;
}

static void update_hr_msrmt_simulation(struct server *server)
{
	if (!server->hr_msrmt_enabled || !server->hr_visible) {
		timeout_remove(server->hr_timeout_id);
		return;
	}

	server->hr_timeout_id = timeout_add(1000, hr_msrmt_cb, server, NULL);
}

static void hr_msrmt_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->hr_msrmt_enabled = false;
	else if (value[0] == 0x01) {
		if (server->hr_msrmt_enabled) {
			PRLOG("HR Measurement Already Enabled\n");
			goto done;
		}

		server->hr_msrmt_enabled = true;
	} else
		ecode = 0x80;

	PRLOG("HR: Measurement Enabled: %s\n",
				server->hr_msrmt_enabled ? "true" : "false");

	update_hr_msrmt_simulation(server);

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void hr_control_point_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	if (!value || len != 1) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 1) {
		PRLOG("HR: Energy Expended value reset\n");
		server->hr_energy_expended = 0;
	}

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void confirm_write(struct gatt_db_attribute *attr, int err,
							void *user_data)
{
	if (!err)
		return;

	fprintf(stderr, "Error caching attribute %p - err: %d\n", attr, err);
	exit(1);
}

static void wifi_chngd_ccc_read_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t value[2];

	PRLOG("Wifi: Service Changed CCC Read called\n");

	value[0] = server->wifi_chngd_enabled ? 0x02 : 0x00;
	value[1] = 0x00;

	gatt_db_attribute_read_result(attrib, id, 0, value, sizeof(value));
}

static void wifi_chngd_ccc_write_cb(struct gatt_db_attribute *attrib,
					unsigned int id, uint16_t offset,
					const uint8_t *value, size_t len,
					uint8_t opcode, struct bt_att *att,
					void *user_data)
{
	struct server *server = user_data;
	uint8_t ecode = 0;

	PRLOG("Wifi: Service Changed CCC Write called [%d:%d]\n", value[0], value[1]);

	if (!value || len != 2) {
		ecode = BT_ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LEN;
		goto done;
	}

	if (offset) {
		ecode = BT_ATT_ERROR_INVALID_OFFSET;
		goto done;
	}

	if (value[0] == 0x00)
		server->wifi_chngd_enabled = false;
	else if (value[0] == 0x01)
		server->wifi_chngd_enabled = true;
	else
		ecode = 0x80;

	PRLOG("Wifi: Service Changed Enabled: %s\n",
				server->wifi_chngd_enabled ? "true" : "false");

done:
	gatt_db_attribute_write_result(attrib, id, ecode);
}

static void populate_gap_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *tmp;
	uint16_t appearance;

	/* Add the GAP service */
	bt_uuid16_create(&uuid, UUID_GAP);
	service = gatt_db_add_service(server->db, &uuid, true, 6);

	/*
	 * Device Name characteristic. Make the value dynamically read and
	 * written via callbacks.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_DEVICE_NAME);
	gatt_db_service_add_characteristic(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					BT_GATT_CHRC_PROP_READ |
					BT_GATT_CHRC_PROP_EXT_PROP,
					gap_device_name_read_cb,
					gap_device_name_write_cb,
					server);

	bt_uuid16_create(&uuid, GATT_CHARAC_EXT_PROPER_UUID);
	gatt_db_service_add_descriptor(service, &uuid, BT_ATT_PERM_READ,
					gap_device_name_ext_prop_read_cb,
					NULL, server);

	/*
	 * Appearance characteristic. Reads and writes should obtain the value
	 * from the database.
	 */
	bt_uuid16_create(&uuid, GATT_CHARAC_APPEARANCE);
	tmp = gatt_db_service_add_characteristic(service, &uuid,
							BT_ATT_PERM_READ,
							BT_GATT_CHRC_PROP_READ,
							NULL, NULL, server);

	/*
	 * Write the appearance value to the database, since we're not using a
	 * callback.
	 */
	put_le16(128, &appearance);
	gatt_db_attribute_write(tmp, 0, (void *) &appearance,
							sizeof(appearance),
							BT_ATT_OP_WRITE_REQ,
							NULL, confirm_write,
							NULL);

	gatt_db_service_set_active(service, true);
}

static void populate_gatt_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *svc_chngd;

	/* Add the GATT service */
	bt_uuid16_create(&uuid, UUID_GATT);
	service = gatt_db_add_service(server->db, &uuid, true, 4);

	bt_uuid16_create(&uuid, GATT_CHARAC_SERVICE_CHANGED);
	svc_chngd = gatt_db_service_add_characteristic(service, &uuid,
			BT_ATT_PERM_READ,
			BT_GATT_CHRC_PROP_READ | BT_GATT_CHRC_PROP_INDICATE,
			gatt_service_changed_cb,
			NULL, server);
	server->gatt_svc_chngd_handle = gatt_db_attribute_get_handle(svc_chngd);

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
				BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
				gatt_svc_chngd_ccc_read_cb,
				gatt_svc_chngd_ccc_write_cb, server);

	gatt_db_service_set_active(service, true);
}

static void populate_hr_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *hr_msrmt, *body;
	uint8_t body_loc = 1;  /* "Chest" */

	/* Add Heart Rate Service */
	bt_uuid16_create(&uuid, UUID_HEART_RATE);
	service = gatt_db_add_service(server->db, &uuid, true, 8);
	server->hr_handle = gatt_db_attribute_get_handle(service);

	/* HR Measurement Characteristic */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_MSRMT);
	hr_msrmt = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_NONE,
						BT_GATT_CHRC_PROP_NOTIFY,
						NULL, NULL, NULL);
	server->hr_msrmt_handle = gatt_db_attribute_get_handle(hr_msrmt);

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					hr_msrmt_ccc_read_cb,
					hr_msrmt_ccc_write_cb, server);

	/*
	 * Body Sensor Location Characteristic. Make reads obtain the value from
	 * the database.
	 */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_BODY);
	body = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_READ,
						BT_GATT_CHRC_PROP_READ,
						NULL, NULL, server);
	gatt_db_attribute_write(body, 0, (void *) &body_loc, sizeof(body_loc),
							BT_ATT_OP_WRITE_REQ,
							NULL, confirm_write,
							NULL);

	/* HR Control Point Characteristic */
	bt_uuid16_create(&uuid, UUID_HEART_RATE_CTRL);
	gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_WRITE,
						BT_GATT_CHRC_PROP_WRITE,
						NULL, hr_control_point_write_cb,
						server);

	if (server->hr_visible)
		gatt_db_service_set_active(service, true);
}

static void populate_wifi_service(struct server *server)
{
	bt_uuid_t uuid;
	struct gatt_db_attribute *service, *wifi;

	printf("populate_wifi_service\n");

	/* Add Wifi Service */
	bt_uuid16_create(&uuid, UUID_WIFI);
	service = gatt_db_add_service(server->db, &uuid, true, 8);
	server->wifi_handle = gatt_db_attribute_get_handle(service);

	/* Add Wifi Characteristic */
	bt_uuid16_create(&uuid, UUID_WIFI_CHR);
	wifi = gatt_db_service_add_characteristic(service, &uuid,
						BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
						BT_GATT_CHRC_PROP_READ |
						BT_GATT_CHRC_PROP_WRITE |
						BT_GATT_CHRC_PROP_NOTIFY,
						wifi_read_cb, wifi_write_cb, server);
	server->wifi_chr_handle = gatt_db_attribute_get_handle(wifi);

	bt_uuid16_create(&uuid, GATT_CLIENT_CHARAC_CFG_UUID);
	gatt_db_service_add_descriptor(service, &uuid,
					BT_ATT_PERM_READ | BT_ATT_PERM_WRITE,
					wifi_chngd_ccc_read_cb,
					wifi_chngd_ccc_write_cb, server);

	gatt_db_service_set_active(service, true);
}

static void populate_db(struct server *server)
{
	populate_gap_service(server);
	populate_gatt_service(server);
	populate_wifi_service(server);
}

static struct server *server_create(int fd, uint16_t mtu, bool hr_visible)
{
	struct server *server;
	size_t name_len = strlen(test_device_name);

	server = new0(struct server, 1);
	if (!server) {
		fprintf(stderr, "Failed to allocate memory for server\n");
		return NULL;
	}

	server->att = bt_att_new(fd, false);
	if (!server->att) {
		fprintf(stderr, "Failed to initialze ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_set_close_on_unref(server->att, true)) {
		fprintf(stderr, "Failed to set up ATT transport layer\n");
		goto fail;
	}

	if (!bt_att_register_disconnect(server->att, att_disconnect_cb, NULL,
									NULL)) {
		fprintf(stderr, "Failed to set ATT disconnect handler\n");
		goto fail;
	}

	server->name_len = name_len + 1;
	server->device_name = malloc(name_len + 1);
	if (!server->device_name) {
		fprintf(stderr, "Failed to allocate memory for device name\n");
		goto fail;
	}

	memcpy(server->device_name, test_device_name, name_len);
	server->device_name[name_len] = '\0';

	server->fd = fd;
	server->db = gatt_db_new();
	if (!server->db) {
		fprintf(stderr, "Failed to create GATT database\n");
		goto fail;
	}

	server->gatt = bt_gatt_server_new(server->db, server->att, mtu, 0);
	if (!server->gatt) {
		fprintf(stderr, "Failed to create GATT server\n");
		goto fail;
	}

	server->hr_visible = hr_visible;

	if (verbose) {
		bt_att_set_debug(server->att, BT_ATT_DEBUG_VERBOSE,
						att_debug_cb, "att: ", NULL);
		bt_gatt_server_set_debug(server->gatt, gatt_debug_cb,
							"server: ", NULL);
	}

	/* Random seed for generating fake Heart Rate measurements */
	srand(time(NULL));

	/* bt_gatt_server already holds a reference */
	populate_db(server);

	return server;

fail:
	gatt_db_unref(server->db);
	free(server->device_name);
	bt_att_unref(server->att);
	free(server);

	return NULL;
}

static void server_destroy(struct server *server)
{
	timeout_remove(server->hr_timeout_id);
	bt_gatt_server_unref(server->gatt);
	gatt_db_unref(server->db);
}

static void usage(void)
{
	printf("btgatt-server\n");
	printf("Usage:\n\tbtgatt-server [options]\n");

	printf("Options:\n"
		"\t-i, --index <id>\t\tSpecify adapter index, e.g. hci0\n"
		"\t-m, --mtu <mtu>\t\t\tThe ATT MTU to use\n"
		"\t-s, --security-level <sec>\tSet security level (low|"
								"medium|high)\n"
		"\t-t, --type [random|public] \t The source address type\n"
		"\t-v, --verbose\t\t\tEnable extra logging\n"
		"\t-r, --heart-rate\t\tEnable Heart Rate service\n"
		"\t-h, --help\t\t\tDisplay help\n");
}

static struct option main_options[] = {
	{ "index",		1, 0, 'i' },
	{ "mtu",		1, 0, 'm' },
	{ "security-level",	1, 0, 's' },
	{ "type",		1, 0, 't' },
	{ "verbose",		0, 0, 'v' },
	{ "heart-rate",		0, 0, 'r' },
	{ "help",		0, 0, 'h' },
	{ }
};

static int l2cap_le_att_listen_and_accept(bdaddr_t *src, int sec,
							uint8_t src_type)
{
	int sk, nsk;
	struct sockaddr_l2 srcaddr, addr;
	socklen_t optlen;
	struct bt_security btsec;
	char ba[18];

	sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
	if (sk < 0) {
		perror("Failed to create L2CAP socket");
		return -1;
	}

	/* Set up source address */
	memset(&srcaddr, 0, sizeof(srcaddr));
	srcaddr.l2_family = AF_BLUETOOTH;
	srcaddr.l2_cid = htobs(ATT_CID);
	srcaddr.l2_bdaddr_type = src_type;
	bacpy(&srcaddr.l2_bdaddr, src);

	if (bind(sk, (struct sockaddr *) &srcaddr, sizeof(srcaddr)) < 0) {
		perror("Failed to bind L2CAP socket");
		goto fail;
	}

	/* Set the security level */
	memset(&btsec, 0, sizeof(btsec));
	btsec.level = sec;
	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &btsec,
							sizeof(btsec)) != 0) {
		fprintf(stderr, "Failed to set L2CAP security level\n");
		goto fail;
	}

	if (listen(sk, 10) < 0) {
		perror("Listening on socket failed");
		goto fail;
	}

	printf("Started listening on ATT channel. Waiting for connections\n");

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);
	nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
	if (nsk < 0) {
		perror("Accept failed");
		goto fail;
	}

	ba2str(&addr.l2_bdaddr, ba);
	printf("Connect from %s\n", ba);
	close(sk);

	return nsk;

fail:
	close(sk);
	return -1;
}

static void notify_usage(void)
{
	printf("Usage: notify [options] <value_handle> <value>\n"
					"Options:\n"
					"\t -i, --indicate\tSend indication\n"
					"e.g.:\n"
					"\tnotify 0x0001 00 01 00\n");
}

static struct option notify_options[] = {
	{ "indicate",	0, 0, 'i' },
	{ }
};

static bool parse_args(char *str, int expected_argc,  char **argv, int *argc)
{
	char **ap;

	for (ap = argv; (*ap = strsep(&str, " \t")) != NULL;) {
		if (**ap == '\0')
			continue;

		(*argc)++;
		ap++;

		if (*argc > expected_argc)
			return false;
	}

	return true;
}

static void conf_cb(void *user_data)
{
	PRLOG("Received confirmation\n");
}

static void cmd_notify(struct server *server, char *cmd_str)
{
	int opt, i;
	char *argvbuf[516];
	char **argv = argvbuf;
	int argc = 1;
	uint16_t handle;
	char *endptr = NULL;
	int length;
	uint8_t *value = NULL;
	bool indicate = false;

	if (!parse_args(cmd_str, 514, argv + 1, &argc)) {
		printf("Too many arguments\n");
		notify_usage();
		return;
	}

	optind = 0;
	argv[0] = "notify";
	while ((opt = getopt_long(argc, argv, "+i", notify_options,
								NULL)) != -1) {
		switch (opt) {
		case 'i':
			indicate = true;
			break;
		default:
			notify_usage();
			return;
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		notify_usage();
		return;
	}

	handle = strtol(argv[0], &endptr, 16);
	if (!endptr || *endptr != '\0' || !handle) {
		printf("Invalid handle: %s\n", argv[0]);
		return;
	}

	length = argc - 1;

	if (length > 0) {
		if (length > UINT16_MAX) {
			printf("Value too long\n");
			return;
		}

		value = malloc(length);
		if (!value) {
			printf("Failed to construct value\n");
			return;
		}

		for (i = 1; i < argc; i++) {
			if (strlen(argv[i]) != 2) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}

			value[i-1] = strtol(argv[i], &endptr, 16);
			if (endptr == argv[i] || *endptr != '\0'
							|| errno == ERANGE) {
				printf("Invalid value byte: %s\n",
								argv[i]);
				goto done;
			}
		}
	}

	if (indicate) {
		if (!bt_gatt_server_send_indication(server->gatt, handle,
							value, length,
							conf_cb, NULL, NULL))
			printf("Failed to initiate indication\n");
	} else if (!bt_gatt_server_send_notification(server->gatt, handle,
							value, length, false))
		printf("Failed to initiate notification\n");

done:
	free(value);
}

static void heart_rate_usage(void)
{
	printf("Usage: heart-rate on|off\n");
}

static void cmd_heart_rate(struct server *server, char *cmd_str)
{
	bool enable;
	uint8_t pdu[4];
	struct gatt_db_attribute *attr;

	if (!cmd_str) {
		heart_rate_usage();
		return;
	}

	if (strcmp(cmd_str, "on") == 0)
		enable = true;
	else if (strcmp(cmd_str, "off") == 0)
		enable = false;
	else {
		heart_rate_usage();
		return;
	}

	if (enable == server->hr_visible) {
		printf("Heart Rate Service already %s\n",
						enable ? "visible" : "hidden");
		return;
	}

	server->hr_visible = enable;
	attr = gatt_db_get_attribute(server->db, server->hr_handle);
	gatt_db_service_set_active(attr, server->hr_visible);
	update_hr_msrmt_simulation(server);

	if (!server->svc_chngd_enabled)
		return;

	put_le16(server->hr_handle, pdu);
	put_le16(server->hr_handle + 7, pdu + 2);

	server->hr_msrmt_enabled = false;
	update_hr_msrmt_simulation(server);

	bt_gatt_server_send_indication(server->gatt,
						server->gatt_svc_chngd_handle,
						pdu, 4, conf_cb, NULL, NULL);
}

static void print_uuid(const bt_uuid_t *uuid)
{
	char uuid_str[MAX_LEN_UUID_STR];
	bt_uuid_t uuid128;

	bt_uuid_to_uuid128(uuid, &uuid128);
	bt_uuid_to_string(&uuid128, uuid_str, sizeof(uuid_str));

	printf("%s\n", uuid_str);
}

static void print_incl(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t handle, start, end;
	struct gatt_db_attribute *service;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_incl_data(attr, &handle, &start, &end))
		return;

	service = gatt_db_get_attribute(server->db, start);
	if (!service)
		return;

	gatt_db_attribute_get_service_uuid(service, &uuid);

	printf("\t  " COLOR_GREEN "include" COLOR_OFF " - handle: "
					"0x%04x, - start: 0x%04x, end: 0x%04x,"
					"uuid: ", handle, start, end);
	print_uuid(&uuid);
}

static void print_desc(struct gatt_db_attribute *attr, void *user_data)
{
	printf("\t\t  " COLOR_MAGENTA "descr" COLOR_OFF
					" - handle: 0x%04x, uuid: ",
					gatt_db_attribute_get_handle(attr));
	print_uuid(gatt_db_attribute_get_type(attr));
}

static void print_chrc(struct gatt_db_attribute *attr, void *user_data)
{
	uint16_t handle, value_handle;
	uint8_t properties;
	uint16_t ext_prop;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_char_data(attr, &handle,
								&value_handle,
								&properties,
								&ext_prop,
								&uuid))
		return;

	printf("\t  " COLOR_YELLOW "charac" COLOR_OFF
				" - start: 0x%04x, value: 0x%04x, "
				"props: 0x%02x, ext_prop: 0x%04x, uuid: ",
				handle, value_handle, properties, ext_prop);
	print_uuid(&uuid);

	gatt_db_service_foreach_desc(attr, print_desc, NULL);
}

static void print_service(struct gatt_db_attribute *attr, void *user_data)
{
	struct server *server = user_data;
	uint16_t start, end;
	bool primary;
	bt_uuid_t uuid;

	if (!gatt_db_attribute_get_service_data(attr, &start, &end, &primary,
									&uuid))
		return;

	printf(COLOR_RED "service" COLOR_OFF " - start: 0x%04x, "
				"end: 0x%04x, type: %s, uuid: ",
				start, end, primary ? "primary" : "secondary");
	print_uuid(&uuid);

	gatt_db_service_foreach_incl(attr, print_incl, server);
	gatt_db_service_foreach_char(attr, print_chrc, NULL);

	printf("\n");
}

static void cmd_services(struct server *server, char *cmd_str)
{
	gatt_db_foreach_service(server->db, NULL, print_service, server);
}

static bool convert_sign_key(char *optarg, uint8_t key[16])
{
	int i;

	if (strlen(optarg) != 32) {
		printf("sign-key length is invalid\n");
		return false;
	}

	for (i = 0; i < 16; i++) {
		if (sscanf(optarg + (i * 2), "%2hhx", &key[i]) != 1)
			return false;
	}

	return true;
}

static void set_sign_key_usage(void)
{
	printf("Usage: set-sign-key [options]\nOptions:\n"
		"\t -c, --sign-key <remote csrk>\tRemote CSRK\n"
		"e.g.:\n"
		"\tset-sign-key -c D8515948451FEA320DC05A2E88308188\n");
}

static bool remote_counter(uint32_t *sign_cnt, void *user_data)
{
	static uint32_t cnt = 0;

	if (*sign_cnt < cnt)
		return false;

	cnt = *sign_cnt;

	return true;
}

static void cmd_set_sign_key(struct server *server, char *cmd_str)
{
	char *argv[3];
	int argc = 0;
	uint8_t key[16];

	memset(key, 0, 16);

	if (!parse_args(cmd_str, 2, argv, &argc)) {
		set_sign_key_usage();
		return;
	}

	if (argc != 2) {
		set_sign_key_usage();
		return;
	}

	if (!strcmp(argv[0], "-c") || !strcmp(argv[0], "--sign-key")) {
		if (convert_sign_key(argv[1], key))
			bt_att_set_remote_key(server->att, key, remote_counter,
									server);
	} else
		set_sign_key_usage();
}

static void cmd_help(struct server *server, char *cmd_str);

typedef void (*command_func_t)(struct server *server, char *cmd_str);

static struct {
	char *cmd;
	command_func_t func;
	char *doc;
} command[] = {
	{ "help", cmd_help, "\tDisplay help message" },
	{ "notify", cmd_notify, "\tSend handle-value notification" },
	{ "heart-rate", cmd_heart_rate, "\tHide/Unhide Heart Rate Service" },
	{ "services", cmd_services, "\tEnumerate all services" },
	{ "set-sign-key", cmd_set_sign_key,
			"\tSet remote signing key for signed write command"},
	{ }
};

static void cmd_help(struct server *server, char *cmd_str)
{
	int i;

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-15s\t%s\n", command[i].cmd, command[i].doc);
}

static void prompt_read_cb(int fd, uint32_t events, void *user_data)
{
	ssize_t read;
	size_t len = 0;
	char *line = NULL;
	char *cmd = NULL, *args;
	struct server *server = user_data;
	int i;

	if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
		mainloop_quit();
		return;
	}

	read = getline(&line, &len, stdin);
	if (read < 0) {
		free(line);
		return;
	}

	if (read <= 1) {
		cmd_help(server, NULL);
		print_prompt();
		free(line);
		return;
	}

	line[read-1] = '\0';
	args = line;

	while ((cmd = strsep(&args, " \t")))
		if (*cmd != '\0')
			break;

	if (!cmd)
		goto failed;

	for (i = 0; command[i].cmd; i++) {
		if (strcmp(command[i].cmd, cmd) == 0)
			break;
	}

	if (command[i].cmd)
		command[i].func(server, args);
	else
		fprintf(stderr, "Unknown command: %s\n", line);

failed:
	print_prompt();

	free(line);
}

static void signal_cb(int signum, void *user_data)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		mainloop_quit();
		break;
	default:
		break;
	}
}

static void index_added(uint16_t index, uint16_t len,
				const void *param, void *user_data)
{
	print("hci%u added", index);
}

static void index_removed(uint16_t index, uint16_t len,
				const void *param, void *user_data)
{
	print("hci%u removed", index);
}
static const char *typestr(uint8_t type)
{
	static const char *str[] = { "BR/EDR", "LE Public", "LE Random" };

	if (type <= BDADDR_LE_RANDOM)
		return str[type];

	return "(unknown)";
}

static void connected(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_connected *ev = param;
	uint16_t eir_len;
	char addr[18];

	if (len < sizeof(*ev)) {
		error("Invalid connected event length (%u bytes)", len);
		return;
	}

	eir_len = get_le16(&ev->eir_len);
	if (len != sizeof(*ev) + eir_len) {
		error("Invalid connected event length (%u != eir_len %u)",
								len, eir_len);
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	print("hci%u %s type %s connected eir_len %u", index, addr,
					typestr(ev->addr.type), eir_len);
}

static void disconnected(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_disconnected *ev = param;
	char addr[18];
	uint8_t reason;

	if (len < sizeof(struct mgmt_addr_info)) {
		error("Invalid disconnected event length (%u bytes)", len);
		return;
	}

	if (len < sizeof(*ev))
		reason = MGMT_DEV_DISCONN_UNKNOWN;
	else
		reason = ev->reason;

	ba2str(&ev->addr.bdaddr, addr);
	print("hci%u %s type %s disconnected with reason %u",
			index, addr, typestr(ev->addr.type), reason);
}

static void conn_failed(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_connect_failed *ev = param;
	char addr[18];

	if (len != sizeof(*ev)) {
		error("Invalid connect_failed event length (%u bytes)", len);
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	print("hci%u %s type %s connect failed (status 0x%02x, %s)",
			index, addr, typestr(ev->addr.type), ev->status,
			mgmt_errstr(ev->status));
}
static const char *settings_str[] = {
				"powered",
				"connectable",
				"fast-connectable",
				"discoverable",
				"bondable",
				"link-security",
				"ssp",
				"br/edr",
				"hs",
				"le",
				"advertising",
				"secure-conn",
				"debug-keys",
				"privacy",
				"configuration",
				"static-addr",
				"phy-configuration",
				"wide-band-speech",
				"cis-central",
				"cis-peripheral",
				"iso-broadcaster",
				"sync-receiver"
};

static const char *settings2str(uint32_t settings)
{
	static char str[256];
	unsigned i;
	int off;

	off = 0;
	str[0] = '\0';

	for (i = 0; i < NELEM(settings_str); i++) {
		if ((settings & (1 << i)) != 0)
			off += snprintf(str + off, sizeof(str) - off, "%s ",
							settings_str[i]);
	}

	return str;
}

static void new_settings(uint16_t index, uint16_t len,
					const void *param, void *user_data)
{
	const uint32_t *ev = param;

	if (len < sizeof(*ev)) {
		error("Too short new_settings event (%u)", len);
		return;
	}

	print("hci%u new_settings: %s", index, settings2str(get_le32(ev)));
}

static void flags_changed(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_device_flags_changed *ev = param;
	char addr[18];

	if (len < sizeof(*ev)) {
		error("Too small (%u bytes) %s event", len, __func__);
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	print("hci%u device_flags_changed: %s (%s)", index, addr,
							typestr(ev->addr.type));
	print("     supp: 0x%08x  curr: 0x%08x",
					ev->supported_flags, ev->current_flags);
}

static void mgmt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	print("%s%s", prefix, str);
}
static void advertising_added(uint16_t index, uint16_t len,
					const void *param, void *user_data)
{
	const struct mgmt_ev_advertising_added *ev = param;

	if (len < sizeof(*ev)) {
		error("Too small (%u bytes) advertising_added event", len);
		return;
	}

	print("hci%u advertising_added: instance %u", index, ev->instance);
}

static void advertising_removed(uint16_t index, uint16_t len,
					const void *param, void *user_data)
{
	const struct mgmt_ev_advertising_removed *ev = param;

	if (len < sizeof(*ev)) {
		error("Too small (%u bytes) advertising_removed event", len);
		return;
	}

	print("hci%u advertising_removed: instance %u", index, ev->instance);
}

static void confirm_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0) {
		error("User Confirm reply failed. status 0x%02x (%s)",
						status, mgmt_errstr(status));
	}

	print("User Confirm Reply successful");
}


static int mgmt_confirm_reply(uint16_t index, const struct mgmt_addr_info *addr)
{
	struct mgmt_cp_user_confirm_reply cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, addr, sizeof(*addr));

	return mgmt_reply(mgmt, MGMT_OP_USER_CONFIRM_REPLY, index,
				sizeof(cp), &cp, confirm_rsp, NULL, NULL);
}

static void user_confirm(uint16_t index, uint16_t len, const void *param,
							void *user_data)
{
	const struct mgmt_ev_user_confirm_request *ev = param;
	uint32_t val;
	char addr[18];

	if (len != sizeof(*ev)) {
		error("Invalid user_confirm request length (%u)", len);
		return;
	}

	ba2str(&ev->addr.bdaddr, addr);
	val = get_le32(&ev->value);

	print("hci%u %s User Confirm %06u hint %u", index, addr,
							val, ev->confirm_hint);

	if (ev->confirm_hint) {
		print("Accept pairing with %s yes", addr);
		mgmt_confirm_reply(index, &ev->addr);
	} else {
		print("Confirm value %06u for %s (yes/no)", val, addr);
		mgmt_confirm_reply(index, &ev->addr);
	}
}

static void register_mgmt_callbacks(struct mgmt *mgmt, uint16_t index)
{
	mgmt_register(mgmt, MGMT_EV_INDEX_ADDED, index, index_added,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_INDEX_REMOVED, index, index_removed,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_NEW_SETTINGS, index, new_settings,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_CONNECTED, index, connected,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_DISCONNECTED, index, disconnected,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_CONNECT_FAILED, index, conn_failed,
								NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_ADVERTISING_ADDED, index,
						advertising_added, NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_ADVERTISING_REMOVED, index,
					advertising_removed, NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_DEVICE_FLAGS_CHANGED, index,
					flags_changed, NULL, NULL);
	mgmt_register(mgmt, MGMT_EV_USER_CONFIRM_REQUEST, index, user_confirm,
							mgmt, NULL);
}

/* Wrapper to get the index and opcode to the response callback */
struct command_data {
	uint16_t id;
	uint16_t op;
	void (*callback) (uint16_t id, uint16_t op, uint8_t status,
					uint16_t len, const void *param);
};

static void cmd_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	struct command_data *data = user_data;
	printf("cmd_rsp\n");

	data->callback(data->op, data->id, status, len, param);
}

static unsigned int send_cmd(struct mgmt *mgmt, uint16_t op, uint16_t id,
				uint16_t len, const void *param,
				void (*cb)(uint16_t id, uint16_t op,
						uint8_t status, uint16_t len,
						const void *param))
{
	struct command_data *data;
	unsigned int send_id;

	data = new0(struct command_data, 1);
	if (!data)
		return 0;

	data->id = id;
	data->op = op;
	data->callback = cb;

	send_id = mgmt_send(mgmt, op, id, len, param, cmd_rsp, data, free);
	if (send_id == 0)
		free(data);

	return send_id;
}

static void cmd_add_adv(void);
static void cmd_rm_adv(void);
static void cmd_setting(uint16_t op, uint8_t val);

static void *adv_thread_func(void *arg)
{
	cmd_add_adv();
	return NULL;
}

static void setting_rsp(uint16_t op, uint16_t id, uint8_t status, uint16_t len,
							const void *param)
{
	const uint32_t *rp = param;

	if (status != 0) {
		error("%s for hci%u failed with status 0x%02x (%s)",
			mgmt_opstr(op), id, status, mgmt_errstr(status));
		goto done;
	}

	if (len < sizeof(*rp)) {
		error("Too small %s response (%u bytes)",
							mgmt_opstr(op), len);
		goto done;
	}

	print("hci%u %s:%d complete, settings: %s", id, mgmt_opstr(op), op,
						settings2str(get_le32(rp)));

	/*
	if (op == MGMT_OP_SET_LE) {
		cmd_add_adv();
		//pthread_t adv_thread;
		//pthread_create(&adv_thread, NULL, adv_thread_func, NULL);
	} else if (op == MGMT_OP_SET_POWERED) {
		cmd_setting(MGMT_OP_SET_LE, 1);
	}
	*/

done:
	return;
}

static void cmd_setting(uint16_t op, uint8_t val)
{
	int index;

	index = mgmt_index;
	if (index == MGMT_INDEX_NONE)
		index = 0;

	if (send_cmd(mgmt, op, index, sizeof(val), &val, setting_rsp) == 0) {
		error("Unable to send %s cmd", mgmt_opstr(op));
		return;
	} else {
		printf("send %s cmd ok\n", mgmt_opstr(op));
	}
}

static bool parse_bytes(char *optarg, uint8_t **bytes, size_t *len)
{
	unsigned i;

	*len = strlen(optarg);

	if (*len % 2) {
		error("Malformed data");
		return false;
	}

	*len /= 2;
	if (*len > UINT8_MAX) {
		error("Data too long");
		return false;
	}

	*bytes = malloc(*len);
	if (!*bytes) {
		error("Failed to allocate memory");
		return false;
	}

	for (i = 0; i < *len; i++) {
		if (sscanf(optarg + (i * 2), "%2hhx", *bytes + i) != 1) {
			error("Invalid data");
			free(*bytes);
			*bytes = NULL;
			return false;
		}
	}

	return true;
}

static void add_adv_rsp(uint8_t status, uint16_t len, const void *param,
								void *user_data)
{
	const struct mgmt_rp_add_advertising *rp = param;

	if (status != 0) {
		error("Add Advertising failed with status 0x%02x (%s)",
						status, mgmt_errstr(status));
		return;
	}

	if (len != sizeof(*rp)) {
		error("Invalid Add Advertising response length (%u)", len);
		return;
	}

	print("Instance added: %u", rp->instance);

	return;
}

static void rm_adv_rsp(uint8_t status, uint16_t len, const void *param,
								void *user_data)
{
	const struct mgmt_rp_remove_advertising *rp = param;

	if (status != 0) {
		error("Remove Advertising failed with status 0x%02x (%s)",
						status, mgmt_errstr(status));
	}

	if (len != sizeof(*rp)) {
		error("Invalid Remove Advertising response length (%u)", len);
	}

	print("Instance removed: %u", rp->instance);
}

static void cmd_rm_adv()
{
	struct mgmt_cp_remove_advertising cp;
	uint8_t instance;
	uint16_t index;

	instance = 1;

	index = mgmt_index;
	if (index == MGMT_INDEX_NONE)
		index = 0;

	memset(&cp, 0, sizeof(cp));

	cp.instance = instance;

	if (!mgmt_send(mgmt, MGMT_OP_REMOVE_ADVERTISING, index, sizeof(cp), &cp,
						rm_adv_rsp, NULL, NULL)) {
		error("Unable to send \"Remove Advertising\" command");
	}
}

static void io_cap_rsp(uint8_t status, uint16_t len, const void *param,
							void *user_data)
{
	if (status != 0)
		error("Could not set IO Capability with status 0x%02x (%s)",
						status, mgmt_errstr(status));
	else
		print("IO Capabilities successfully set");

}

static void cmd_io_cap(void)
{
	struct mgmt_cp_set_io_capability cp;
	uint8_t cap;
	uint16_t index;

	index = mgmt_index;
	if (index == MGMT_INDEX_NONE)
		index = 0;

	cap = 3;
	memset(&cp, 0, sizeof(cp));
	cp.io_capability = cap;

	if (mgmt_send(mgmt, MGMT_OP_SET_IO_CAPABILITY, index, sizeof(cp), &cp,
					io_cap_rsp, NULL, NULL) == 0) {
		error("Unable to send set-io-cap cmd");
	}
}

#define MAX_AD_UUID_BYTES 32
static void cmd_add_adv(void)
{
	struct mgmt_cp_add_advertising *cp = NULL;
	int opt;
	uint8_t *adv_data = NULL, *scan_rsp = NULL;
	size_t adv_len = 0, scan_rsp_len = 0;
	size_t cp_len;
	uint8_t uuids[MAX_AD_UUID_BYTES];
	size_t uuid_bytes = 0;
	uint8_t uuid_type = 0;
	uint16_t timeout = 0, duration = 0;
	uint8_t instance;
	bt_uuid_t uuid;
	bool success = false;
	bool quit = true;
	uint32_t flags = 0;
	uint16_t index;

	printf("cmd_add_adv\n");

	//add-adv -u 180d -u 180f -d 080954657374204C45 1

	if (bt_string_to_uuid(&uuid, "180d") < 0) {
		print("Invalid UUID: %s", optarg);
		goto done;
	}

	bt_uuid_to_le(&uuid, uuids + uuid_bytes);
	uuid_bytes += 2;

	if (!uuid_type)
		uuid_type = uuid.type;

	if (!parse_bytes("080954657374204C45", &adv_data, &adv_len))
		goto done;

	if (uuid_bytes)
		uuid_bytes += 2;

	instance = 1;

	index = 0;

	cp_len = sizeof(*cp) + uuid_bytes + adv_len + scan_rsp_len;
	cp = malloc0(cp_len);
	if (!cp)
		goto done;

	cp->instance = instance;

	flags = MGMT_ADV_FLAG_CONNECTABLE | MGMT_ADV_FLAG_DISCOV;
	put_le32(flags, &cp->flags);
	
	put_le16(timeout, &cp->timeout);
	
	put_le16(duration, &cp->duration);

	cp->adv_data_len = adv_len + uuid_bytes;
	cp->scan_rsp_len = scan_rsp_len;

	uuid_bytes = 0;

	if (uuid_bytes) {
		cp->data[0] = uuid_bytes - 1;
		cp->data[1] = uuid_type == SDP_UUID16 ? 0x03 : 0x07;
		memcpy(cp->data + 2, uuids, uuid_bytes - 2);
	}

	if (adv_len)
		memcpy(cp->data + uuid_bytes, adv_data, adv_len);

	if (scan_rsp_len)
		memcpy(cp->data + uuid_bytes + adv_len, scan_rsp, scan_rsp_len);

	if (!mgmt_send(mgmt, MGMT_OP_ADD_ADVERTISING, index, cp_len, cp,
						add_adv_rsp, NULL, NULL)) {
		error("Unable to send \"Add Advertising\" command");
		goto done;
	} else {
		printf("send Add Advertising command ok\n");
	}

	quit = false;

done:
	free(adv_data);
	free(scan_rsp);
	free(cp);
}

static void *mainloop_thread_func(void *arg)
{
	printf("mainloop_thread_func\n");

	mainloop_run_with_signal(signal_cb, NULL);

	return NULL;
}

int main(int argc, char *argv[])
{
	int opt;
	bdaddr_t src_addr;
	int dev_id = -1;
	int fd;
	int sec = BT_SECURITY_LOW;
	uint8_t src_type = BDADDR_LE_PUBLIC;
	uint16_t mtu = 0;
	bool hr_visible = false;
	struct server *server;

	//wait hci0 appear
	int times = 10;
	while (times-- > 0 && access("/sys/class/bluetooth/hci0", F_OK)) {
		printf("wait hci0 appear\n");
		sleep(1);
	}

	sleep(1);
	system("hciconfig hci0 up");
	sleep(1);

	dev_id = hci_devid("hci0");
	if (dev_id < 0) {
		perror("Invalid adapter");
		return EXIT_FAILURE;
	}

	if (dev_id == -1)
		bacpy(&src_addr, BDADDR_ANY);
	else if (hci_devba(dev_id, &src_addr) < 0) {
		perror("Adapter not available");
		return EXIT_FAILURE;
	}

	mainloop_init();

	mgmt = mgmt_new_default();
	if (!mgmt) {
		fprintf(stderr, "Unable to open mgmt_socket\n");
		return false;
	}

	if (getenv("MGMT_DEBUG"))
		mgmt_set_debug(mgmt, mgmt_debug, "mgmt: ", NULL);

	register_mgmt_callbacks(mgmt, mgmt_index);

	printf("register_mgmt_callbacks ok\n");

	pthread_t mainloop_thread;
	pthread_create(&mainloop_thread, NULL, mainloop_thread_func, NULL);

	sleep(1);

	cmd_setting(MGMT_OP_SET_POWERED, 0);

	sleep(1);

	cmd_setting(MGMT_OP_SET_POWERED, 1);

	sleep(1);

	cmd_setting(MGMT_OP_SET_BONDABLE, 1);

	sleep(1);

	cmd_io_cap();

	sleep(1);

	cmd_setting(MGMT_OP_SET_LE, 1);

	sleep(1);

	cmd_add_adv();

	fd = l2cap_le_att_listen_and_accept(&src_addr, sec, src_type);
	if (fd < 0) {
		fprintf(stderr, "Failed to accept L2CAP ATT connection\n");
		return EXIT_FAILURE;
	}

	server = server_create(fd, mtu, hr_visible);
	if (!server) {
		close(fd);
		return EXIT_FAILURE;
	}

	if (mainloop_add_fd(fileno(stdin),
				EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR,
				prompt_read_cb, server, NULL) < 0) {
		fprintf(stderr, "Failed to initialize console\n");
		server_destroy(server);

		return EXIT_FAILURE;
	}

	printf("Running GATT server\n");

	while (1)
		sleep(1);
	//print_prompt();

	//mainloop_run_with_signal(signal_cb, NULL);

	printf("\n\nShutting down...\n");

	server_destroy(server);

	return EXIT_SUCCESS;
}
