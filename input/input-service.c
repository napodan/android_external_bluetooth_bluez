/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2005-2006  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
 
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/hidp.h>

#include <glib.h>

#include <dbus/dbus.h>

#include "dbus.h"
#include "logging.h"
#include "textfile.h"

#include "input-service.h"
#include "storage.h"

#define INPUT_PATH "/org/bluez/input"
#define INPUT_MANAGER_INTERFACE	"org.bluez.input.Manager"
#define INPUT_DEVICE_INTERFACE	"org.bluez.input.Device"
#define INPUT_ERROR_INTERFACE	"org.bluez.Error"

#define L2CAP_PSM_HIDP_CTRL		0x11
#define L2CAP_PSM_HIDP_INTR		0x13

static DBusConnection *connection = NULL;
const char *pnp_uuid = "00001200-0000-1000-8000-00805f9b34fb";
const char *hid_uuid = "00001124-0000-1000-8000-00805f9b34fb";

struct input_device {
	bdaddr_t dba;
	struct hidp_connadd_req hidp;
};

struct pending_req {
	char *adapter_path;	/* Local adapter D-Bus path */
	bdaddr_t sba;		/* Local adapter BT address */
	bdaddr_t dba;		/* Peer BT address */
	DBusConnection *conn;
	DBusMessage *msg;
	sdp_record_t *pnp_rec;
	sdp_record_t *hid_rec;
};

struct pending_connect {
	bdaddr_t sba;
	bdaddr_t dba;
	DBusConnection *conn;
	DBusMessage *msg;
};

static struct input_device *input_device_new(bdaddr_t *dba)
{
	struct input_device *idev;

	idev = malloc(sizeof(struct input_device));
	if (!idev)
		return NULL;

	memset(idev, 0, sizeof(struct input_device));

	bacpy(&idev->dba, dba);

	return idev;
}

static void input_device_free(struct input_device *idev)
{
	if (!idev)
		return;
	if (idev->hidp.rd_data)
		free(idev->hidp.rd_data);
	free(idev);
}

static struct pending_req *pending_req_new(DBusConnection *conn,
		DBusMessage *msg, const char *adapter_path,
		bdaddr_t *sba, bdaddr_t *dba)
{
	struct pending_req *pr;
	pr = malloc(sizeof(struct pending_req));
	if (!pr)
		return NULL;

	memset(pr, 0, sizeof(struct pending_req));
	pr->adapter_path = strdup(adapter_path);
	bacpy(&pr->sba, sba);
	bacpy(&pr->dba, dba);
	pr->conn = dbus_connection_ref(conn);
	pr->msg = dbus_message_ref(msg);

	return pr;
}

static void pending_req_free(struct pending_req *pr)
{
	if (!pr)
		return;
	if (pr->adapter_path)
		free(pr->adapter_path);
	if (pr->conn)
		dbus_connection_unref(pr->conn);
	if (pr->msg)
		dbus_message_unref(pr->msg);
	if (pr->pnp_rec)
		sdp_record_free(pr->pnp_rec);
	if (pr->hid_rec)
		sdp_record_free(pr->hid_rec);
	free(pr);
}

static struct pending_connect *pending_connect_new(bdaddr_t *sba, bdaddr_t *dba,
					DBusConnection *conn, DBusMessage *msg)
{
	struct pending_connect *pc;
	pc = malloc(sizeof(struct pending_connect));
	if (!pc)
		return NULL;

	memset(pc, 0, sizeof(struct pending_connect));
	bacpy(&pc->sba, sba);
	bacpy(&pc->dba, dba);
	pc->conn = dbus_connection_ref(conn);
	pc->msg = dbus_message_ref(msg);

	return pc;
}

static void pending_connect_free(struct pending_connect *pc)
{
	if (!pc)
		return;
	if (pc->conn)
		dbus_connection_unref(pc->conn);
	if (pc->msg)
		dbus_message_unref(pc->msg);
	free(pc);
}

/*
 * Common D-Bus BlueZ input error functions
 */
static DBusHandlerResult err_unknown_device(DBusConnection *conn, DBusMessage *msg)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE ".UnknownDevice",
				"Invalid device"));
}

