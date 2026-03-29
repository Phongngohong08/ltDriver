/*
 * ltusbmouse.c — USB HID Boot Mouse Driver
 *
 * Demonstrates USB driver development: URB interrupt transfers +
 * Linux input subsystem integration.
 *
 * The driver matches USB HID Boot Mouse devices (class 3, subclass 1,
 * protocol 2) and logs every mouse event via pr_info.
 *
 * NOTE: The generic usbhid driver will claim the mouse first.
 * To test, unbind from usbhid and bind to ltusbmouse manually:
 *   echo "X-Y:1.0" | sudo tee /sys/bus/usb/drivers/usbhid/unbind
 *   echo "X-Y:1.0" | sudo tee /sys/bus/usb/drivers/ltusbmouse/bind
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/slab.h>

#define DRIVER_NAME     "ltusbmouse"
#define DRIVER_DESC     "LT USB HID Boot Mouse Driver"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ltDriver");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION("1.0");

/* HID Boot Mouse report: 3 bytes */
#define MOUSE_REPORT_SIZE   3

struct ltmouse_dev {
    struct usb_device   *udev;
    struct usb_interface *intf;
    struct input_dev    *input;
    struct urb          *urb;

    unsigned char       *data;

    int                  pipe;
    int                  interval;
    int                  pktsize;   /* actual URB transfer length (= maxp) */
    bool                 closing;   /* set before usb_kill_urb to suppress resubmit */

    char                 phys[64];
};

/* ------------------------------------------------------------------ */
/* URB completion — called in interrupt context (GFP_ATOMIC required)  */
/* ------------------------------------------------------------------ */

static void ltmouse_irq(struct urb *urb)
{
    struct ltmouse_dev *mouse = urb->context;
    unsigned char *data = mouse->data;
    struct input_dev *dev = mouse->input;
    int status;

    pr_info(DRIVER_NAME ": irq called status=%d actual_len=%d\n",
            urb->status, urb->actual_length);

    switch (urb->status) {
    case 0:             /* success — data in buffer */
        break;
    case -ENOENT:
        /* VMware's virtual HCD completes the URB with -ENOENT when
         * there is no pending mouse data (~125 ms timeout), rather
         * than keeping the URB queued like real hardware does.
         * Resubmit so we keep polling, unless we are closing. */
        if (mouse->closing)
            return;
        goto resubmit;
    case -ECONNRESET:
    case -ESHUTDOWN:
        /* device gone or driver unloading — stop */
        return;
    default:
        pr_err(DRIVER_NAME ": urb error: %d\n", urb->status);
        goto resubmit;
    }

    /* Log raw bytes to see whatever format the device actually sends */
    pr_info(DRIVER_NAME ": len=%d data=%02x %02x %02x %02x %02x %02x %02x %02x\n",
            urb->actual_length,
            data[0], data[1], data[2], data[3],
            data[4], data[5], data[6], data[7]);

    /* Try both Boot Protocol (data[0]=buttons) and Report Protocol
     * (data[0]=report_id, data[1]=buttons). Log helps identify which. */
    input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
    input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
    input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);
    input_report_rel(dev, REL_X, (signed char)data[1]);
    input_report_rel(dev, REL_Y, (signed char)data[2]);

    input_sync(dev);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        pr_err(DRIVER_NAME ": failed to resubmit urb: %d\n", status);
}

/* ------------------------------------------------------------------ */
/* input open/close — start/stop the URB when a reader appears         */
/* ------------------------------------------------------------------ */

static int ltmouse_open(struct input_dev *dev)
{
    struct ltmouse_dev *mouse = input_get_drvdata(dev);

    int err;

    mouse->closing = false;
    mouse->urb->dev = mouse->udev;
    err = usb_submit_urb(mouse->urb, GFP_KERNEL);
    if (err) {
        pr_err(DRIVER_NAME ": usb_submit_urb failed on open: %d\n", err);
        return -EIO;
    }
    pr_info(DRIVER_NAME ": input opened, urb submitted\n");
    return 0;
}

static void ltmouse_close(struct input_dev *dev)
{
    struct ltmouse_dev *mouse = input_get_drvdata(dev);

    mouse->closing = true;
    usb_kill_urb(mouse->urb);
    /* Do NOT reset closing here — usb_kill_urb synchronously drains any
     * in-flight completion, but a -ENOENT resubmit that raced in just
     * before kill could fire again immediately after.  Leaving closing=true
     * suppresses that stray completion.  ltmouse_open resets it on reuse. */
    pr_info(DRIVER_NAME ": input closed, urb killed\n");
}

/* ------------------------------------------------------------------ */
/* probe / disconnect                                                   */
/* ------------------------------------------------------------------ */

