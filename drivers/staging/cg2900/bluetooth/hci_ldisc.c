/*
 * Copyright (C) ST-Ericsson SA 2010
 * Authors:
 * Par-Gunnar Hjalmdahl (par-gunnar.p.hjalmdahl@stericsson.com) for ST-Ericsson.
 * License terms:  GNU General Public License (GPL), version 2
 *
 * This file is a staging solution and shall be integrated into
 * /drivers/bluetooth/hci_ldisc.c.
 *
 * Original hci_ldisc.c file:
 *  Copyright (C) 2000-2001  Qualcomm Incorporated
 *  Copyright (C) 2002-2003  Maxim Krasnyansky <maxk@qualcomm.com>
 *  Copyright (C) 2004-2005  Marcel Holtmann <marcel@holtmann.org>
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>

#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/signal.h>
#include <linux/ioctl.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "hci_uart.h"

#define VERSION "2.3"

#define TTY_BREAK_ON		(-1)
#define TTY_BREAK_OFF		(0)

static bool reset;

static struct hci_uart_proto *hup[HCI_UART_MAX_PROTO];

int cg2900_hci_uart_register_proto(struct hci_uart_proto *p)
{
	if (p->id >= HCI_UART_MAX_PROTO)
		return -EINVAL;

	if (hup[p->id])
		return -EEXIST;

	hup[p->id] = p;

	return 0;
}

int cg2900_hci_uart_unregister_proto(struct hci_uart_proto *p)
{
	if (p->id >= HCI_UART_MAX_PROTO)
		return -EINVAL;

	if (!hup[p->id])
		return -EINVAL;

	hup[p->id] = NULL;

	return 0;
}

static struct hci_uart_proto *hci_uart_get_proto(unsigned int id)
{
	if (id >= HCI_UART_MAX_PROTO)
		return NULL;

	return hup[id];
}

static inline void hci_uart_tx_complete(struct hci_uart *hu, int pkt_type)
{
	struct hci_dev *hdev = hu->hdev;

	if (!hdev)
		return;

	/* Update HCI stat counters */
	switch (pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;

	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;

	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}
}

static inline struct sk_buff *hci_uart_dequeue(struct hci_uart *hu)
{
	struct sk_buff *skb = hu->tx_skb;

	if (!skb)
		skb = hu->proto->dequeue(hu);
	else
		hu->tx_skb = NULL;

	return skb;
}

int cg2900_hci_uart_tx_wakeup(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;
	struct hci_dev *hdev = hu->hdev;
	struct sk_buff *skb;

	if (test_and_set_bit(HCI_UART_SENDING, &hu->tx_state)) {
		set_bit(HCI_UART_TX_WAKEUP, &hu->tx_state);
		return 0;
	}

	BT_DBG("");

restart:
	clear_bit(HCI_UART_TX_WAKEUP, &hu->tx_state);

	while ((skb = hci_uart_dequeue(hu))) {
		int len;

		set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
		len = tty->ops->write(tty, skb->data, skb->len);
		if (hdev)
			hdev->stat.byte_tx += len;

		skb_pull(skb, len);
		if (skb->len) {
			hu->tx_skb = skb;
			break;
		}

		hci_uart_tx_complete(hu, bt_cb(skb)->pkt_type);
		kfree_skb(skb);
	}

	if (test_bit(HCI_UART_TX_WAKEUP, &hu->tx_state))
		goto restart;

	clear_bit(HCI_UART_SENDING, &hu->tx_state);
	return 0;
}

int cg2900_hci_uart_set_break(struct hci_uart *hu, bool break_on)
{
	struct tty_struct *tty = hu->tty;
	int state = TTY_BREAK_OFF;

	if (break_on)
		state = TTY_BREAK_ON;

	if (tty->ops->break_ctl)
		return tty->ops->break_ctl(tty, state);
	else
		return -EOPNOTSUPP;
}

void cg2900_hci_uart_flow_ctrl(struct hci_uart *hu, bool flow_on)
{
	if (flow_on)
		tty_unthrottle(hu->tty);
	else
		tty_throttle(hu->tty);
}