static DBusHandlerResult err_unknown_method(DBusConnection *conn, DBusMessage *msg)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE ".UnknownMethod",
				"Unknown input method"));
}

static DBusHandlerResult err_failed(DBusConnection *conn, DBusMessage *msg,
				const char *str)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE ".Failed", str));
}

static DBusHandlerResult err_connection_failed(DBusConnection *conn,
					DBusMessage *msg, const char *str)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE".ConnectionAttemptFailed",
				str));
}

static DBusHandlerResult err_already_exists(DBusConnection *conn,
				DBusMessage *msg, const char *str)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE ".AlreadyExists", str));
}

static DBusHandlerResult err_does_not_exist(DBusConnection *conn,
				DBusMessage *msg, const char *str)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg,
				INPUT_ERROR_INTERFACE ".DoesNotExist", str));
}

static DBusHandlerResult err_generic(DBusConnection *conn, DBusMessage *msg,
				const char *name, const char *str)
{
	return send_message_and_unref(conn,
			dbus_message_new_error(msg, name, str));

}

static void extract_hid_record(sdp_record_t *rec, struct hidp_connadd_req *req)
{
	sdp_data_t *pdlist, *pdlist2;
	uint8_t attr_val;

	pdlist = sdp_data_get(rec, 0x0101);
	pdlist2 = sdp_data_get(rec, 0x0102);
	if (pdlist) {
		if (pdlist2) {
			if (strncmp(pdlist->val.str, pdlist2->val.str, 5)) {
				strncpy(req->name, pdlist2->val.str, 127);
				strcat(req->name, " ");
			}
			strncat(req->name, pdlist->val.str, 127 - strlen(req->name));
		} else
			strncpy(req->name, pdlist->val.str, 127);
	} else {
		pdlist2 = sdp_data_get(rec, 0x0100);
		if (pdlist2)
			strncpy(req->name, pdlist2->val.str, 127);
 	}
 
	pdlist = sdp_data_get(rec, 0x0201);
	req->parser = pdlist ? pdlist->val.uint16 : 0x0100;
 
	pdlist = sdp_data_get(rec, 0x0202);
	req->subclass = pdlist ? pdlist->val.uint8 : 0;

	pdlist = sdp_data_get(rec, 0x0203);
	req->country = pdlist ? pdlist->val.uint8 : 0;

	pdlist = sdp_data_get(rec, 0x0204);
	attr_val = pdlist ? pdlist->val.uint8 : 0;
	if (attr_val)
		req->flags |= (1 << HIDP_VIRTUAL_CABLE_UNPLUG);

	pdlist = sdp_data_get(rec, 0x020E);
	attr_val = pdlist ? pdlist->val.uint8 : 0;
	if (attr_val)
		req->flags |= (1 << HIDP_BOOT_PROTOCOL_MODE);

	pdlist = sdp_data_get(rec, 0x0206);
	if (pdlist) {
		pdlist = pdlist->val.dataseq;
		pdlist = pdlist->val.dataseq;
		pdlist = pdlist->next;

		req->rd_data = malloc(pdlist->unitSize);
		if (req->rd_data) {
			memcpy(req->rd_data, (unsigned char *) pdlist->val.str, pdlist->unitSize);
			req->rd_size = pdlist->unitSize;
		}
	}
}

static void extract_pnp_record(sdp_record_t *rec, struct hidp_connadd_req *req)
{
	sdp_data_t *pdlist;

	pdlist = sdp_data_get(rec, 0x0201);
	req->vendor = pdlist ? pdlist->val.uint16 : 0x0000;

	pdlist = sdp_data_get(rec, 0x0202);
	req->product = pdlist ? pdlist->val.uint16 : 0x0000;

	pdlist = sdp_data_get(rec, 0x0203);
	req->version = pdlist ? pdlist->val.uint16 : 0x0000;
}

