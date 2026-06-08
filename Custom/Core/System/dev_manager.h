#ifndef DEV_MANAGER_H
#define DEV_MANAGER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// #define offsetof(type, member) ((size_t)&((type *)0)->member)

/*Device Name List*/
#define CAMERA_DEVICE_NAME  "camera"
#define CAT1_DEVICE_NAME    "cat1"
#define CODEC_DEVICE_NAME   "codec"
#define SD_DEVICE_NAME      "sd"
#define WIFI_DEVICE_NAME    "wifi"
#define WDG_DEVICE_NAME     "wdg"
#define JPEG_DEVICE_NAME    "jpeg"
#define ENC_DEVICE_NAME     "venc"
#define DRTC_DEVICE_NAME    "rtc"
#define FLASH_DEVICE_NAME   "flash_led"
#define KEY_DEVICE_NAME     "key"
#define IND_DEVICE_NAME     "ind"
#define IND_EXT_DEVICE_NAME IND_DEVICE_NAME // "ind_ext"
#define LIGHT_DEVICE_NAME   "light"
#define BATTERY_DEVICE_NAME "battery"
#define PWR_DEVICE_NAME     "pwr"
#define STROAGE_DEVICE_NAME "flash_storage"
#define USBH_DEVICE_NAME    "usbh_video"
#define DRAW_DEVICE_NAME    "draw"
#define IO_DEVICE_NAME      "io"
#define PIR_DEVICE_NAME     "pir"
#define NN_DEVICE_NAME      "nn"

#define CAMERA_CMD_BASE       0x00010000
#define WIFI_CMD_BASE         0x00020000
#define MISC_CMD_BASE         0x00030000
#define CODEC_CMD_BASE        0x00040000
#define CAT1_CMD_BASE         0x00050000
#define ENC_CMD_BASE          0x00060000
#define JPEGC_CMD_BASE        0x00070000
#define WDG_CMD_BASE          0x00080000
#define USBH_CMD_BASE         0x000A0000
#define DRAW_CMD_BASE         0x000B0000

// Device type enumeration
typedef enum {
    DEV_TYPE_VIDEO = 0,
    DEV_TYPE_AUDIO,
    DEV_TYPE_STORAGE,
    DEV_TYPE_NET,
    DEV_TYPE_MISC,
    DEV_TYPE_AI,
    DEV_TYPE_MAX
} dev_type_t;

// Lock function type definitions
typedef void (*dev_lock_func_t)(void);
typedef void (*dev_unlock_func_t)(void);
// Iterate over all devices

// Linked list implementation
struct list_head {
    struct list_head *next, *prev;
};

// Device operation function structure
typedef struct device_operations {
    int (*init)(void *priv);
    int (*deinit)(void *priv);
    int (*open)(void *priv);
    int (*close)(void *priv);
    int (*start)(void *priv);
    int (*stop)(void *priv);
    int (*ioctl)(void *priv, unsigned int cmd, unsigned char* ubuf, unsigned long arg);
} dev_ops_t;

// Device structure
typedef struct device {
    char name[16];              // Device name
    dev_type_t type;            // Device type
    dev_ops_t *ops;             // Device operations set
    void *priv_data;            // Private data
    struct list_head list;      // Linked list node
} device_t;

typedef int (*device_callback_t)(device_t *dev, void *arg);
// Device manager structure
typedef struct device_manager {
    struct list_head devices[DEV_TYPE_MAX];  // Linked list head for each device type
    
    /* Thread safety control */
    bool thread_safe;           // Whether thread safe
    dev_lock_func_t lock;           // Lock function
    dev_unlock_func_t unlock;       // Unlock function
} dev_mgr_t;

int device_register(device_t *dev);
void device_unregister(device_t *dev);
int device_open(device_t *dev);
int device_close(device_t *dev);
int device_start(device_t *dev);
int device_stop(device_t *dev);
int device_ioctl(device_t *dev, unsigned int cmd, unsigned char* ubuf, unsigned long arg);
int device_foreach(device_callback_t callback, void *arg);
int device_foreach_type(dev_type_t type, device_callback_t callback, void *arg);
int device_count(dev_type_t type);
device_t *device_find_pattern(const char *pattern, dev_type_t type);
void device_manager_init(dev_lock_func_t lock, dev_unlock_func_t unlock);
#endif