int cg2900_hci_uart_set_baudrate(struct hci_uart *hu, int baud)
{
	struct ktermios old_termios;
	struct tty_struct *tty = hu->tty;

	if (!tty->ops->set_termios)
		return -EOPNOTSUPP;

	mutex_lock(&(tty->termios_mutex));
	/* Start by storing the old termios. */
	memcpy(&old_termios, tty->termios, sizeof(old_termios));

	/*
	 * Make sure the core will not snap baudrate to something
	 * "close to" requested rate by setting the BOTHER
	 * (baud rate other) flag.
	 */
	tty->termios->c_cflag &= ~CBAUD;
	tty->termios->c_cflag |= BOTHER | (BOTHER >> IBSHIFT);
	tty_encode_baud_rate(tty, baud, baud);

	/* Finally inform the driver */
	tty->ops->set_termios(tty, &old_termios);

	mutex_unlock(&(tty->termios_mutex));

	return 0;
}

int cg2900_hci_uart_tiocmget(struct hci_uart *hu)
{
	struct tty_struct *tty = hu->tty;

	if (!tty->ops->tiocmget ||  !hu->fd)
		return -EOPNOTSUPP;

	return tty->ops->tiocmget(tty);
}

void cg2900_hci_uart_flush_buffer(struct hci_uart *hu)
{
	tty_driver_flush_buffer(hu->tty);
}

int cg2900_hci_uart_chars_in_buffer(struct hci_uart *hu)
{
	return tty_chars_in_buffer(hu->tty);
}

/* ------- Interface to HCI layer ------ */
/* Initialize device */
static int hci_uart_open(struct hci_dev *hdev)
{
	BT_DBG("%s %p", hdev->name, hdev);

	/* Nothing to do for UART driver */

	set_bit(HCI_RUNNING, &hdev->flags);

	return 0;
}

/* Reset device */
static int hci_uart_flush(struct hci_dev *hdev)
{
	struct hci_uart *hu  = hci_get_drvdata(hdev);
	struct tty_struct *tty = hu->tty;

	BT_DBG("hdev %p tty %p", hdev, tty);

	if (hu->tx_skb) {
		kfree_skb(hu->tx_skb); hu->tx_skb = NULL;
	}

	/* Flush any pending characters in the driver and discipline. */
	tty_ldisc_flush(tty);
	tty_driver_flush_buffer(tty);

	if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
		hu->proto->flush(hu);

	return 0;
}

/* Close device */
static int hci_uart_close(struct hci_dev *hdev)
{
	BT_DBG("hdev %p", hdev);

	if (!test_and_clear_bit(HCI_RUNNING, &hdev->flags))
		return 0;

	hci_uart_flush(hdev);
	hdev->flush = NULL;
	return 0;
}

/* Send frames from HCI layer */
static int hci_uart_send_frame(struct sk_buff *skb)
{
	struct hci_dev* hdev = (struct hci_dev *) skb->dev;
	struct hci_uart *hu;

	if (!hdev) {
		BT_ERR("Frame for unknown device (hdev=NULL)");
		return -ENODEV;
	}

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return -EBUSY;

	hu = hci_get_drvdata(hdev);

	BT_DBG("%s: type %d len %d", hdev->name, bt_cb(skb)->pkt_type,
	       skb->len);

	hu->proto->enqueue(hu, skb);

	hci_uart_tx_wakeup(hu);

	return 0;
}

/* ------ LDISC part ------ */
/* hci_uart_tty_open
 *
 *     Called when line discipline changed to HCI_UART.
 *
 * Arguments:
 *     tty    pointer to tty info structure
 * Return Value:
 *     0 if success, otherwise error code
 */
static int hci_uart_tty_open(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *) tty->disc_data;

	BT_DBG("tty %p", tty);

	/* FIXME: This btw is bogus, nothing requires the old ldisc to clear
	   the pointer */
	if (hu)
		return -EEXIST;

	/* Error if the tty has no write op instead of leaving an exploitable
	   hole */
	if (tty->ops->write == NULL)
		return -EOPNOTSUPP;

	hu = kzalloc(sizeof(struct hci_uart), GFP_KERNEL);
	if (!hu) {
		BT_ERR("Can't allocate control structure");
		return -ENFILE;
	}

	tty->disc_data = hu;
	hu->tty = tty;
	tty->receive_room = 65536;

	spin_lock_init(&hu->rx_lock);

	/* Flush any pending characters in the driver and line discipline. */

	/* FIXME: why is this needed. Note don't use ldisc_ref here as the
	   open path is before the ldisc is referencable */

	if (tty->ldisc->ops->flush_buffer)
		tty->ldisc->ops->flush_buffer(tty);
	tty_driver_flush_buffer(tty);

	return 0;
}