static const char *create_input_path(uint8_t minor)
{
	static char path[48];
	char subpath[32];
	static int next_id = 0;

	switch (minor & 0xc0) {
	case 0x40:
		strcpy(subpath, "keyboard");
		break;
	case 0x80:
		strcpy(subpath, "pointing");
		break;
	case 0xc0:
		strcpy(subpath, "combo");
		break;
	default:
		subpath[0] = '\0';
		break;
	}

	if ((minor & 0x3f) && (strlen(subpath) > 0))
		strcat(subpath, "/");

	switch (minor & 0x3f) {
	case 0x00:
		break;
	case 0x01:
		strcat(subpath, "joystick");
		break;
	case 0x02:
		strcat(subpath, "gamepad");
		break;
	case 0x03:
		strcat(subpath, "remotecontrol");
		break;
	case 0x04:
		strcat(subpath, "sensing");
		break;
	case 0x05:
		strcat(subpath, "digitizertablet");
		break;
	case 0x06:
		strcat(subpath, "cardreader");
		break;
	default:
		strcat(subpath, "reserved");
		break;
	}

	snprintf(path, 48, "%s/%s%d", INPUT_PATH, subpath, next_id++);
	return path;
}

static int l2cap_connect(struct pending_connect *pc,
		unsigned short psm, GIOFunc cb)
{
	GIOChannel *io;
	struct sockaddr_l2 addr;
	struct l2cap_options opts;
	int sk, err;

	if ((sk = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)) < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.l2_family  = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, &pc->sba);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		goto failed;

	if (set_nonblocking(sk, NULL) < 0)
		goto failed;

	memset(&opts, 0, sizeof(opts));
	opts.imtu = HIDP_DEFAULT_MTU;
	opts.omtu = HIDP_DEFAULT_MTU;
	opts.flush_to = 0xffff;

	if (setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts)) < 0)
		goto failed;

	memset(&addr, 0, sizeof(addr));
	addr.l2_family  = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, &pc->dba);
	addr.l2_psm = htobs(psm);

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, FALSE);

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		if (!(errno == EAGAIN || errno == EINPROGRESS))
			goto failed;

		g_io_add_watch(io, G_IO_OUT, (GIOFunc) cb, pc);
	} else {
		cb(io, G_IO_OUT, pc);
	}

	return 0;

failed:
	err = errno;
	close(sk);
	errno = err;

	return -1;
}

static gboolean interrupt_connect_cb(GIOChannel *chan, GIOCondition cond,
			struct pending_connect *pc)
{
	struct input_device *idev;
	int ctl, isk, ret, err;
	socklen_t len;
	const char *path;

	path = dbus_message_get_path(pc->msg);
	dbus_connection_get_object_path_data(pc->conn, path, (void *) &idev);

	if (cond & G_IO_NVAL) {
		err = EHOSTDOWN;
		isk = -1;
		goto failed;
	}

	isk = g_io_channel_unix_get_fd(chan);
	idev->hidp.intr_sock = isk;
	idev->hidp.idle_to = 1800; /* 30 sec */

	len = sizeof(ret);
	if (getsockopt(isk, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		err = errno;
		error("getsockopt(SO_ERROR): %s (%d)", strerror(err), err);
		goto failed;
	}

	if (ret != 0) {
		err = ret;
		error("connect(): %s (%d)", strerror(ret), ret);
		goto failed;
	}

	ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HIDP);
	if (ctl < 0) {
		err = errno;
		error("Can't open HIDP control socket");
		goto failed;
	}
	if (ioctl(ctl, HIDPCONNADD, &idev->hidp) < 0) {
		err = errno;
		close(ctl);
		goto failed;
	}
	close(ctl);

	send_message_and_unref(pc->conn,
			dbus_message_new_method_return(pc->msg));

	pending_connect_free(pc);
	g_io_channel_unref(chan);

	return FALSE;
failed:
	if (isk > 0)
		close(isk);

	idev->hidp.intr_sock = -1;
	err_connection_failed(pc->conn, pc->msg, strerror(err));
	pending_connect_free(pc);
	g_io_channel_unref(chan);

	return FALSE;
}

static gboolean control_connect_cb(GIOChannel *chan, GIOCondition cond,
			struct pending_connect *pc)
{
	struct input_device *idev;
	int ret, csk, err;
	socklen_t len;
	const char *path;

	path = dbus_message_get_path(pc->msg);
	dbus_connection_get_object_path_data(pc->conn, path, (void *) &idev);

