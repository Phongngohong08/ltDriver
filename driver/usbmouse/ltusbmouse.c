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

    /* DMA-coherent buffer for URB */
    unsigned char       *data;
    dma_addr_t           data_dma;

    int                  pipe;
    int                  interval;

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

    switch (urb->status) {
    case 0:             /* success */
        break;
    case -ECONNRESET:
    case -ENOENT:
    case -ESHUTDOWN:
        /* driver is being unloaded, stop resubmitting */
        return;
    default:
        pr_err(DRIVER_NAME ": urb error: %d\n", urb->status);
        goto resubmit;
    }

    /* parse HID Boot Mouse 3-byte report */
    pr_info(DRIVER_NAME ": btn=%02x X=%d Y=%d\n",
            data[0], (signed char)data[1], (signed char)data[2]);

    /* buttons */
    input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
    input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
    input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);

    /* relative movement */
    input_report_rel(dev, REL_X, (signed char)data[1]);
    input_report_rel(dev, REL_Y, (signed char)data[2]);

    input_sync(dev);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status)
        pr_err(DRIVER_NAME ": failed to resubmit urb: %d\n", status);
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

    /* allocate DMA-coherent data buffer */
    mouse->data = usb_alloc_coherent(udev, MOUSE_REPORT_SIZE,
                                     GFP_KERNEL, &mouse->data_dma);
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

    /* declare capabilities */
    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] =
        BIT_MASK(BTN_LEFT) | BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
    input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

    /* fill URB */
    usb_fill_int_urb(mouse->urb, udev, pipe,
                     mouse->data, min(maxp, MOUSE_REPORT_SIZE),
                     ltmouse_irq, mouse, endpoint->bInterval);
    mouse->urb->transfer_dma    = mouse->data_dma;
    mouse->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    ret = input_register_device(input_dev);
    if (ret)
        goto err_free_urb;

    usb_set_intfdata(intf, mouse);

    ret = usb_submit_urb(mouse->urb, GFP_KERNEL);
    if (ret) {
        pr_err(DRIVER_NAME ": usb_submit_urb failed: %d\n", ret);
        goto err_unregister;
    }

    pr_info(DRIVER_NAME ": USB mouse connected (%s)\n", mouse->phys);
    return 0;

err_unregister:
    input_unregister_device(input_dev);
    input_dev = NULL;   /* avoid double-free: input_unregister frees it */
err_free_urb:
    usb_free_urb(mouse->urb);
err_free_buf:
    usb_free_coherent(udev, MOUSE_REPORT_SIZE, mouse->data, mouse->data_dma);
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

    usb_kill_urb(mouse->urb);
    input_unregister_device(mouse->input);
    usb_free_urb(mouse->urb);
    usb_free_coherent(mouse->udev, MOUSE_REPORT_SIZE,
                      mouse->data, mouse->data_dma);
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