/* hci_uart_tty_close()
 *
 *    Called when the line discipline is changed to something
 *    else, the tty is closed, or the tty detects a hangup.
 */
static void hci_uart_tty_close(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *)tty->disc_data;

	BT_DBG("tty %p", tty);

	/* Detach from the tty */
	tty->disc_data = NULL;

	if (hu) {
		struct hci_dev *hdev = hu->hdev;

		if (hdev)
			hci_uart_close(hdev);

		if (test_and_clear_bit(HCI_UART_PROTO_SET, &hu->flags)) {
			hu->proto->close(hu);
			if (hdev) {
				hci_unregister_dev(hdev);
				hci_free_dev(hdev);
			}
		}
	}
}

/* hci_uart_tty_wakeup()
 *
 *    Callback for transmit wakeup. Called when low level
 *    device driver can accept more send data.
 *
 * Arguments:        tty    pointer to associated tty instance data
 * Return Value:    None
 */
static void hci_uart_tty_wakeup(struct tty_struct *tty)
{
	struct hci_uart *hu = (void *)tty->disc_data;

	BT_DBG("");

	if (!hu)
		return;

	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);

	if (tty != hu->tty)
		return;

	if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
		hu->proto->send_callback(hu);
}

/* hci_uart_tty_receive()
 *
 *     Called by tty low level driver when receive data is
 *     available.
 *
 * Arguments:  tty          pointer to tty isntance data
 *             data         pointer to received data
 *             flags        pointer to flags for data
 *             count        count of received data in bytes
 *
 * Return Value:    None
 */
static void hci_uart_tty_receive(struct tty_struct *tty, const u8 *data,
				 char *flags, int count)
{
	struct hci_uart *hu = (void *)tty->disc_data;

	if (!hu || tty != hu->tty)
		return;

	if (!test_bit(HCI_UART_PROTO_SET, &hu->flags))
		return;

	spin_lock(&hu->rx_lock);
	hu->proto->recv(hu, (void *) data, count);
	if (hu->hdev)
		hu->hdev->stat.byte_rx += count;
	spin_unlock(&hu->rx_lock);

	tty_unthrottle(tty);
}

static int hci_uart_register_dev(struct hci_uart *hu)
{
	struct hci_dev *hdev;

	BT_DBG("");

	/* Initialize and register HCI device */
	hdev = hci_alloc_dev();
	if (!hdev) {
		BT_ERR("Can't allocate HCI device");
		return -ENOMEM;
	}

	hu->hdev = hdev;

	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, hu);

	hdev->open  = hci_uart_open;
	hdev->close = hci_uart_close;
	hdev->flush = hci_uart_flush;
	hdev->send  = hci_uart_send_frame;

	if (!reset)
		set_bit(HCI_QUIRK_NO_RESET, &hdev->quirks);

	if (test_bit(HCI_UART_RAW_DEVICE, &hu->hdev_flags))
		set_bit(HCI_QUIRK_RAW_DEVICE, &hdev->quirks);

	if (hci_register_dev(hdev) < 0) {
		BT_ERR("Can't register HCI device");
		hci_free_dev(hdev);
		return -ENODEV;
	}

	return 0;
}

static int hci_uart_set_proto(struct hci_uart *hu, int id)
{
	struct hci_uart_proto *p;
	int err;

	p = hci_uart_get_proto(id);
	if (!p)
		return -EPROTONOSUPPORT;

	hu->proto = p;

	err = p->open(hu);
	if (err)
		return err;

	/*
	 * Protocol might register hdev by itself.
	 * In that case, there is no need to register it here.
	 */
	if (!hu->proto->register_hci_dev)
		return 0;

	err = hci_uart_register_dev(hu);
	if (err) {
		p->close(hu);
		return err;
	}

	return 0;
}