	if (cond & G_IO_NVAL) {
		err = EHOSTDOWN;
		csk = -1;
		goto failed;
	}

	csk = g_io_channel_unix_get_fd(chan);
	/* Set HID control channel */
	idev->hidp.ctrl_sock = csk;

	len = sizeof(ret);
	if (getsockopt(csk, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		err = errno;
		error("getsockopt(SO_ERROR): %s (%d)", strerror(err), err);
		goto failed;
	}

	if (ret != 0) {
		err = ret;
		error("connect(): %s (%d)", strerror(ret), ret);
		goto failed;
	}
	/* Connect to the HID interrupt channel */
	if (l2cap_connect(pc, L2CAP_PSM_HIDP_INTR,
			(GIOFunc) interrupt_connect_cb) < 0) {

		err = errno;
		error("L2CAP connect failed:%s (%d)", strerror(errno), errno);
		goto failed;
	}

	g_io_channel_unref(chan);
	return FALSE;

failed:
	if (csk > 0)
		close(csk);

	idev->hidp.ctrl_sock = -1;
	err_connection_failed(pc->conn, pc->msg, strerror(err));
	pending_connect_free(pc);
	g_io_channel_unref(chan);

	return FALSE;
}

static int disconnect(struct input_device *idev,  uint32_t flags)
{
	struct hidp_conndel_req req;
	struct hidp_conninfo ci;
	int ctl, err, ret = -1;

	ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HIDP);
	if (ctl < 0) {
		error("Can't open HIDP control socket");
		return -1;
	}

	memset(&ci, 0, sizeof(struct hidp_conninfo));
	bacpy(&ci.bdaddr, &idev->dba);
	if (ioctl(ctl, HIDPGETCONNINFO, &ci) < 0) {
		error("Can't retrive HID information: %s(%d)",
				strerror(errno), errno);
		goto fail;
	}

	if (ci.state != BT_CONNECTED) {
		errno = ENOTCONN;
		goto fail;
	}

	memset(&req, 0, sizeof(struct hidp_conndel_req));
	bacpy(&req.bdaddr, &idev->dba);
	req.flags = flags;
	if (ioctl(ctl, HIDPCONNDEL, &req) < 0) {
		error("Can't delete the HID device: %s(%d)",
				strerror(errno), errno);
		goto fail;
	}

	ret = 0;
fail:
	err = errno;
	close(ctl);
	errno = err;

	idev->hidp.intr_sock = -1;
	idev->hidp.ctrl_sock = -1;

	return ret;
}

static int is_connected(bdaddr_t *dba)
{
	struct hidp_conninfo ci;
	int ctl;

	ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HIDP);
	if (ctl < 0)
		return 0;

	memset(&ci, 0, sizeof(struct hidp_conninfo));
	bacpy(&ci.bdaddr, dba);
	if (ioctl(ctl, HIDPGETCONNINFO, &ci) < 0) {
		close(ctl);
		return 0;
	}

	close(ctl);

	if (ci.state != BT_CONNECTED)
		return 0;
	else
		return 1;
}

/*
 * Input Device methods
 */
