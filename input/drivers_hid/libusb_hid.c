/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2015 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libusb-1.0/libusb.h>
#include <rthreads/rthreads.h>
#include "../../driver.h"
#include "../input_hid_driver.h"

typedef struct libusb_hid
{
   libusb_hotplug_callback_handle hp;
} libusb_hid_t;

struct libusb_adapter
{
   volatile bool quitting;
   struct libusb_device *device;
   libusb_device_handle *handle;

   uint8_t manufacturer_name[255];
   uint8_t name[255];

   sthread_t *thread;
   struct libusb_adapter *next;
};

static struct libusb_adapter adapters;

static void adapter_thread(void *data)
{
   struct libusb_adapter *adapter = (struct libusb_adapter*)data;

   while (!adapter->quitting)
   {
#if 0
      static unsigned count;
      fprintf(stderr, "Gets here, count: %d\n", count++);
#endif
   }
}

static int add_adapter(struct libusb_device *dev)
{
   int rc;
   struct libusb_device_descriptor desc;
   struct libusb_adapter *old_head = NULL;
   struct libusb_adapter *adapter  = (struct libusb_adapter*)calloc(1, sizeof(struct libusb_adapter));

   if (!adapter)
   {
      fprintf(stderr, "Allocation of adapter failed.\n");
      return -1;
   }
   rc = libusb_get_device_descriptor(dev, &desc);

   if (rc != LIBUSB_SUCCESS)
   {
      fprintf(stderr, "Error getting device descriptor.\n");
      goto error;
   }

   adapter->device = dev;

   rc = libusb_open (adapter->device, &adapter->handle);

   if (rc != LIBUSB_SUCCESS)
   {
      fprintf(stderr, "Error opening device 0x%p (VID/PID: %04x:%04x).\n",
            adapter->device, desc.idVendor, desc.idProduct);
      goto error;
   }

   if (desc.iManufacturer)
   {
      libusb_get_string_descriptor_ascii(adapter->handle,
            desc.iManufacturer, adapter->manufacturer_name,
            sizeof(adapter->manufacturer_name));
      fprintf(stderr, "Adapter Manufacturer name: %s\n",
            adapter->manufacturer_name);
   }

   if (desc.iProduct)
   {
      libusb_get_string_descriptor_ascii(adapter->handle,
            desc.iProduct, adapter->name,
            sizeof(adapter->name));
      fprintf(stderr, "Adapter name: %s\n", adapter->name);
   }

   if (libusb_kernel_driver_active(adapter->handle, 0) == 1
         && libusb_detach_kernel_driver(adapter->handle, 0))
   {
      fprintf(stderr, "Error detaching handle 0x%p from kernel.\n", adapter->handle);
      goto error;
   }

   fprintf(stderr, "Device 0x%p attached (VID/PID: %04x:%04x).\n",
         adapter->device, desc.idVendor, desc.idProduct);

   adapter->thread = sthread_create(adapter_thread, adapter);

   if (!adapter->thread)
   {
      fprintf(stderr, "Error initializing adapter thread.\n");
      goto error;
   }

   old_head      = adapters.next;
   adapters.next = adapter;
   adapter->next = old_head;

   return 0;

error:
   if (adapter->thread)
   {
      sthread_join(adapter->thread);
   }
   if (adapter)
      free(adapter);
   return -1;
}

static int remove_adapter(struct libusb_device *dev)
{
   struct libusb_adapter *adapter = (struct libusb_adapter*)&adapters;

   while (adapter->next != NULL)
   {
      if (adapter->next->device == dev)
      {
         fprintf(stderr, "Device 0x%p disconnected.\n", adapter->next->device);

         struct libusb_adapter *new_next = NULL;
         adapter->next->quitting = true;
         sthread_join(adapter->next->thread);

         libusb_close(adapter->next->handle);

         new_next = adapter->next->next;
         free(adapter->next);
         adapter->next = new_next;

         return 0;
      }

      adapter = adapter->next;
   }

   return -1;
}

static int libusb_hid_hotplug_callback(struct libusb_context *ctx,
      struct libusb_device *dev, libusb_hotplug_event event, void *user_data)
{

   switch (event)
   {
      case LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED:
         add_adapter(dev);
         break;
      case LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT:
         remove_adapter(dev);
         break;
      default:
         fprintf(stderr, "Unhandled event: %d\n", event);
         break;
   }

   return 0;
}


static bool libusb_hid_joypad_query(void *data, unsigned pad)
{
   return pad < MAX_USERS;
}

static const char *libusb_hid_joypad_name(void *data, unsigned pad)
{
   /* TODO/FIXME - implement properly */
   if (pad >= MAX_USERS)
      return NULL;

   return NULL;
}

static uint64_t libusb_hid_joypad_get_buttons(void *data, unsigned port)
{
   (void)data;
   (void)port;

   return 0;
}

static bool libusb_hid_joypad_button(void *data, unsigned port, uint16_t joykey)
{
   (void)data;
   (void)port;
   (void)joykey;

   return false;
}

static bool libusb_hid_joypad_rumble(void *data, unsigned pad,
      enum retro_rumble_effect effect, uint16_t strength)
{
   (void)data;
   (void)pad;
   (void)effect;
   (void)strength;

   return false;
}

static int16_t libusb_hid_joypad_axis(void *data, unsigned port, uint32_t joyaxis)
{
   (void)data;
   (void)port;
   (void)joyaxis;

   return 0;
}

static void libusb_hid_free(void *data)
{
   libusb_hid_t *hid = (libusb_hid_t*)data;

   while(adapters.next)
      remove_adapter(adapters.next->device);

   libusb_hotplug_deregister_callback(NULL, hid->hp);

   libusb_exit(NULL);
   if (hid)
      free(hid);
}

static void *libusb_hid_init(void)
{
   unsigned i;
   int ret, count;
   struct libusb_device **devices;
   libusb_hid_t *hid = (libusb_hid_t*)calloc(1, sizeof(*hid));

   if (!hid)
      goto error;

   ret = libusb_init(NULL);

   if (ret < 0)
      goto error;

   if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
      goto error;

   count = libusb_get_device_list(NULL, &devices);

   for (i = 0; i < count; i++)
   {
      struct libusb_device_descriptor desc;
      libusb_get_device_descriptor(devices[i], &desc);

      if (desc.idVendor > 0 && desc.idProduct > 0)
         add_adapter(devices[i]);
   }

   if (count > 0)
      libusb_free_device_list(devices, 1);

   ret = libusb_hotplug_register_callback(NULL,
         LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
         LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0, 
         LIBUSB_HOTPLUG_MATCH_ANY,
         LIBUSB_HOTPLUG_MATCH_ANY,
         LIBUSB_HOTPLUG_MATCH_ANY,
         libusb_hid_hotplug_callback,
         NULL,
         &hid->hp);

   if (ret != LIBUSB_SUCCESS)
   {
      fprintf(stderr, "Error creating a hotplug callback.\n");
      goto error;
   }

   return hid;

error:
   libusb_hid_free(hid);
   return hid;
}


static void libusb_hid_poll(void *data)
{
   (void)data;
}

hid_driver_t libusb_hid = {
   libusb_hid_init,
   libusb_hid_joypad_query,
   libusb_hid_free,
   libusb_hid_joypad_button,
   libusb_hid_joypad_get_buttons,
   libusb_hid_joypad_axis,
   libusb_hid_poll,
   libusb_hid_joypad_rumble,
   libusb_hid_joypad_name,
   "libusb",
};