/* hci_uart_tty_ioctl()
 *
 *    Process IOCTL system call for the tty device.
 *
 * Arguments:
 *
 *    tty        pointer to tty instance data
 *    file       pointer to open file object for device
 *    cmd        IOCTL command code
 *    arg        argument for IOCTL call (cmd dependent)
 *
 * Return Value:    Command dependent
 */
static int hci_uart_tty_ioctl(struct tty_struct *tty, struct file * file,
					unsigned int cmd, unsigned long arg)
{
	struct hci_uart *hu = (void *)tty->disc_data;
	int err = 0;

	BT_DBG("");

	/* Verify the status of the device */
	if (!hu)
		return -EBADF;

	switch (cmd) {
	case HCIUARTSETPROTO:
		if (!test_and_set_bit(HCI_UART_PROTO_SET, &hu->flags)) {
			err = hci_uart_set_proto(hu, arg);
			if (err) {
				clear_bit(HCI_UART_PROTO_SET, &hu->flags);
				return err;
			}
			/* Keep file descriptor.*/
			hu->fd = file;
		} else
			return -EBUSY;
		break;

	case HCIUARTGETPROTO:
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
			return hu->proto->id;
		return -EUNATCH;

	case HCIUARTGETDEVICE:
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags)) {
			if (hu->hdev)
				return hu->hdev->id;
			else
				return -ENOMSG;
		}
		return -EUNATCH;

	case HCIUARTSETFLAGS:
		if (test_bit(HCI_UART_PROTO_SET, &hu->flags))
			return -EBUSY;
		hu->hdev_flags = arg;
		break;

	case HCIUARTGETFLAGS:
		return hu->hdev_flags;

	default:
		err = n_tty_ioctl_helper(tty, file, cmd, arg);
		break;
	};

	return err;
}

/*
 * We don't provide read/write/poll interface for user space.
 */
static ssize_t hci_uart_tty_read(struct tty_struct *tty, struct file *file,
					unsigned char __user *buf, size_t nr)
{
	return 0;
}

static ssize_t hci_uart_tty_write(struct tty_struct *tty, struct file *file,
					const unsigned char *data, size_t count)
{
	return 0;
}

static unsigned int hci_uart_tty_poll(struct tty_struct *tty,
					struct file *filp, poll_table *wait)
{
	return 0;
}

static int __init cg2900_hci_uart_init(void)
{
	static struct tty_ldisc_ops hci_uart_ldisc;
	int err;

	BT_INFO("HCI UART driver ver %s", VERSION);

	/* Register the tty discipline */

	memset(&hci_uart_ldisc, 0, sizeof(hci_uart_ldisc));
	hci_uart_ldisc.magic		= TTY_LDISC_MAGIC;
	hci_uart_ldisc.name		= "n_cg2900_hci";
	hci_uart_ldisc.open		= hci_uart_tty_open;
	hci_uart_ldisc.close		= hci_uart_tty_close;
	hci_uart_ldisc.read		= hci_uart_tty_read;
	hci_uart_ldisc.write		= hci_uart_tty_write;
	hci_uart_ldisc.ioctl		= hci_uart_tty_ioctl;
	hci_uart_ldisc.poll		= hci_uart_tty_poll;
	hci_uart_ldisc.receive_buf	= hci_uart_tty_receive;
	hci_uart_ldisc.write_wakeup	= hci_uart_tty_wakeup;
	hci_uart_ldisc.owner		= THIS_MODULE;

	err = tty_register_ldisc(N_CG2900_HCI, &hci_uart_ldisc);
	if (err) {
		BT_ERR("HCI line discipline registration failed. (%d)", err);
		return err;
	}

	return 0;
}

static void __exit cg2900_hci_uart_exit(void)
{
	int err;

	/* Release tty registration of line discipline */
	err = tty_unregister_ldisc(N_CG2900_HCI);
	if (err)
		BT_ERR("Can't unregister HCI line discipline (%d)", err);
}

module_init(cg2900_hci_uart_init);
module_exit(cg2900_hci_uart_exit);

module_param(reset, bool, 0644);
MODULE_PARM_DESC(reset, "Send HCI reset command on initialization");

MODULE_AUTHOR("Par-Gunnar Hjalmdahl <par-gunnar.p.hjalmdahl@stericsson.com>");
MODULE_DESCRIPTION("CG2900 Staging Bluetooth HCI UART driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS_LDISC(N_CG2900_HCI);