static DBusHandlerResult device_connect(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	struct pending_connect *pc;

	if (is_connected(&idev->dba))
		return err_connection_failed(conn, msg, "Already connected");

	pc = pending_connect_new(BDADDR_ANY, &idev->dba, conn, msg);
	if (!pc)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (l2cap_connect(pc, L2CAP_PSM_HIDP_CTRL,
			(GIOFunc) control_connect_cb) < 0) {
		error("L2CAP connect failed: %s(%d)", strerror(errno), errno);
		pending_connect_free(pc);
		return err_connection_failed(conn, msg, strerror(errno));
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult device_disconnect(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;

	if (disconnect(idev, 0) < 0)
		return err_failed(conn, msg, strerror(errno));

	return send_message_and_unref(conn,
			dbus_message_new_method_return(msg));
}

static DBusHandlerResult device_is_connected(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	DBusMessage *reply;
	dbus_bool_t connected;

	connected = is_connected(&idev->dba);
	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_BOOLEAN, &connected,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_get_address(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	DBusMessage *reply;
	char addr[18];
	const char *paddr = addr;
	
	ba2str(&idev->dba, addr);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &paddr,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_get_name(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	DBusMessage *reply;
	const char *pname = idev->hidp.name;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &pname,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_get_product_id(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_UINT16, &idev->hidp.product,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_get_vendor_id(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_device *idev = data;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_append_args(reply,
			DBUS_TYPE_UINT16, &idev->hidp.vendor,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult device_set_timeout(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult device_message(DBusConnection *conn,
		DBusMessage *msg, void *data)
{
	const char *iface, *member;

	iface = dbus_message_get_interface(msg);
	member = dbus_message_get_member(msg);

	/* Accept messages from the input interface only */
	if (strcmp(INPUT_DEVICE_INTERFACE, iface))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(member, "Connect") == 0)
		return device_connect(conn, msg, data);

	if (strcmp(member, "Disconnect") == 0)
		return device_disconnect(conn, msg, data);

	if (strcmp(member, "IsConnected") == 0)
		return device_is_connected(conn, msg, data);

	if (strcmp(member, "GetAddress") == 0)
		return device_get_address(conn, msg, data);

	if (strcmp(member, "GetName") == 0)
		return device_get_name(conn, msg, data);

	if (strcmp(member, "GetProductId") == 0)
		return device_get_product_id(conn, msg, data);

	if (strcmp(member, "GetVendorId") == 0)
		return device_get_vendor_id(conn, msg, data);

	if (strcmp(member, "SetTimeout") == 0)
		return device_set_timeout(conn, msg, data);

	return err_unknown_method(conn, msg);
}

static void device_unregister(DBusConnection *conn, void *data)
{
	input_device_free(data);
}

/* Virtual table to handle device object path hierarchy */
static const DBusObjectPathVTable device_table = {
	.message_function = device_message,
	.unregister_function = device_unregister,
};

/*
 * Input Manager methods
 */
struct input_manager {
	bdaddr_t sba;		/* Local adapter BT address */
	GSList *paths;		/* Input registered paths */
};

static void input_manager_free(struct input_manager *mgr)
{
	if (!mgr)
		return;
	if (mgr->paths) {
		g_slist_foreach(mgr->paths, (GFunc) free, NULL);
		g_slist_free(mgr->paths);
	}
	free(mgr);
}

static int register_input_device(DBusConnection *conn,
			struct input_device *idev, const char *path)
{
	DBusMessage *msg;
	struct input_manager *mgr;

	if (!dbus_connection_register_object_path(conn,
				path, &device_table, idev)) {
		error("Input device path registration failed");
		return -1;
	}

	dbus_connection_get_object_path_data(conn, INPUT_PATH, (void *) &mgr);
	mgr->paths = g_slist_append(mgr->paths, strdup(path));

	msg = dbus_message_new_signal(INPUT_PATH,
			INPUT_MANAGER_INTERFACE, "DeviceCreated");
	if (!msg)
		return -1;

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	send_message_and_unref(conn, msg);

	info("Created input device: %s", path);

	return 0;
}

static int unregister_input_device(DBusConnection *conn, const char *path)
{
	DBusMessage *msg;

	if (!dbus_connection_unregister_object_path(conn, path)) {
		error("Input device path unregister failed");
		return -1;
	}

	msg = dbus_message_new_signal(INPUT_PATH,
			INPUT_MANAGER_INTERFACE, "DeviceRemoved");
	if (!msg)
		return -1;

	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	send_message_and_unref(conn, msg);

	return 0;
}

static int path_bdaddr_cmp(const char *path, const bdaddr_t *bdaddr)
{
	struct input_device *idev;

	if (!dbus_connection_get_object_path_data(connection, path,
				(void *) &idev))
		return -1;

	if (!idev)
		return -1;

	return bacmp(&idev->dba, bdaddr);
}

static int get_record(struct pending_req *pr, uint32_t handle,
			DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *pending;
	char addr[18];
	const char *paddr = addr;

	msg = dbus_message_new_method_call("org.bluez", pr->adapter_path,
			"org.bluez.Adapter", "GetRemoteServiceRecord");
	if (!msg)
		return -1;

	ba2str(&pr->dba, addr);
	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &paddr,
			DBUS_TYPE_UINT32, &handle,
			DBUS_TYPE_INVALID);

	if (dbus_connection_send_with_reply(pr->conn, msg, &pending, -1) == FALSE) {
		error("Can't send D-Bus message.");
		return -1;
	}

	dbus_pending_call_set_notify(pending, cb, pr, NULL);
	dbus_message_unref(msg);

	return 0;
}

static int get_handles(struct pending_req *pr, const char *uuid,
			DBusPendingCallNotifyFunction cb)
{
	DBusMessage *msg;
	DBusPendingCall *pending;
	char addr[18];
	const char *paddr = addr;

	msg  = dbus_message_new_method_call("org.bluez", pr->adapter_path,
			"org.bluez.Adapter", "GetRemoteServiceHandles");
	if (!msg)
		return -1;

	ba2str(&pr->dba, addr);
	dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &paddr,
			DBUS_TYPE_STRING, &uuid,
			DBUS_TYPE_INVALID);
	
	if (dbus_connection_send_with_reply(pr->conn, msg, &pending, -1) == FALSE) {
		error("Can't send D-Bus message.");
		return -1;
	}

	dbus_pending_call_set_notify(pending, cb, pr, NULL);
	dbus_message_unref(msg);

	return 0;
}

static void hid_record_reply(DBusPendingCall *call, void *data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	DBusMessage *pr_reply;
	struct pending_req *pr = data;
	struct input_device *idev;
	DBusError derr;
	uint8_t *rec_bin;
	const char *path;
	int len, scanned;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &rec_bin, &len,
				DBUS_TYPE_INVALID)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len == 0) {
		err_failed(pr->conn, pr->msg, "SDP error");
		goto fail;
	}

	pr->hid_rec = sdp_extract_pdu(rec_bin, &scanned);
	if (!pr->hid_rec) {
		err_failed(pr->conn, pr->msg, "HID not supported");
		goto fail;
	}

	idev = input_device_new(&pr->dba);

	extract_hid_record(pr->hid_rec, &idev->hidp);
	if (pr->pnp_rec)
		extract_pnp_record(pr->pnp_rec, &idev->hidp);

	path = create_input_path(idev->hidp.subclass);

	if (register_input_device(pr->conn, idev, path) < 0) {
		err_failed(pr->conn, pr->msg, "D-Bus path registration failed");
		input_device_free(idev);
		goto fail;
	}

	pr_reply = dbus_message_new_method_return(pr->msg);
	dbus_message_append_args(pr_reply,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);
	send_message_and_unref(pr->conn, pr_reply);

	store_device_info(&pr->sba, &pr->dba, &idev->hidp);
fail:
	pending_req_free(pr);
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static void hid_handle_reply(DBusPendingCall *call, void *data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	struct pending_req *pr = data;
	uint32_t *phandle;
	DBusError derr;
	int len;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &phandle, &len,
				DBUS_TYPE_INVALID)) {

		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len != 0) {
		if (get_record(pr, *phandle, hid_record_reply) < 0)
			error("HID record search error");
		else
			goto done;
	}
	err_failed(pr->conn, pr->msg, "SDP error");
fail:
	pending_req_free(pr);
done:
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static void pnp_record_reply(DBusPendingCall *call, void *data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	struct pending_req *pr = data;
	DBusError derr;
	uint8_t *rec_bin;
	int len;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &rec_bin, &len,
				DBUS_TYPE_INVALID)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len != 0) {
		int scanned;
		pr->pnp_rec = sdp_extract_pdu(rec_bin, &scanned);
		if (get_handles(pr, hid_uuid, hid_handle_reply) < 0)
			error("HID record search error");
		else
			goto done;
	}
	err_failed(pr->conn, pr->msg, "SDP error");
fail:
	pending_req_free(pr);
done:
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static void pnp_handle_reply(DBusPendingCall *call, void *data)
{
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	struct pending_req *pr = data;
	DBusError derr;
	uint32_t *phandle;
	int len;

	dbus_error_init(&derr);
	if (dbus_set_error_from_message(&derr, reply)) {
		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (!dbus_message_get_args(reply, &derr,
				DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &phandle, &len,
				DBUS_TYPE_INVALID)) {

		err_generic(pr->conn, pr->msg, derr.name, derr.message);
		dbus_error_free(&derr);
		goto fail;
	}

	if (len == 0) {
		/* PnP is optional: Ignore it and request the HID handle  */
		if (get_handles(pr, hid_uuid, hid_handle_reply) < 0) {
			err_failed(pr->conn, pr->msg, "SDP error");
			goto fail;
		}
	} else {
		/* Request PnP record */
		if (get_record(pr, *phandle, pnp_record_reply) < 0) {
			err_failed(pr->conn, pr->msg, "SDP error");
			goto fail;
		}
	}

	goto done;
fail:
	pending_req_free(pr);
done:
	dbus_message_unref(reply);
	dbus_pending_call_unref(call);
}

static DBusHandlerResult manager_create_device(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_manager *mgr = data;
	struct input_device *idev;
	DBusMessage *reply;
	DBusError derr;
	char adapter[18], adapter_path[32];
	const char *addr, *path;
	GSList *l;
	bdaddr_t dba;
	int dev_id;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &addr,
				DBUS_TYPE_INVALID)) {
		err_generic(conn, msg, derr.name, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	str2ba(addr, &dba);
	l = g_slist_find_custom(mgr->paths, &dba,
			(GCompareFunc) path_bdaddr_cmp);
	if (l)
		return err_already_exists(conn, msg, "Input Already exists");

	ba2str(&mgr->sba, adapter);
	dev_id = hci_devid(adapter);
	snprintf(adapter_path, 32, "/org/bluez/hci%d", dev_id);

	idev = input_device_new(&dba);
	if (!idev)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	if (get_stored_device_info(&mgr->sba, &idev->dba, &idev->hidp) < 0) {
		struct pending_req *pr;

		/* Data not found: create the input device later */
		input_device_free(idev);
		pr = pending_req_new(conn, msg, adapter_path, &mgr->sba, &dba);
		if (!pr)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		if (get_handles(pr, pnp_uuid, pnp_handle_reply) < 0) {
			pending_req_free(pr);
			return err_failed(conn, msg, "SDP error");
		}

		return DBUS_HANDLER_RESULT_HANDLED;
	}

	path = create_input_path(idev->hidp.subclass);
	if (register_input_device(conn, idev, path) < 0) {
		input_device_free(idev);
		return err_failed(conn, msg, "D-Bus path registration failed");
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply) {
		input_device_free(idev);
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	dbus_message_append_args(reply,
			DBUS_TYPE_STRING, &path,
			DBUS_TYPE_INVALID);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult manager_remove_device(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_manager *mgr = data;
	struct input_device *idev;
	DBusMessage *reply;
	DBusError derr;
	GSList *l;
	const char *path;

	dbus_error_init(&derr);
	if (!dbus_message_get_args(msg, &derr,
				DBUS_TYPE_STRING, &path,
				DBUS_TYPE_INVALID)) {
		err_generic(conn, msg, derr.name, derr.message);
		dbus_error_free(&derr);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	l = g_slist_find_custom(mgr->paths, path, (GCompareFunc) strcmp);
	if (!l)
		return err_does_not_exist(conn, msg, "Input doesn't exist");

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	/* Try disconnect */
	if (dbus_connection_get_object_path_data(conn, path, (void *) &idev) && idev)
		disconnect(idev, (1 << HIDP_VIRTUAL_CABLE_UNPLUG));

	del_stored_device_info(&mgr->sba, &idev->dba);

	if (unregister_input_device(conn, path) < 0) {
		dbus_message_unref(reply);
		return err_failed(conn, msg, "D-Bus path unregistration failed");
	}

	free(l->data);
	mgr->paths = g_slist_remove(mgr->paths, l->data);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult manager_list_devices(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	struct input_manager *mgr = data;
	DBusMessageIter iter, iter_array;
	DBusMessage *reply;
	GSList *paths;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_iter_init_append(reply, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &iter_array);

	for (paths = mgr->paths; paths != NULL; paths = paths->next) {
		const char *ppath = paths->data;
		dbus_message_iter_append_basic(&iter_array,
				DBUS_TYPE_STRING, &ppath);
	}

	dbus_message_iter_close_container(&iter, &iter_array);

	return send_message_and_unref(conn, reply);
}

static DBusHandlerResult manager_message(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	const char *path, *iface, *member;

	path = dbus_message_get_path(msg);
	iface = dbus_message_get_interface(msg);
	member = dbus_message_get_member(msg);

	/* Catching fallback paths */
	if (strcmp(INPUT_PATH, path) != 0)
		return err_unknown_device(conn, msg);

	/* Accept messages from the input manager interface only */
	if (strcmp(INPUT_MANAGER_INTERFACE, iface))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(member, "ListDevices") == 0)
		return manager_list_devices(conn, msg, data);

	if (strcmp(member, "CreateDevice") == 0)
		return manager_create_device(conn, msg, data);

	if (strcmp(member, "RemoveDevice") == 0)
		return manager_remove_device(conn, msg, data);

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void manager_unregister(DBusConnection *conn, void *data)
{
	struct input_manager *mgr = data;

	info("Unregistered manager path");

	input_manager_free(mgr);
}

/* Virtual table to handle manager object path hierarchy */
static const DBusObjectPathVTable manager_table = {
	.message_function = manager_message,
	.unregister_function = manager_unregister,
};

static void stored_input(char *key, char *value, void *data)
{
	DBusConnection *conn = data;
	struct input_device *idev;
	const char *path;
	bdaddr_t dba;

	str2ba(key, &dba);
	idev = input_device_new(&dba);
	if (parse_stored_device_info(value, &idev->hidp) < 0) {
		input_device_free(idev);
		return;
	}

	path = create_input_path(idev->hidp.subclass);
	if (register_input_device(conn, idev, path) < 0)
		input_device_free(idev);
}

static int register_stored_inputs(DBusConnection *conn, bdaddr_t *sba)
{
	char filename[PATH_MAX + 1];
	char addr[18];

	ba2str(sba, addr);
	create_name(filename, PATH_MAX, STORAGEDIR, addr, "hidd");
	textfile_foreach(filename, stored_input, conn);

	return 0;
}

int input_dbus_init(void)
{
	struct input_manager *mgr;
	bdaddr_t sba;
	int dev_id;

	connection = init_dbus(NULL, NULL, NULL);
	if (!connection)
		return -1;

	dbus_connection_set_exit_on_disconnect(connection, TRUE);

	mgr = malloc(sizeof(struct input_manager));
	memset(mgr, 0, sizeof(struct input_manager));
	/* Fallback to catch invalid device path */
	if (!dbus_connection_register_fallback(connection, INPUT_PATH,
						&manager_table, mgr)) {
		error("D-Bus failed to register %s path", INPUT_PATH);
		goto fail;
	}

	info("Registered input manager path:%s", INPUT_PATH);

	/* Set the default adapter */
	bacpy(&sba, BDADDR_ANY);
	dev_id = hci_get_route(&sba);
	if (dev_id < 0) {
		error("Bluetooth device not available");
		goto fail;
	}

	if (hci_devba(dev_id, &sba) < 0) {
		error("Can't get local adapter device info");
		goto fail;
	}

	bacpy(&mgr->sba, &sba);
	/* Register well known HID devices */
	register_stored_inputs(connection, &sba);

	return 0;
fail:
	input_manager_free(mgr);

	return -1;
}

void input_dbus_exit(void)
{
	dbus_connection_unregister_object_path(connection, INPUT_PATH);

	dbus_connection_unref(connection);
}

void internal_service(const char *identifier)
{
	DBusMessage *msg, *reply;
	const char *name = "Input Service Debug", *desc = "";

	info("Registering service");

	msg = dbus_message_new_method_call("org.bluez", "/org/bluez",
				"org.bluez.Database", "RegisterService");
	if (!msg) {
		error("Can't create service register method");
		return;
	}

	dbus_message_append_args(msg, DBUS_TYPE_STRING, &identifier,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_STRING, &desc, DBUS_TYPE_INVALID);

	reply = dbus_connection_send_with_reply_and_block(connection, msg, -1, NULL);
	if (!reply) {
		error("Can't register service");
		return;
	}

	dbus_message_unref(msg);
	dbus_message_unref(reply);

	dbus_connection_flush(connection);
}