static int ltmouse_probe(struct usb_interface *intf,
                          const struct usb_device_id *id)
{
    struct usb_device *udev = interface_to_usbdev(intf);
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint = NULL;
    struct ltmouse_dev *mouse;
    struct input_dev *input_dev;
    int pipe, maxp, i, ret;

    iface_desc = intf->cur_altsetting;

    /* find the interrupt IN endpoint */
    for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *ep =
            &iface_desc->endpoint[i].desc;
        if (usb_endpoint_is_int_in(ep)) {
            endpoint = ep;
            break;
        }
    }

    if (!endpoint) {
        pr_err(DRIVER_NAME ": no interrupt IN endpoint found\n");
        return -ENODEV;
    }

    /* Re-enable endpoints disabled when the previous driver (e.g. usbhid)
     * unbound.  usb_unbind_interface() may call usb_disable_interface(),
     * which zeroes dev->ep_in[], causing usb_pipe_endpoint() → NULL →
     * usb_submit_urb() → -ENODEV.  Calling usb_set_interface() here
     * re-enables all endpoints for this alt setting before we use them. */
    ret = usb_set_interface(udev, iface_desc->desc.bInterfaceNumber,
                            iface_desc->desc.bAlternateSetting);
    if (ret)
        pr_warn(DRIVER_NAME ": usb_set_interface failed: %d (continuing)\n", ret);

    mouse = kzalloc(sizeof(*mouse), GFP_KERNEL);
    if (!mouse)
        return -ENOMEM;

    input_dev = input_allocate_device();
    if (!input_dev) {
        ret = -ENOMEM;
        goto err_free_mouse;
    }

    mouse->udev  = udev;
    mouse->intf  = intf;
    mouse->input = input_dev;

    pipe     = usb_rcvintpipe(udev, endpoint->bEndpointAddress);
    maxp     = usb_maxpacket(udev, pipe, usb_pipeout(pipe));
    mouse->pipe     = pipe;
    mouse->interval = endpoint->bInterval;
    /* Use full maxp so the HCD never sees a short-transfer overflow.
     * Boot Mouse only uses bytes 0-2 but the device may send up to maxp. */
    mouse->pktsize  = maxp;

    /* plain kmalloc — let the USB core handle DMA mapping.
     * usb_alloc_coherent + URB_NO_TRANSFER_DMA_MAP can silently
     * break on VMware's emulated host controller. */
    mouse->data = kmalloc(mouse->pktsize, GFP_KERNEL);
    if (!mouse->data) {
        ret = -ENOMEM;
        goto err_free_input;
    }

    /* allocate URB */
    mouse->urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!mouse->urb) {
        ret = -ENOMEM;
        goto err_free_buf;
    }

    /* build physical location string */
    usb_make_path(udev, mouse->phys, sizeof(mouse->phys));
    strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

    /* set up input device */
    input_dev->name       = "LT USB Mouse";
    input_dev->phys       = mouse->phys;
    usb_to_input_id(udev, &input_dev->id);
    input_dev->dev.parent = &intf->dev;
    input_set_drvdata(input_dev, mouse);
    input_dev->open  = ltmouse_open;
    input_dev->close = ltmouse_close;

    /* declare capabilities */
    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] =
        BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
    input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

    /* fill URB — use endpoint's bInterval and full maxp size.
     * Low-speed USB requires interval >= 8ms; hardcoding 1 causes
     * -EINVAL from UHCI.  Do NOT send SET_PROTOCOL: VMware's virtual
     * HID device stops sending data if the guest switches to Boot
     * Protocol; leave it in Report Protocol and accept any report. */
    usb_fill_int_urb(mouse->urb, udev, pipe,
                     mouse->data, mouse->pktsize,
                     ltmouse_irq, mouse, mouse->interval);

    ret = input_register_device(input_dev);
    if (ret)
        goto err_free_urb;

    usb_set_intfdata(intf, mouse);

    /* URB is submitted via ltmouse_open() when a reader opens the device */
    pr_info(DRIVER_NAME ": USB mouse connected (%s)\n", mouse->phys);
    return 0;

err_free_urb:
    usb_free_urb(mouse->urb);
err_free_buf:
    kfree(mouse->data);
err_free_input:
    if (input_dev)
        input_free_device(input_dev);
err_free_mouse:
    kfree(mouse);
    return ret;
}

static void ltmouse_disconnect(struct usb_interface *intf)
{
    struct ltmouse_dev *mouse = usb_get_intfdata(intf);

    usb_set_intfdata(intf, NULL);
    if (!mouse)
        return;

    mouse->closing = true;
    usb_kill_urb(mouse->urb);
    input_unregister_device(mouse->input);
    usb_free_urb(mouse->urb);
    kfree(mouse->data);
    kfree(mouse);

    pr_info(DRIVER_NAME ": USB mouse disconnected\n");
}

/* match USB HID Boot Mouse: class=0x03, subclass=0x01, protocol=0x02 */
static const struct usb_device_id ltmouse_id_table[] = {
    { USB_INTERFACE_INFO(0x03, 0x01, 0x02) },
    { }
};
MODULE_DEVICE_TABLE(usb, ltmouse_id_table);

static struct usb_driver ltmouse_driver = {
    .name       = DRIVER_NAME,
    .probe      = ltmouse_probe,
    .disconnect = ltmouse_disconnect,
    .id_table   = ltmouse_id_table,
};

module_usb_driver(ltmouse_driver);
